#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# Safety-latch manager: watches the SDM log for a fault-safe latch, identifies
# the cause, and releases it once the cause has recovered.
#
#   this service --"release\n"--> cmd_rx release FIFO --UDP--> SDM
#
# Auto-release daemon only: no network port, no knowledge of any UI. Its whole
# operator surface is two files in /run/atl-latch.
#
# Automatic release needs ALL of: cause identified and auto-releasable, cause
# recovered, recovery held for its dwell, trailer known clear. The trailer must
# be clear for EVERY cause, because an accepted release makes the SDM run
# resetAtlStateFromPlcSafeRelease(), zeroing its occupancy state.

import argparse
import errno
import json
import os
import queue
import re
import select
import shlex
import signal
import stat
import subprocess
import sys
import threading
import time


# Mirrors safeStateLatchCauses in sdm/ccplex/ATLControl.cpp. Every auto=True
# cause is re-checked by the SDM in activeSafeLatchCauses(), the backstop behind
# this policy. The two manual causes are manual for different reasons:
# count_drift is omitted from that re-check and leaves occupancy untrustworthy;
# pss is re-checked, but its clear is unlogged so no recovery can be awaited.
class Cause:
    def __init__(self, name, onset, recovery, auto, dwell_sec=None, manual_reason=""):
        self.name = name
        self.onset = onset
        self.recovery = recovery
        self.auto = auto
        self.dwell_sec = dwell_sec          # None -> use --dwell-sec
        self.manual_reason = manual_reason


CAUSES = (
    Cause("sensors",
          re.compile(r"All safety-relevant sensors failed; entering safe hold"),
          re.compile(r"All safety-relevant sensors failed condition cleared"),
          auto=True),
    Cause("gateway_hb",
          re.compile(r"HB-PSD: gateway HB fault latched"),
          re.compile(r"HB-PSD: gateway HB recovered"),
          auto=True),
    Cause("decision_freshness",
          re.compile(r"DecisionRequest freshness timeout; entering safe hold"),
          re.compile(r"DecisionRequest freshness recovered"),
          auto=True),
    Cause("decision_sequence",
          re.compile(r"Gateway DecisionRequest sequence gap; entering safe hold"),
          re.compile(r"Gateway DecisionRequest sequence recovered"),
          auto=True),
    Cause("count_drift",
          re.compile(r"personsInTrailerCount (?:saturated at|already 0)"),
          None,
          auto=False,
          manual_reason="SDM person count is untrusted and the SDM does not re-check "
                        "this cause on release"),
    Cause("pss",
          re.compile(r"PSS is in error mode|SW_FAIL received"),
          None,
          auto=False,
          manual_reason="the PSS reported a fault, and the SDM logs nothing when it "
                        "clears, so recovery cannot be observed from here"),
)

# Every safe-hold send is logged undeduplicated, so this stays true for the life
# of the latch rather than showing only an edge.
RE_LATCH = re.compile(r"entering safe hold|entering fault-safe lockout|"
                      r"Sending decision command:\s*FAULT SAFE STATE")
RE_RELEASE_ACCEPTED = re.compile(r"PLC safe-release accepted")

# The SDM's other three answers. Without them a refusal is indistinguishable
# from a request that never arrived.
RE_RELEASE_DENIED = re.compile(r"PLC safe-release denied")
RE_DENIED_CAUSES = re.compile(r"active_causes=(\S+)")
RE_RELEASE_RATE_LIMITED = re.compile(r"rate-limited PLC safe-release retry")
RE_RELEASE_NO_LATCH = re.compile(r"PLC safe-release received while no "
                                 r"safe-state latch is active")

# The SDM answers immediately; this covers scheduling plus the ~0.5s lag of
# `docker logs` trailing atl_sdm.log.
REPLY_TIMEOUT_SEC = 5.0

# Digits are blanked before comparing two refusal messages, so a dwell countdown
# does not read as a new reason every re-check.
RE_DIGITS = re.compile(r"\d+")

# Per-pipeline sensor health. The "sensors" recovery line above is NOT proof the
# cameras are back: the SDM drops that flag as soon as the failed mask stops
# being EVERY sensor, so it fires on the first pipeline to recover. These two
# carry the per-pipeline truth and gate the release.
RE_SENSOR_FAILED = re.compile(r"safety-sensor health change\s+pipelineID=(\d+)\s+state=FAILED")
RE_SENSOR_RECOVERED = re.compile(r"safety-sensor health change\s+pipelineID=(\d+)\s+state=RECOVERED")

# A line materially older than now is --tail history: it must build state but
# never trigger a release.
RE_TIMESTAMP = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)")
LIVE_SKEW_SEC = 10.0
# Arms a source that never emits a datable line at all. Counted only while no
# line has EVER parsed, so a genuine long replay stays in replay.
UNDATED_ARM_SEC = 60.0

# Scene state. These handlers run before the SDM's `if (safeStateLatched())
# return;` gate, so they keep arriving while latched: live evidence, not a
# stale snapshot.
RE_FORKLIFT_IN = re.compile(r"Forklift entered trailer")
RE_FORKLIFT_OUT = re.compile(r"Forklift exited trailer")
RE_PERSON_COUNT = re.compile(r"Person (?:entered|exited) trailer \([^)]*\), "
                             r"personsInTrailerCount=(\d+)")
RE_VIOLATION_SET = re.compile(r"Person restricted area violation set")
RE_VIOLATION_CLEARED = re.compile(r"Person restricted area violation cleared")
# Emitted only with a live Unmute decision, so it stops for the whole length of
# a latch. Still the only practical source of the person count, since entry and
# exit are logged as transitions and an empty rig produces none for days. See
# backfill_scene, which recovers it from history.
RE_SNAPSHOT = re.compile(r"forkliftInTrailer=(\d+)\s+personsInTrailer=(\d+)\s+"
                         r"restrictedAreaViolationByPerson=(\d+)")

# Proof that a stream really carries SDM output, assembled from the patterns
# above so it cannot drift from them. Undated arming requires one of these: a
# reader can emit its own error text and nothing else -- `tail -F` retries a
# file it cannot open forever, never exiting -- and arming on that silence
# reports a healthy, armed manager that is in fact blind to every latch.
RE_SDM_CONTENT = (RE_LATCH, RE_SNAPSHOT, RE_FORKLIFT_IN, RE_FORKLIFT_OUT,
                  RE_PERSON_COUNT, RE_VIOLATION_SET, RE_VIOLATION_CLEARED,
                  RE_SENSOR_FAILED, RE_SENSOR_RECOVERED, RE_RELEASE_ACCEPTED,
                  RE_RELEASE_DENIED, RE_RELEASE_RATE_LIMITED,
                  RE_RELEASE_NO_LATCH) \
    + tuple(p for c in CAUSES for p in (c.onset, c.recovery) if p is not None)

# How far back to look for that snapshot, counted from the newest end. 200k
# lines is ~50 min at 68 lines/s and costs under a second to sift.
SCENE_BACKFILL_LINES = "200000"
SCENE_BACKFILL_TIMEOUT_SEC = 20.0
# Retry pacing while --wait-source-sec waits for a source to appear, and how long
# to watch a freshly started reader for an immediate failure.
SOURCE_RETRY_SEC = 3.0
SOURCE_PROBE_SEC = 2.0
# Literal prefix of RE_SNAPSHOT, kept in step by hand. Pre-filtering with grep
# keeps a possibly-gigabyte source off the Python side.
SCENE_SNAPSHOT_KEY = "forkliftInTrailer="

_running = True


def _on_signal(_signum, _frame):
    """Ask every loop to finish, so shutdown runs main's cleanup.

    # Just set a flag—nothing else is safe here. Otherwise, exit paths aren't reached.
    """
    global _running
    _running = False


# Label column for the startup lines, so their values line up. Shared because
# those lines are printed from three different places.
_LBL = 15


def _log(msg, kind="info"):
    """Print one manager line. kind='warn' routes it to stderr.

    Clock only: the date is printed once by the startup line, and repeating it
    on every line just pushes the message off to the right. For whoever runs the
    manager, not the panel: the panel shows state, and the status file carries
    state. No history is kept here.
    """
    print(f"{time.strftime('%H:%M:%S')}   {msg}", flush=True,
          file=sys.stderr if kind == "warn" else sys.stdout)


# ======================= release path (cmd_rx FIFO) =========================
# The receiver creates this FIFO itself and reads it alongside its console, so
# a release from here and one typed by a human are the same input to it. Not to
# be confused with RELEASE_FIFO, which is the panel's way in to THIS process.
RELEASE_WORD = b"release\n"
DEFAULT_CMD_RX_FIFO = "/tmp/cmd_rx_release"   # cmd_rx.cpp g_releaseFifoPath


def resolve_target(path):
    """(path, error, fatal) for the release FIFO. Re-checked per attempt.

    Absent is not fatal: the receiver creates it at startup, so the manager can
    outlive a receiver restart. Existing but not a FIFO is a launch mistake that
    waiting cannot fix.
    """
    try:
        mode = os.stat(path).st_mode
    except OSError:
        return "", (f"the receiver has not created {path} yet; start it with "
                    f"--release-fifo {path}"), False
    if not stat.S_ISFIFO(mode):
        return "", (f"{path} exists but is not a FIFO; delete it and restart "
                    "the receiver"), True
    return path, "", False


def make_releaser(args):
    """Build the callable that asks cmd_rx to send CMD_SAFE_RELEASE_REQUEST.

    True only once the word reached the receiver, so callers do not spend the
    retry budget on one that is not there.
    """
    def _release():
        path, err, _ = resolve_target(args.release_fifo)
        if args.dry_run:
            _log(f"[dry-run] would write 'release' to {path or '?'}"
                 + (f" ({err})" if err else ""))
            return True
        if err:
            _log(f"cannot release: {err}", "warn")
            return False
        try:
            # O_NONBLOCK: a reader that vanished since the check above fails
            # fast with ENXIO instead of blocking the tick loop.
            fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
        except OSError as exc:
            if exc.errno == errno.ENXIO:
                _log(f"cannot release: nothing is reading {path}", "warn")
            else:
                _log(f"cannot release: opening {path} failed: {exc}", "warn")
            return False
        try:
            n = os.write(fd, RELEASE_WORD)
        except OSError as exc:
            if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                _log("cannot release: the receiver is not draining its input",
                     "warn")
            else:
                _log(f"cannot release: writing to {path} failed: {exc}", "warn")
            return False
        finally:
            os.close(fd)
        if n != len(RELEASE_WORD):
            # Well below PIPE_BUF, so a short write means the pipe is nearly
            # full; half a word reads as an unknown console command.
            _log(f"cannot release: only {n} of {len(RELEASE_WORD)} bytes "
                 f"reached {path}", "warn")
            return False
        # Success is announced by the caller's _await_reply, not here: two lines
        # per release saying the word went out is one line too many.
        return True
    return _release


# ======================= log follower =======================================
def build_log_command(args):
    """Turn the chosen source into a command that streams SDM log lines.

    All sources funnel through --log-cmd; nothing assumes a container runtime.
    """
    if args.log_cmd:
        return shlex.split(args.log_cmd)
    # History is replayed by default: the scene state and the latch itself may
    # both predate this process.
    if args.log_file:
        if args.tail.isdigit():
            return ["tail", "-n", args.tail, "-F", args.log_file]
        return ["tail", "-c", "+1", "-F", args.log_file]
    return ["docker", "logs", "-f", "--tail", args.tail, args.docker_container]


def history_cmd(args):
    """Command that re-reads recent history, or None if the source cannot rewind.

    A pipe or stdin is consumed once, so those have no history left to search.
    """
    if args.docker_container:
        return ["docker", "logs", "--tail", SCENE_BACKFILL_LINES,
                args.docker_container]
    if args.log_file:
        return ["tail", "-n", SCENE_BACKFILL_LINES, args.log_file]
    return None


def seek_scene_snapshot(args):
    """Latest (forklift, persons, violation) in the log's recent history.

    None if the source cannot be re-read, the scan times out, or nothing is in
    range -- all three leave the scene unknown, which the gate refuses on.
    """
    cmd = history_cmd(args)
    if cmd is None:
        return None
    src = sift = None
    try:
        src = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL)
        sift = subprocess.Popen(["grep", "-a", SCENE_SNAPSHOT_KEY],
                                stdin=src.stdout, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL)
    except OSError:
        for proc in (sift, src):
            if proc is not None:
                proc.kill()
                proc.wait()
        return None
    # Ours to close, or grep never sees the source end.
    src.stdout.close()
    try:
        out, _ = sift.communicate(timeout=SCENE_BACKFILL_TIMEOUT_SEC)
    except subprocess.TimeoutExpired:
        out = b""
    finally:
        for proc in (sift, src):
            if proc.poll() is None:
                proc.kill()
            proc.wait()
    found = None
    for line in out.decode(errors="replace").splitlines():
        m = RE_SNAPSHOT.search(line)
        if m:
            found = (bool(int(m.group(1))), int(m.group(2)),
                     bool(int(m.group(3))))
    return found


def open_log_source(args, announce=True, probe_sec=0.0):
    """Open the log source. (fh, proc), or (None, None) if it cannot be read.

    probe_sec waits for immediate failure to detect "not up yet" cases—merged
    stderr means errors appear as normal lines. Data from the wait remains buffered.
    """
    if args.stdin:
        _log("reading SDM log lines from stdin")
        return sys.stdin, None
    # `tail -F` retries forever rather than exiting, so an unreadable file would
    # otherwise hang here with no error and no stream. Absent is still fine: that
    # is what -F is for, and the file may appear later.
    if args.log_file and os.path.exists(args.log_file) \
            and not os.access(args.log_file, os.R_OK):
        _log(f"error: cannot read {args.log_file}: permission denied", "warn")
        return None, None
    cmd = build_log_command(args)
    if announce:
        _log(f"{'watching':<{_LBL}}{' '.join(cmd)}")
    # stderr merged on purpose: the SDM logs cause onsets to stderr and
    # recoveries to stdout, and both are needed.
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                bufsize=1, encoding="utf-8", errors="replace")
    except OSError as exc:
        # A missing runtime is a startup mistake, not a log line: report it as one
        # rather than letting it surface as a traceback.
        _log(f"error: cannot run the log source ({cmd[0]}): {exc}", "warn")
        return None, None
    end = time.monotonic() + probe_sec
    while proc.poll() is None and time.monotonic() < end:
        time.sleep(0.05)
    # Treat exited sources as unhealthy streams. Must repoll to update returncode.
    src_rc = proc.poll()
    if src_rc is not None:
        # Discard stream; capture log of failure reason (e.g., bad container/perm/flag).
        detail = ""
        try:
            first = proc.stdout.read(2048).strip().splitlines()
            detail = first[0][:200] if first else ""   # one line, capped: a reader
        except (OSError, ValueError):                  # may emit 2k without a newline
            pass
        proc.stdout.close()
        proc.wait()     # explicit, so reaping does not rest on poll()'s side effect
        if announce:
            # Once per wait, not once per retry: the retry loop would otherwise
            # repeat this every SOURCE_RETRY_SEC for the whole budget.
            _log(f"error: the log source failed at startup (exit {src_rc})"
                 + (f": {detail}" if detail else ""), "warn")
        return None, None
    return proc.stdout, proc


def _reader_thread(fh, q):
    try:
        for line in fh:
            q.put(("line", line))
    except (OSError, ValueError):
        pass
    finally:
        q.put(("eof", None))


def _stop_reader(proc):
    """Stop the log reader and reap it. Escalates SIGTERM to SIGKILL.

    Never raises: it is called from a finally, where an exception would replace
    the manager's exit code with a traceback.
    """
    try:
        proc.terminate()
        proc.wait(timeout=2)
        return
    except subprocess.TimeoutExpired:
        pass
    except Exception:
        return
    try:
        proc.kill()
        proc.wait(timeout=2)
    except Exception:
        pass


# ======================= release policy =====================================
# Require two presses to confirm release: within [min, max] sec.
# Too fast = double-click, re-arms; too slow = expiry, re-arms.
CONFIRM_MIN_SEC = 3.0
CONFIRM_MAX_SEC = 300.0

class LatchReleaser:
    def __init__(self, args, releaser):
        self.args = args
        self.releaser = releaser
        # Re-entrant: the manual path re-uses the gate helpers.
        self.lock = threading.RLock()
        # None means "never observed", which is not the same as clear and
        # blocks release.
        self.forklift = None
        self.persons = None
        self.violation = None
        # Survives reset(): sensor health belongs to the rig, not to one episode.
        self.failed_pipelines = set()
        self.live = False
        self._scene_backfilled = False
        self._ts_seen = ""
        self._ts_epoch = None      # None means the last stamp would not parse
        self._saw_dated = False    # any line has ever carried a usable clock
        self._saw_sdm = False      # any line was recognisably SDM output
        self._warned_blind = False
        self._replay_start = time.monotonic()
        # Survives reset(): an operator must still see why a request was refused
        # after the episode it referred to has ended.
        self._request_seq = 0
        self._last_request = None
        self.reset()

    def reset(self):
        self.latched = False
        self.latch_mono = None
        self.next_attempt = None
        self.attempts = 0
        # cause name -> monotonic time its recovery was logged, or None.
        self.causes = {}
        # True if this episode may have hidden a forklift crossing; reset by observed crossing or new latch.
        self.forklift_unverified = False
        # When the operator's first press armed the confirmation, or None.
        self._confirm_mono = None
        self.gave_up = ""       # non-empty once this episode needs a human
        self.next_nag = None
        # Set while a delivered request awaits the SDM's answer.
        self.sent_mono = None
        self._last_announce = ""   # refusal text, digits blanked

    # ---------------- observation ----------------
    def _observe_scene(self, line):
        m = RE_SNAPSHOT.search(line)
        if m:
            f, p, v = (int(g) for g in m.groups())
            self.forklift, self.persons, self.violation = bool(f), p, bool(v)
            return
        if RE_FORKLIFT_IN.search(line):
            self.forklift = True
            self.forklift_unverified = False
        elif RE_FORKLIFT_OUT.search(line):
            self.forklift = False
            self.forklift_unverified = False
        m = RE_PERSON_COUNT.search(line)
        if m:
            self.persons = int(m.group(1))
        if RE_VIOLATION_SET.search(line):
            self.violation = True
        elif RE_VIOLATION_CLEARED.search(line):
            self.violation = False

    def _note_liveness(self, line):
        """Leave replay mode once the log reaches the present.

        Only a datable, recent line arms releases: the log is full of undatable
        ones, and treating those as live drove releases from hours-old events. A
        source with no clock at all arms on going quiet, or on UNDATED_ARM_SEC.
        """
        if self.live:
            return
        m = RE_TIMESTAMP.match(line)
        if m is None:
            if not self._saw_dated and self._armable_undated():
                self.mark_live("log source has no readable timestamps")
            return
        stamp = m.group(1)
        # Memoised: replaying a large log runs this per line.
        if stamp != self._ts_seen:
            self._ts_seen = stamp
            try:
                self._ts_epoch = time.mktime(time.strptime(stamp, "%Y-%m-%d %H:%M:%S"))
                self._saw_dated = True
            except ValueError:
                self._ts_epoch = None
        # Require _saw_sdm to ensure current log is from SDM, as in _armable_undated.
        if self._ts_epoch is not None and self._saw_sdm and \
                time.time() - self._ts_epoch <= LIVE_SKEW_SEC:
            self.mark_live("log replay caught up with the present")

    def restart_replay(self):
        """Re-date the replay window to now, for a source that has just opened.

        The undated-arming timer runs from here, so time spent waiting for a
        source that was not up yet must not count towards it -- that would arm
        the gate on the first quiet moment after the source finally appears,
        which is the stale-history release note_quiet exists to prevent.
        """
        if not self.live:
            self._replay_start = time.monotonic()

    def note_quiet(self):
        """Arming path for a source that has fallen silent.

        Like _note_liveness: Only undated sources can arm on silence, since a gap in
        a dated log may just be a paused replay. Needed so undated, silent sources
        can arm after history is drained.

        """
        if self.live or self._saw_dated:
            return
        if self._armable_undated():
            self.mark_live("log source has no readable timestamps and went quiet")

    def _armable_undated(self):
        """Whether an undated source has waited out its window AND proved itself.

        The proof is the point: without it, a source that never yields one SDM
        line arms on its own silence. A blind manager stays un-armed here, and
        says so once, rather than reporting itself ready to judge releases.
        """
        if time.monotonic() - self._replay_start <= UNDATED_ARM_SEC:
            return False
        if self._saw_sdm:
            return True
        if not self._warned_blind:
            self._warned_blind = True
            _log("no recognisable SDM log lines have arrived; the source is being "
                 "read but carries nothing this manager understands. Releases stay "
                 "disarmed. Check the container name, log path and permissions",
                 "warn")
        return False

    def saw_sdm(self):
        """Whether any line so far was recognisable SDM output.

        Unlocked: written in _observe and read here, both on the thread that
        drains the reader queue.
        """
        return self._saw_sdm

    def mark_live(self, why):
        with self.lock:
            if not self.live:
                self.live = True
                _log(f"{why}; releases are now armed")

    def _trail(self, msg, kind="info"):
        """Narrate one episode event, silent while replaying history.

        Everything described is already over by the time --tail replays it.
        State still updates; only the telling waits.
        """
        if self.live:
            _log(msg, kind)

    def backfill_scene(self):
        """Fill scene fields the live log cannot supply, from its history.

        Once, after replay catches up: the snapshot line stops for the whole
        length of a latch, so a manager starting inside one would refuse every
        release. Only still-unknown fields are filled; an event already seen is
        newer evidence and wins.
        """
        if self._scene_backfilled or not self.live:
            return
        with self.lock:
            if self._scene_backfilled or not self.live:
                return
            self._scene_backfilled = True
            if None not in (self.forklift, self.persons, self.violation):
                return
        # Outside the lock: this reads the log source and can take seconds.
        found = seek_scene_snapshot(self.args)
        with self.lock:
            if found is None:
                _log("scene state is unknown and no decision snapshot was found "
                     "in the recent log; releases stay blocked until the SDM "
                     "emits one")
                return
            forklift, persons, violation = found
            filled = []
            if self.forklift is None:
                self.forklift = forklift
                filled.append(f"forklift={int(forklift)}")
            if self.persons is None:
                self.persons = persons
                filled.append(f"persons={persons}")
            if self.violation is None:
                self.violation = violation
                filled.append(f"violation={int(violation)}")
            if filled:
                _log("scene state recovered from the last logged decision: "
                     + ", ".join(filled))

    def observe(self, line):
        with self.lock:
            self._observe(line)

    def _observe(self, line):
        # Guarded, so the scan stops costing anything after the first match.
        if not self._saw_sdm and any(p.search(line) for p in RE_SDM_CONTENT):
            self._saw_sdm = True
        self._note_liveness(line)
        self._observe_scene(line)

        m = RE_SENSOR_FAILED.search(line)
        if m:
            self.failed_pipelines.add(int(m.group(1)))
        m = RE_SENSOR_RECOVERED.search(line)
        if m:
            self.failed_pipelines.discard(int(m.group(1)))

        if RE_RELEASE_ACCEPTED.search(line):
            if self.latched:
                self._trail(f"SDM ACCEPTED the safe-release{self._waited()}; latch "
                            "cleared. Re-arming.", "ok")
            # The SDM zeroes its occupancy state on an accepted release, so the
            # scene is known clear from here whatever we had recorded.
            self.forklift, self.persons, self.violation = False, 0, False
            self.reset()
            return

        if RE_RELEASE_DENIED.search(line):
            m = RE_DENIED_CAUSES.search(line)
            waited = self._waited()
            self.sent_mono = None
            self._trail(f"SDM DENIED the safe-release{waited}"
                        + (f"; it still sees active cause(s) {m.group(1)}" if m else "")
                        + ". Holding; will re-check.", "deny")
        elif RE_RELEASE_RATE_LIMITED.search(line):
            # The SDM drops repeats inside 1s and re-sends its previous answer,
            # so this means the retry cadence outran it, not that anything failed.
            self.sent_mono = None
            self._trail("SDM rate-limited the request and re-sent its previous "
                        "answer", "warn")
        elif RE_RELEASE_NO_LATCH.search(line):
            self.sent_mono = None
            self._trail("SDM had no latch to release; the request was a no-op")

        for cause in CAUSES:
            if cause.onset.search(line):
                self._note_cause(cause)
            elif cause.recovery is not None and cause.recovery.search(line):
                if cause.name in self.causes and self.causes[cause.name] is None:
                    self.causes[cause.name] = time.monotonic()
                    self._trail(f"latch cause '{cause.name}' recovered; dwelling "
                                f"{self._dwell(cause):.0f}s before release")

        if RE_LATCH.search(line) and not self.latched:
            self.latched = True
            self.latch_mono = time.monotonic()
            self.next_attempt = self.latch_mono
            self.next_nag = self.latch_mono  # announce the first refusal at once
            self._doubt_forklift()
            self._trail("safe-hold latch detected; identifying cause(s) from the "
                        "SDM log", "warn")

    def _doubt_forklift(self):
        """Mark the mirrored forklift belief unverified, voiding any pending press.

        Any cause can hide a crossing; only anchoring on the latch covers them all.
        Confirmation must be voided if doubt is raised after operator confirms—don't reuse answers.
        """
        self.forklift_unverified = True
        self._confirm_mono = None

    def _note_cause(self, cause):
        # Only an onset for a cause still pending is a duplicate. One that had
        # recovered and fires again is a new episode, and must clear the old
        # recovery stamp: kept, it spends the dwell on a fault active right now
        # and re-doubts nothing. Flapping is the normal case for a marginal
        # sensor or link, not an edge case.
        if cause.name in self.causes and self.causes[cause.name] is None:
            return
        refired = cause.name in self.causes
        # An onset can precede the generic latch line; adopt the episode here so
        # the cause is not attributed to a latch we have not yet seen.
        if not self.latched:
            self.latched = True
            self.latch_mono = time.monotonic()
            self.next_attempt = self.latch_mono
            self.next_nag = self.latch_mono
        self._doubt_forklift()
        self.causes[cause.name] = None
        if refired:
            self._trail(f"latch cause '{cause.name}' fired again after recovering; "
                        f"dwell restarts on its next recovery")
        else:
            how = "auto-releasable" if cause.auto else "MANUAL RELEASE ONLY"
            self._trail(f"latch cause identified: '{cause.name}' ({how})")
        if self.gave_up:
            # Written off as unidentifiable, and the cause has now appeared:
            # re-open and let the gate decide again from scratch.
            self._trail("cause now known; reconsidering the latch we had handed "
                        "to the operator")
            self.gave_up = ""
            self.next_attempt = time.monotonic()

    # ---------------- timing and the SDM's reply ----------------
    def _dwell(self, cause):
        return self.args.dwell_sec if cause.dwell_sec is None else cause.dwell_sec

    def _waited(self):
        """' after 1.2s' when an answer was outstanding, '' when it was not.

        Empty covers an answer to somebody else's request, which must not be
        timed as ours.
        """
        if self.sent_mono is None:
            return ""
        return f" after {time.monotonic() - self.sent_mono:.1f}s"

    def _await_reply(self):
        self.sent_mono = time.monotonic()
        _log("delivered to the command receiver; waiting for the SDM's answer",
             "send")

    def _check_reply_timeout(self, now):
        """Report a request the SDM never answered.

        The word reached the receiver, so either its console reader is gone or
        this log source is not carrying the reply. Neither is visible from the
        release path.
        """
        if self.sent_mono is None or now - self.sent_mono <= REPLY_TIMEOUT_SEC:
            return
        self.sent_mono = None
        _log(f"no answer from the SDM within {REPLY_TIMEOUT_SEC:.0f}s of the "
             "release request; check that the receiver is reading its console "
             "and that this log source carries the SDM's reply", "warn")

    # ---------------- policy ----------------
    def _occupied(self, ignore_forklift=False):
        """Reason the trailer is KNOWN to hold something, or '' if not.

        Unknown is deliberately not 'occupied': an unidentified cause passes
        through on the operator path, a known-occupied trailer never does.

        The ignore applies only to the operator path with an unverified forklift.
        The automatic path can waive nothing. Persons and violations are absolute.
        """
        if self.forklift and not ignore_forklift:
            return "forklift still IN the trailer"
        if self.persons:
            return f"{self.persons} person(s) still IN the trailer"
        if self.violation:
            return "restricted-area violation currently ACTIVE"
        return ""

    def _waivers(self):
        """Terms the operator's own look at the trailer stands in for."""
        return {"ignore_forklift": self.forklift_unverified}

    def _sensors_blind(self):
        """Reason some safety camera is still FAILED, or '' if all are healthy.

        The only guard: the SDM's own re-check fires only when ALL sensors are
        failed, so it accepts a release with two of three cameras dead.
        """
        if not self.failed_pipelines:
            return ""
        ids = ", ".join(str(p) for p in sorted(self.failed_pipelines))
        return (f"safety sensor pipeline(s) {ids} still FAILED; the SDM's "
                "'condition cleared' line only means they are not ALL failed")

    def _scene_known(self):
        """True once every scene term has evidence behind it.

        Unknown is not zero. _occupied deliberately reads an unset term as 'not
        occupied' so an unobservable cause can still pass on the operator path,
        which means an unknown count has to be paid for with a confirming second
        press instead of waved through as empty.
        """
        return None not in (self.forklift, self.persons, self.violation)

    def _scene_clear(self):
        if not self._scene_known():
            return False, ("trailer state unknown (no scene evidence in the log yet); "
                           "release would make the SDM forget any occupant")
        occupied = self._occupied()
        if occupied:
            return False, occupied
        return True, "forklift and persons are out of the trailer, no restricted-area violation"

    def _safe_to_release(self, skip_dwell=False):
        """The automatic gate. (ok, developer-facing reason)."""
        # First, because every gate below judges the scene from lines that are
        # still historical during replay.
        if not self.live:
            return False, ("still replaying the SDM log history; nothing is "
                           "released until it reaches the present")
        if not self.causes:
            return False, ("no latch cause identified in the log; cannot reason about "
                           "an unobservable cause (PSS faults log nowhere this manager "
                           "can see)")

        by_name = {c.name: c for c in CAUSES}
        manual = [n for n in self.causes if not by_name[n].auto]
        if manual:
            why = "; ".join(f"{n}: {by_name[n].manual_reason}" for n in manual)
            return False, f"cause requires a human ({why})"

        pending = [n for n, rec in self.causes.items() if rec is None]
        if pending:
            return False, f"cause(s) not recovered yet: {', '.join(sorted(pending))}"

        blind = self._sensors_blind()
        if blind:
            return False, blind

        now = time.monotonic()
        if not skip_dwell:
            for name, recovered in self.causes.items():
                remaining = (recovered + self._dwell(by_name[name])) - now
                if remaining > 0:
                    return False, f"dwelling after '{name}' recovery, {remaining:.0f}s left"

        if self.forklift_unverified:
            return False, ("forklift position not confirmed since the latch began; "
                           "waiting for an observed crossing")

        ok, why = self._scene_clear()
        if not ok:
            return False, why
        return True, f"{', '.join(sorted(self.causes))} recovered and settled, {why}"

    # ---------------- action ----------------
    def _announce(self, msg, now):
        """Log on every change of reason, otherwise once per --nag-sec.

        A stuck latch must keep saying why, or its one line scrolls away and the
        manager looks dead. Digits are blanked when comparing, so a dwell
        countdown does not read as a new reason.
        """
        key = RE_DIGITS.sub("#", msg)
        if key == self._last_announce and \
                self.next_nag is not None and now < self.next_nag:
            return
        self._last_announce = key
        _log(msg, "wait")
        self.next_nag = now + self.args.nag_sec

    def _give_up(self, reason):
        self.gave_up = reason
        self.next_attempt = None
        self.next_nag = time.monotonic() + self.args.nag_sec
        _log(f"NOT releasing automatically -- {reason}. Latch stands until an "
             "operator releases it.", "warn")

    def tick(self):
        with self.lock:
            self._tick()

    def _tick(self):
        now = time.monotonic()
        # Ahead of the latch gate: an accepted release resets the episode, so an
        # unanswered request would never be reported from inside it.
        self._check_reply_timeout(now)
        if not self.latched or not self.live:
            return

        if self.gave_up:
            held = now - (self.latch_mono or now)
            self._announce(f"latch still held after {held:.0f}s and needs an "
                           f"operator: {self.gave_up}", now)
            return

        if self.next_attempt is None or now < self.next_attempt:
            return

        safe, why = self._safe_to_release()
        if not safe:
            self.next_attempt = now + self.args.recheck_sec
            by_name = {c.name: c for c in CAUSES}
            if any(not by_name[n].auto for n in self.causes):
                self._give_up(why)                     # a human is required, now
            elif not self.causes and \
                    now - (self.latch_mono or now) >= self.args.identify_sec:
                # A cause can be logged after the first safe-hold send, so give
                # it a window to appear before calling the latch unreadable.
                self._give_up(why)
            else:
                held = now - (self.latch_mono or now)
                self._announce(f"latch held {held:.0f}s, not releasing: {why}", now)
            return

        if self.args.max_attempts and self.attempts >= self.args.max_attempts:
            self._give_up(f"the SDM refused {self.attempts} release attempts; the "
                          "latch cause is still active on its side")
            return

        held = now - (self.latch_mono or now)
        _log(f"releasing safe-hold latch, attempt {self.attempts + 1}, "
             f"held {held:.0f}s: {why}", "send")
        # Only a delivered request spends the retry budget; an unreachable
        # receiver must not consume retries reserved for SDM denials.
        if self.releaser():
            self.attempts += 1
            self._await_reply()
        self.next_attempt = now + self.args.recheck_sec

    # ---------------- operator API surface ----------------
    def snapshot(self):
        """State for the operator panel. Safe to call from any thread."""
        # Outside self.lock: it stats the FIFO, and no release decision uses it.
        _, cmd_rx_err, _ = resolve_target(self.args.release_fifo)
        with self.lock:
            now = time.monotonic()
            by_name = {c.name: c for c in CAUSES}
            causes = []
            for name in sorted(self.causes):
                cause, recovered = by_name[name], self.causes[name]
                causes.append({
                    "name": name,
                    "auto": cause.auto,
                    "recovered": recovered is not None,
                    "dwell_remaining_sec": None if recovered is None else
                        round(max(0.0, (recovered + self._dwell(cause)) - now), 1),
                    "manual_reason": cause.manual_reason,
                })
            can_release, reason = self._safe_to_release() if self.latched \
                else (False, "no latch is currently held")
            # Same predicate the press itself uses, so the panel can never invite
            # a press the manager would refuse outright. A permitted press may
            # still ask for confirmation first when the forklift reading is
            # unverified; that is a step inside the accepted flow, not a refusal.
            can_now, reason_now = self._can_operator_release()
            return {
                "latched": self.latched,
                "live": self.live,
                "held_sec": round(now - self.latch_mono, 1) if self.latch_mono else 0.0,
                "causes": causes,
                "failed_pipelines": sorted(self.failed_pipelines),
                "scene": {
                    "known": self._scene_known(),
                    "forklift_in_trailer": self.forklift,
                    "persons_in_trailer": self.persons,
                    "restricted_area_violation": self.violation,
                    "occupied_reason": self._occupied(),
                    "forklift_unverified": self.forklift_unverified,
                },
                "attempts": self.attempts,
                "max_attempts": self.args.max_attempts,
                "needs_operator": self.gave_up,
                "can_auto_release": can_release,
                "auto_reason": reason,
                "can_manual_release": can_now,
                "manual_reason": reason_now,
                # The one line the panel shows; everything else here is detail.
                "operator_note": self._operator_note(),
                "dry_run": bool(self.args.dry_run),
                "cmd_rx": self.args.release_fifo,
                # Surfaced so a receiver launched on a terminal is visible in the
                # panel and not only in this process's log.
                "cmd_rx_ready": not cmd_rx_err,
                "cmd_rx_error": cmd_rx_err,
                "manual_only_causes": [c.name for c in CAUSES if not c.auto],
                "awaiting_reply": self.sent_mono is not None,
                # Reply channel for FIFO requests; null until the first one. A
                # requester compares seq with what it read before writing.
                "last_request": self._last_request,
            }

    def handle_request(self):
        """Run an operator request and record its outcome for the status file.

        The recording is the reply channel: a requester writes to the FIFO, then
        watches last_request for a seq higher than the one it saw.
        """
        ok, reason = self.request_manual_release()
        with self.lock:
            self._request_seq += 1
            self._last_request = {
                "seq": self._request_seq,
                "at": time.strftime("%Y-%m-%d %H:%M:%S"),
                "accepted": bool(ok),
                "reason": reason,
            }
        return ok, reason

    # ---------------- operator wording ----------------
    # Operator text is written here and in the two methods below, nowhere else.
    # The gate's own reasons name causes, pipelines and dwell counters -- right
    # for whoever runs the manager, useless to the person at the trailer. Every
    # string must pass split_test.py's DEV_WORDS filter.
    def _plain_blocker(self, ignore_forklift=False):
        """Operator wording for a physical condition blocking release, or ''.

        The ignore mirrors _occupied: an unverified belief must not be quoted at
        the operator as a fact, or a refusal names the wrong thing to go and fix.
        """
        if self.forklift and not ignore_forklift:
            return "the forklift is still in the trailer"
        if self.persons:
            n = self.persons
            return f"{n} {'person is' if n == 1 else 'people are'} still in the trailer"
        if self.violation:
            return "someone is in a restricted area"
        if self.failed_pipelines:
            n = len(self.failed_pipelines)
            return f"{n} safety camera{'' if n == 1 else 's'} is not working" \
                if n == 1 else f"{n} safety cameras are not working"
        return ""

    def _operator_note(self):
        """The single line the panel shows. Caller must hold self.lock."""
        if not self.live:
            return "Starting up, checking recent history."
        if not self.latched:
            return "No safe state latch held."
        if self.sent_mono is not None:
            return "Clearing the safe state latch."
        blocker = self._plain_blocker(**self._waivers())
        if blocker:
            fix = ("Contact maintenance."
                   if self.failed_pipelines and not self._occupied(**self._waivers())
                   else "It will clear once the trailer is clear.")
            return f"Safe state held because {blocker}. {fix}"
        allowed, _ = self._can_operator_release()
        if self.gave_up and allowed:
            return ("Safe state held. It will not clear on its own \u2014 press "
                    "\u201cRelease safe state\u201d.")
        pending = [n for n, rec in self.causes.items() if rec is None]
        if pending:
            return "Safe state held by a fault that has not cleared yet. Waiting."
        if self._needs_confirm():
            if self._confirm_armed():
                return (f"Safe state held. {self._unconfirmed_check()}, then press "
                        "\u201cRelease safe state\u201d again to confirm.")
            why = self._unconfirmed_why()
            return (f"Safe state held. {why[:1].upper()}{why[1:]}. "
                    f"{self._unconfirmed_check()}, then press "
                    "\u201cRelease safe state\u201d.")
        return "Safe state held. Clearing automatically, please wait."

    def _unconfirmed_why(self):
        """Operator wording for what the fault left unconfirmed. Lower-cased.

        An unknown scene outranks an unverified forklift: it covers the people
        too, and naming only the forklift would send the operator to check less
        than the manager actually needs looked at.
        """
        if not self._scene_known():
            return "what is inside the trailer could not be confirmed after the fault"
        return "the forklift position could not be confirmed after the fault"

    def _unconfirmed_check(self):
        """What to walk over and look at, in operator wording."""
        if not self._scene_known():
            return "Check the trailer is clear of people and of the forklift"
        return "Check the trailer is clear of the forklift"

    # ---------------- the operator's press ----------------
    def _can_operator_release(self):
        """Would an operator press be accepted? (bool, reason). Holds self.lock.

        Shared with the status snapshot so the panel can never invite a press the
        manager would refuse. Occupancy and blindness are absolute and have no
        override: an accepted release makes the SDM zero its occupancy state, so
        releasing with someone inside makes the SDM forget they are there.
        """
        if not self.latched:
            return False, "There is no safe state to clear."
        if not self.live:
            return False, "Still starting up. Try again in a moment."
        if self._occupied(**self._waivers()):
            return False, (f"Cannot clear the safe state: "
                           f"{self._plain_blocker(**self._waivers())}. "
                           "Clear the trailer first.")
        if self._sensors_blind():
            return False, (f"Cannot clear the safe state: "
                           f"{self._plain_blocker(**self._waivers())}. "
                           "Contact maintenance.")
        return True, ""

    def _needs_confirm(self):
        """True when a press must be paid for twice, for terms only a look settles.

        Two of them: a forklift belief a latch may have invalidated, and a scene
        term never observed at all. Both mean the manager is passing a judgement
        it cannot make to the person who can. One predicate so the panel cannot
        invite a single press the gate would then answer with a prompt.
        """
        return self.forklift_unverified or not self._scene_known()

    def _confirm_armed(self):
        """True while a first press is still waiting for its confirming second."""
        return (self._confirm_mono is not None and
                time.monotonic() - self._confirm_mono <= CONFIRM_MAX_SEC)

    def _confirm_step(self):
        """(confirmed, operator sentence). Arms on one press, confirms the next.

        Reached only once every condition the manager can check for itself has
        passed, so all that is left is the terms it cannot vouch for and the walk
        to the trailer that settles them.
        """
        now = time.monotonic()
        if self._confirm_mono is not None:
            waited = now - self._confirm_mono
            if CONFIRM_MIN_SEC <= waited <= CONFIRM_MAX_SEC:
                self._confirm_mono = None
                return True, ""
        # Too soon, too late, or the first press: all three re-arm, so the clock
        # always runs from the most recent press and a burst can never confirm.
        self._confirm_mono = now
        why = self._unconfirmed_why()
        return False, (f"{why[:1].upper()}{why[1:]}. {self._unconfirmed_check()}, "
                       "then press again to confirm.")

    def request_manual_release(self):
        """Operator-initiated release. Returns (accepted, operator-facing reason).

        Refuses only what _can_operator_release refuses; everything past it
        releases, once _needs_confirm has been satisfied. That covers a latch the
        gate already agrees on, one with only the dwell left, and one automatic
        release can never take -- refusing the last would leave a latch nothing
        could clear, and the SDM's own activeSafeLatchCauses() re-check is the
        backstop.
        """
        with self.lock:
            allowed, refusal = self._can_operator_release()
            if not allowed:
                return False, refusal
            if self._needs_confirm():
                confirmed, ask = self._confirm_step()
                if not confirmed:
                    return False, ask
            ok, why = self._safe_to_release(skip_dwell=True)
            if not ok:
                # Gate's words to the terminal; the operator gets a sentence
                # describing what was DONE, never a refusal string.
                _log(f"operator release, no automatic path for this latch ({why})",
                     "warn")
                answer = ("Clearing the safe state on your authority \u2014 the "
                          "system could not clear this one by itself.")
            else:
                _log(f"operator requested release: {why}", "send")
                answer = "Clearing the safe state."
            if not self.releaser():
                return False, ("Cannot reach the trailer controller. "
                               "Contact maintenance.")
            self.attempts += 1
            self._await_reply()
            # Spent: the confirmation authorised this release, not the next one.
            self._confirm_mono = None
            # Asking again re-opens a latch we had handed back, so a transient
            # refusal does not strand the episode in the given-up state.
            self.gave_up = ""
            self.next_attempt = time.monotonic() + self.args.recheck_sec
            return True, answer


# ======================= operator surface ===================================
# A file for status, a pipe for the request: status is current state, where a
# reader wants the newest value and no backlog; a request is a one-off event
# that should wake the reader instantly. The answer comes back through the
# status file in last_request, so a UI can die mid-request without losing it.
#
# Paths are fixed, not configurable: a contract between this daemon and its
# panel, and a flag would only let the two disagree. The environment override is
# the test suite's seam; nothing sets it in the field.
STATE_DIR = os.environ.get("ATL_LATCH_STATE_DIR", "/run/atl-latch")
STATUS_FILE = os.path.join(STATE_DIR, "status.json")
RELEASE_FIFO = os.path.join(STATE_DIR, "release")

# Republished this often even when nothing changed, so the mtime means "alive"
# and not merely "something happened recently". Without it a quiet SDM produces
# no writes and a healthy manager looks dead.
HEARTBEAT_SEC = 2.0


def prepare_state_dir():
    """Create the state directory and the release FIFO. True on success.

    Fatal on failure, with the fix: /run is root-owned on a stock image, and
    falling back elsewhere would leave the panel watching a dead directory.
    """
    try:
        os.makedirs(STATE_DIR, mode=0o755, exist_ok=True)
    except OSError as exc:
        _log(f"error: cannot create {STATE_DIR}: {exc}", "warn")
        _log(f"       fix: sudo mkdir -p {STATE_DIR} && "
             f"sudo chown $(id -un):$(id -gn) {STATE_DIR}", "warn")
        return False
    if os.path.exists(RELEASE_FIFO) and not stat.S_ISFIFO(os.stat(RELEASE_FIFO).st_mode):
        _log(f"error: {RELEASE_FIFO} exists and is not a FIFO; "
             "delete it and restart", "warn")
        return False
    if not os.path.exists(RELEASE_FIFO):
        try:
            os.mkfifo(RELEASE_FIFO, 0o660)
        except OSError as exc:
            _log(f"error: cannot create the release FIFO {RELEASE_FIFO}: {exc}",
                 "warn")
            return False
    _log(f"{'status out':<{_LBL}}{STATUS_FILE}")
    _log(f"{'requests in':<{_LBL}}echo release > {RELEASE_FIFO}")
    return True


class StatusFile:
    """Publishes the snapshot to a file, atomically.

    os.replace() so a reader never sees half a document. Rewritten on change and
    at least every HEARTBEAT_SEC, which is what makes the mtime a liveness
    signal. Replaced rather than appended, so it never grows.
    """

    def __init__(self, path):
        self.path = path
        self._tmp = path + ".tmp"
        self._last = None
        self._last_write = 0.0

    def publish(self, snapshot):
        body = json.dumps(snapshot)
        now = time.monotonic()
        if body == self._last and now - self._last_write < HEARTBEAT_SEC:
            return
        try:
            with open(self._tmp, "w") as fh:
                fh.write(body)
                fh.flush()
                os.fsync(fh.fileno())
            os.replace(self._tmp, self.path)
        except OSError as exc:
            # Once per distinct failure, not per tick: a full or read-only
            # filesystem would otherwise bury the manager's own log.
            if self._last != "":
                _log(f"cannot publish status to {self.path}: {exc}", "warn")
            self._last = ""
            return
        self._last = body
        self._last_write = now


def release_request_thread(releaser, fifo_path):
    """Serve release requests on the FIFO until the process exits.

    O_RDWR so this process is always also a writer: a read-only reader sees EOF
    each time a requester closes its end, then exits or spins. Only the word
    "release" is accepted; anything else is logged and ignored.
    """
    try:
        fd = os.open(fifo_path, os.O_RDWR | os.O_NONBLOCK)
    except OSError as exc:
        _log(f"error: cannot open the release FIFO {fifo_path}: {exc}; "
             "operator releases will not be possible", "warn")
        return
    pending = b""
    try:
        while _running:
            r, _, _ = select.select([fd], [], [], 1.0)
            if not r:
                continue
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                continue
            except OSError as exc:
                _log(f"release FIFO read failed: {exc}", "warn")
                return
            if not chunk:
                continue                     # cannot happen while we hold O_RDWR
            pending += chunk
            if len(pending) > 4096:
                # An unbounded line is not a release request; drop it rather
                # than grow this buffer without limit.
                _log("release FIFO: oversized input discarded", "warn")
                pending = b""
                continue
            while b"\n" in pending:
                raw, _, pending = pending.partition(b"\n")
                word = raw.decode("utf-8", "replace").strip().lower()
                if not word:
                    continue
                if word != "release":
                    _log(f"release FIFO: ignoring unknown command {word!r}", "warn")
                    continue
                releaser.handle_request()
    finally:
        os.close(fd)


# ========================== entry point =====================================
def build_parser():
    p = argparse.ArgumentParser(description="Safety-latch manager", allow_abbrev=False,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    src = p.add_argument_group("log source (choose exactly one)")
    src.add_argument("--log-file", metavar="PATH",
                     help="follow a log file with tail -F, e.g. the SDM's own "
                          "atl_sdm.log, which is line-buffered")
    src.add_argument("--log-cmd", metavar="CMD",
                     help="run any command and follow its output, e.g. "
                          "'journalctl -fu psf', 'kubectl logs -f pod/psf', "
                          "'cat /dev/ttyTHS0', 'ssh host tail -F atl_sdm.log'")
    src.add_argument("--docker-container", metavar="NAME",
                     help="shorthand for --log-cmd 'docker logs -f --tail N NAME', "
                          "e.g. nv-psf for a containerised SDM")
    src.add_argument("--stdin", action="store_true", help="read log lines from stdin")
    src.add_argument("--tail", metavar="N", default="40000",
                     help="log history to replay before following live (default: 40000, "
                          "~10 min at 68 lines/s). "
                          "Set high enough to catch latches that started before startup.")
    src.add_argument("--wait-source-sec", metavar="SEC", type=float, default=300.0,
                     help="retry a log source that is not up yet, for up to SEC seconds "
                          "(default: 300; 0 fails at once). Useful if starting before "
                          "container/log becomes available.")

    tgt = p.add_argument_group("release target")
    tgt.add_argument("--release-fifo", metavar="PATH", default=DEFAULT_CMD_RX_FIFO,
                     help="write 'release' into this FIFO. Must match the "
                          "receiver's --release-fifo, which creates it "
                          f"(default: {DEFAULT_CMD_RX_FIFO})")
    tgt.add_argument("--dry-run", action="store_true",
                     help="log the decision without asking for a release")

    pol = p.add_argument_group("policy")
    pol.add_argument("--dwell-sec", type=float, default=10.0,
                     help="settle time after a cause RECOVERS, before release "
                          "(default: 10)")
    pol.add_argument("--identify-sec", type=float, default=30.0,
                     help="how long to wait for a latch cause to appear in the log "
                          "before handing the latch to an operator (default: 30)")
    pol.add_argument("--recheck-sec", type=float, default=5.0,
                     help="re-check interval between release attempts (default: 5)")
    pol.add_argument("--nag-sec", type=float, default=60.0,
                     help="how often to re-announce a latch we are not releasing "
                          "(default: 60)")
    pol.add_argument("--max-attempts", type=int, default=20,
                     help="max SDM refusals per episode before handing to an "
                          "operator, 0=unlimited (default: 20)")
    return p


def main():
    # Before anything else, so a stop during startup is still a clean one.
    signal.signal(signal.SIGTERM, _on_signal)
    signal.signal(signal.SIGINT, _on_signal)
    args = build_parser().parse_args()
    chosen = [n for n, v in (("--log-file", args.log_file), ("--log-cmd", args.log_cmd),
                             ("--docker-container", args.docker_container),
                             ("--stdin", args.stdin)) if v]
    if len(chosen) != 1:
        print(f"error: choose exactly one log source, got {chosen or 'none'}. "
              "See --help for --log-file / --log-cmd / --docker-container / --stdin.",
              file=sys.stderr)
        return 2
    if not args.release_fifo:
        print("error: --release-fifo cannot be empty", file=sys.stderr)
        return 2

    _log(f"safety-latch manager starting   {time.strftime('%Y-%m-%d')}")
    _log(f"{'policy':<{_LBL}}dwell {args.dwell_sec:g}s, recheck "
         f"{args.recheck_sec:g}s, max attempts {args.max_attempts}"
         + ("   DRY RUN: nothing will be sent" if args.dry_run else ""))
    _log(f"{'auto causes':<{_LBL}}" +
         ", ".join(c.name for c in CAUSES if c.auto))
    _log(f"{'manual causes':<{_LBL}}" +
         ", ".join(c.name for c in CAUSES if not c.auto) +
         ", plus any latch whose cause does not appear in the log")
    # Checked at startup purely to report it: otherwise a wrong path is only
    # discovered at the first release, mid-latch.
    path, err, fatal = resolve_target(args.release_fifo)
    if not err:
        _log(f"{'releases out':<{_LBL}}{path}")
    elif fatal and not args.dry_run:
        # Refuse to start rather than run blind: this mistake cannot be retried
        # away, and finding out at the first release means finding out mid-latch.
        print(f"error: {err}", file=sys.stderr)
        return 2
    else:
        # Absent is not misconfigured: the receiver may still be coming up, and
        # the target is re-checked on every attempt.
        _log(f"{'releases out':<{_LBL}}{args.release_fifo} (not created yet)")
        _log(f"warning: {err}; releases will fail until that is fixed")

    if not prepare_state_dir():
        return 2

    releaser = LatchReleaser(args, make_releaser(args))
    status = StatusFile(STATUS_FILE)
    # Before the log source opens, so a UI started first sees this manager as
    # present-and-replaying rather than absent.
    status.publish(releaser.snapshot())
    # After the first publish: a request arriving before the file existed would
    # have nowhere to report its outcome.
    threading.Thread(target=release_request_thread, args=(releaser, RELEASE_FIFO),
                     daemon=True).start()

    # Waiting happens entirely before the follow loop, so a source that is not up
    # yet never reaches the releaser at all.
    probe_sec = SOURCE_PROBE_SEC if args.wait_source_sec > 0 else 0.0
    wait_until = time.monotonic() + max(0.0, args.wait_source_sec)
    fh = proc = None
    waited = False
    while _running:
        fh, proc = open_log_source(args, announce=not waited, probe_sec=probe_sec)
        if fh is not None:
            break
        if time.monotonic() >= wait_until:
            _log("error: the log source cannot be read; check that it exists "
                 "and is running", "warn")
            return 2
        if not waited:
            _log(f"log source is not up yet; retrying every "
                 f"{SOURCE_RETRY_SEC:g}s for up to "
                 f"{args.wait_source_sec:g}s", "warn")
            waited = True
        status.publish(releaser.snapshot())
        time.sleep(SOURCE_RETRY_SEC)
    if fh is None:
        return 2                    # stopped by a signal while still waiting
    if waited:
        _log("log source is up")
    # Counts the replay window from the moment reading starts, not from the wait.
    releaser.restart_replay()
    print(flush=True)               # separates the startup block from the stream
    q = queue.Queue()
    threading.Thread(target=_reader_thread, args=(fh, q), daemon=True).start()

    rc = 0
    saw_line = False
    # Wait for valid log content; just opening isn't enough. Always wait at least UNDATED_ARM_SEC.
    content_wait = max(args.wait_source_sec, UNDATED_ARM_SEC)
    content_by = time.monotonic() + content_wait
    try:
        while _running:
            if content_by is not None:
                if releaser.saw_sdm():
                    content_by = None
                elif time.monotonic() > content_by:
                    _log(f"error: no SDM log lines arrived within {content_wait:g}s; "
                         f"check the container name or log path", "warn")
                    rc = 2
                    status.publish(releaser.snapshot())
                    break
            try:
                kind, payload = q.get(timeout=1.0)
            except queue.Empty:
                releaser.note_quiet()
                releaser.backfill_scene()
                releaser.tick()
                status.publish(releaser.snapshot())
                continue
            if kind == "eof":
                # Losing the source mid-latch means nobody is watching that latch
                # any more. Say so, or a supervised restart looks like a clean
                # exit and the latch quietly stays up.
                snap = releaser.snapshot()
                # Judged on the reader's exit status, not on whether any line
                # arrived: stderr is merged into this stream, so a failing reader
                # delivers its complaint as a line and a line count would read
                # that as a healthy source that simply ended.
                src_rc = None
                if proc is not None:
                    try:
                        src_rc = proc.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        src_rc = None
                if src_rc or not saw_line:
                    # Non-zero so a supervisor retries rather than recording a
                    # clean stop with nobody left watching the latch.
                    _log(f"error: the log source failed"
                         f"{f' (exit {src_rc})' if src_rc else ''}; check that it "
                         "exists and is readable", "warn")
                    rc = 2
                elif snap["latched"]:
                    _log(f"log source ended while a latch was still held "
                         f"({snap['auto_reason']}); exiting WITHOUT releasing it")
                else:
                    _log("log source ended")
                status.publish(releaser.snapshot())
                break
            saw_line = True
            releaser.observe(payload.rstrip("\n"))
            releaser.backfill_scene()
            releaser.tick()
            status.publish(releaser.snapshot())
    finally:
        if proc is not None:
            _stop_reader(proc)
    _log("manager stopped")
    return rc


if __name__ == "__main__":
    sys.exit(main())
