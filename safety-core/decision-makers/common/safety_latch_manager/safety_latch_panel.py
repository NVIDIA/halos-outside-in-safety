#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# Operator panel for the safety-latch manager. Entirely optional: nothing about
# the manager's behaviour changes if this never runs.
#
#   browser <--HTTP--> safety_latch_panel  <--/run/atl-latch/status.json--  manager
#                                          ---/run/atl-latch/release----->  manager
#
# The manager is an auto-release daemon with no network port and no knowledge of
# any UI. It publishes status to a file and takes requests on a FIFO. Everything
# web-shaped lives here, so the system serves exactly one port however many SDMs
# it grows to.
#
# It holds no policy of its own, and it writes no operator text of its own. The
# Release button is always live: it writes 'release' to the FIFO, the manager
# decides, and the answer shown is the manager's own words read back out of the
# status file. Routes:
#   /panel.js     the card, as a widget injected into another site's page
#   /v1/status    the manager's snapshot, plus stale_sec from the file's mtime
#   /v1/release   write to the FIFO, answer with the manager's recorded reason
#   /v1/events    push the snapshot to the browser whenever the file changes
#
#   ./safety_latch_panel.py --bind 0.0.0.0:8771
#
# The port here must match the one in the /panel.js injection snippet below,
# because the widget calls /v1/* back on the origin it was served from.
#
# There is no authentication. Bind to loopback, or restrict it at the network
# layer, unless the deployment has its own front door.

import argparse
import json
import os
import select
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Injectable widget for a page served by someone else, loaded the same way
# that page loads its own scripts:
#   var s=document.createElement('script');
#   s.src='http://'+location.hostname+':8771/panel.js';
#   document.head.appendChild(s);
# It calls back to the origin it was served from, so no address is baked in and
# the same line works whatever host or port the panel is bound to. The card
# lives in a shadow root so the host page's CSS cannot reach in and this CSS
# cannot leak out, and sits bottom-right, clear of the distortion card.
PANEL_JS = r"""
(function () {
  var HOST = (function () {
    var el = document.currentScript;
    if (el && el.src) { var a = document.createElement('a'); a.href = el.src; return a.origin; }
    return location.origin;
  })();
  var ID = 'nv-latch-panel';
  var old = document.getElementById(ID);
  if (old) old.remove();

  var CSS = `
.card{--line:rgba(255,255,255,.09);--text:#eceef2;--muted:rgba(236,238,242,.5);
 --good:#22c55e;--bad:#ef4444;--warn:#f59e0b;--accent:#76b900;
 width:330px;box-sizing:border-box;padding:14px 14px 13px;border-radius:14px;
 font:13px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Inter,Roboto,Helvetica,sans-serif;
 color:var(--text);background:linear-gradient(168deg,#1c1d22 0%,#141518 100%);
 border:1px solid var(--line);box-shadow:0 18px 44px rgba(0,0,0,.5);
 -webkit-backdrop-filter:blur(16px);backdrop-filter:blur(16px);
 opacity:0;transform:translateY(8px);transition:opacity .22s ease,transform .22s ease}
.card.in{opacity:1;transform:none}
.gone{display:none !important}

.hd{display:flex;align-items:center;gap:9px;padding-bottom:11px;margin-bottom:12px;
 border-bottom:1px solid var(--line)}
.ttl{font-size:14.5px;font-weight:600;letter-spacing:-.01em;color:#fff;white-space:nowrap}
.ttl em{font-style:normal;font-weight:500;color:var(--muted)}
.chip{display:flex;align-items:center;gap:6px;margin-left:auto;padding:0 9px;height:20px;
 border-radius:10px;box-sizing:border-box;
 font:700 9px/1 ui-monospace,SFMono-Regular,Menlo,monospace;white-space:nowrap;
 letter-spacing:.09em;color:#bbf7d0;background:rgba(34,197,94,.14)}
.chip i{flex:none;width:5px;height:5px;border-radius:50%;background:currentColor;
 box-shadow:0 0 6px rgba(34,197,94,.9)}
.chip.dry{color:var(--muted);background:rgba(255,255,255,.06)}
.chip.dry i{box-shadow:none;opacity:.5}
.x{width:24px;height:24px;flex:none;display:flex;align-items:center;justify-content:center;
 border:0;border-radius:7px;background:transparent;cursor:pointer;font:300 17px/1 inherit;
 color:rgba(255,255,255,.32);transition:all .15s}
.x:hover{color:#fff;background:rgba(255,255,255,.08)}

.st{display:flex;align-items:center;gap:9px;margin-bottom:12px}
.dot{flex:none;width:9px;height:9px;border-radius:50%;background:#4a4d56;
 transition:background .2s,box-shadow .2s}
.dot.ok{background:var(--good);box-shadow:0 0 7px rgba(34,197,94,.9),0 0 15px rgba(34,197,94,.4)}
.dot.bad{background:var(--bad);box-shadow:0 0 7px rgba(239,68,68,.85),0 0 15px rgba(239,68,68,.4);
 animation:beat 1.2s ease-in-out infinite}
@keyframes beat{0%,100%{opacity:1}50%{opacity:.3}}
.txt{font-size:15px;font-weight:600;letter-spacing:-.01em}
.held{margin-left:auto;font-size:11px;color:var(--muted);font-variant-numeric:tabular-nums}

.row{display:flex;gap:12px;align-items:baseline;padding:7px 0;border-top:1px solid var(--line)}
.row:first-child{border-top:none;padding-top:0}
.k{flex:none;width:124px;color:var(--muted);font-size:11.5px}
.v{flex:1;min-width:0;text-align:right;font-size:12px;font-weight:500;
 font-variant-numeric:tabular-nums;overflow-wrap:anywhere}
.v.ok{color:#a7f3c0}.v.warn{color:#fcd77f}.v.bad{color:#fca5a5}

.why{margin-top:12px;padding:11px 13px;border-radius:10px;
 font-size:12px;line-height:1.55;
 height:76px;overflow-y:auto;box-sizing:border-box;
 color:#fcd77f;background:rgba(245,158,11,.1);border:1px solid rgba(245,158,11,.22);
 overflow-wrap:anywhere;scrollbar-width:thin;
 scrollbar-color:rgba(255,255,255,.18) transparent}
.why::-webkit-scrollbar{width:6px}
.why::-webkit-scrollbar-track{background:transparent}
.why::-webkit-scrollbar-thumb{background:rgba(255,255,255,.18);border-radius:3px}
.why.ok{color:#a7f3c0;background:rgba(34,197,94,.1);border-color:rgba(34,197,94,.24)}
.why.bad{color:#fca5a5;background:rgba(239,68,68,.1);border-color:rgba(239,68,68,.24)}

.btn{width:100%;height:38px;margin-top:12px;border-radius:9px;cursor:pointer;
 font:600 12.5px/1 inherit;color:#fca5a5;
 background:transparent;border:1px solid rgba(239,68,68,.5);
 transition:background .15s,border-color .15s,color .15s,transform .1s,opacity .15s}
.btn:hover:not(:disabled){background:rgba(239,68,68,.12);
 border-color:rgba(239,68,68,.85);color:#fecaca}
.btn:active:not(:disabled){transform:scale(.985);background:rgba(239,68,68,.2)}
.btn:disabled{cursor:default;opacity:.45;background:transparent;
 border-color:var(--line);color:var(--muted)}

.note{margin-top:11px;padding-top:10px;border-top:1px solid var(--line);
 font-size:10.5px;line-height:1.5;color:var(--muted)}
`;

  var host = document.createElement('div');
  host.id = ID;
  host.style.cssText = 'position:fixed;bottom:16px;right:16px;z-index:2147483647';
  var root = host.attachShadow({mode: 'open'});
  root.innerHTML = '<style>' + CSS + '</style>' + [
'<div class="card">',
  '<div class="hd">',
    '<span class="ttl">Safety Latch <em>Panel</em></span>',
    '<span class="chip"><i></i>AUTO-RELEASE</span>',
    '<button class="x" type="button" aria-label="Close">&times;</button></div>',
  '<div class="st"><span class="dot"></span><span class="txt">connecting\u2026</span>',
    '<span class="held"></span></div>',
  '<div class="rows"></div>',
  '<div class="why"></div>',
  '<button class="btn" type="button">Release safe state</button>',
  '<div class="note">The safe state normally clears by itself once the trailer ',
  'is clear and the fault has recovered. Press the button if you need to clear ',
  'it now; if it cannot be cleared yet, the reason appears above.</div>',
'</div>'].join('');
  document.body.appendChild(host);

  var q = function (s) { return root.querySelector(s); };
  var card = q('.card'), dot = q('.dot'), txt = q('.txt'), held = q('.held');
  var chip = q('.chip'), rows = q('.rows'), why = q('.why');
  var btn = q('.btn');
  var timer = null;
  var es = null;
  var holdUntil = 0;
  var busy = false;
  var POLL_MS = 200;

  requestAnimationFrame(function () { card.classList.add('in'); });

  q('.x').onclick = function () {
    if (timer) { clearInterval(timer); timer = null; }
    if (es) { es.close(); es = null; }
    host.remove();
  };

  function note(cls, text) {
    why.className = 'why' + (cls ? ' ' + cls : '');
    why.textContent = text;
    why.title = text;
  }

  btn.onclick = function () {
    if (busy) { return; }
    busy = true;
    btn.disabled = true;
    holdUntil = Date.now() + 4000;
    note('', 'Sending\u2026');
    fetch(HOST + '/v1/release', {method: 'POST'})
      .then(function (r) { return r.json(); })
      .then(function (j) { note(j.accepted ? 'ok' : 'bad', j.reason); })
      .catch(function () { note('bad', 'Could not reach the safety system.'); })
      .then(function () {
        busy = false;
        btn.disabled = false;
        holdUntil = Date.now() + 4000;
        poll();
      });
  };

  function row(k, v, cls) {
    return '<div class="row"><span class="k">' + k + '</span><span class="v ' +
      (cls || '') + '">' + v + '</span></div>';
  }

  function causeText(cs) {
    if (!cs.length) { return '\u2014'; }
    return cs.map(function (c) {
      return c.name + (!c.auto ? ' \u00b7 manual only'
        : !c.recovered ? ' \u00b7 waiting'
        : c.dwell_remaining_sec > 0 ? ' \u00b7 settling ' + c.dwell_remaining_sec + 's'
        : ' \u00b7 recovered');
    }).join(', ');
  }

  function render(s) {
    if (s.manager_absent) { offline(s.reason || 'no manager is publishing'); return; }

    dot.className = 'dot ' + (s.latched ? 'bad' : 'ok');
    txt.textContent = s.latched ? 'Safe state latched' : 'No latch held';
    txt.style.color = s.latched ? '#fca5a5' : '#a7f3c0';
    held.textContent = s.latched ? 'held ' + Math.round(s.held_sec) + 's' : '';
    chip.className = 'chip' + (s.dry_run ? ' dry' : '');
    chip.innerHTML = '<i></i>' + (s.dry_run ? 'DRY RUN' : 'AUTO-RELEASE');

    var blind = s.failed_pipelines.length;
    var sc = s.scene;
    rows.innerHTML =
      row('latch cause', causeText(s.causes)) +
      row('cameras', blind ? 'failed: ' + s.failed_pipelines.join(', ') : 'all healthy',
          blind ? 'bad' : 'ok') +
      row('forklift in trailer', sc.forklift_in_trailer === null ? 'unknown'
            : (sc.forklift_in_trailer ? 'YES' : 'no') +
              (sc.forklift_unverified ? ' \u00b7 not confirmed' : ''),
          sc.forklift_in_trailer ? 'bad'
            : sc.forklift_in_trailer === null || sc.forklift_unverified ? 'warn'
            : 'ok') +
      row('persons in trailer', sc.persons_in_trailer === null ? 'unknown'
            : String(sc.persons_in_trailer),
          sc.persons_in_trailer ? 'bad'
            : sc.persons_in_trailer === null ? 'warn' : 'ok') +
      row('restricted area', sc.restricted_area_violation === null ? 'unknown'
            : sc.restricted_area_violation ? 'VIOLATION' : 'clear',
          sc.restricted_area_violation === null ? 'warn'
            : sc.restricted_area_violation ? 'bad' : 'ok') +
      row('release attempts', s.attempts + (s.max_attempts ? ' of ' + s.max_attempts : ''));
    if (Date.now() < holdUntil) { return; }
    if (s.manager_stale) {
      note('bad', 'Lost contact with the safety system. The information below ' +
                  'may be out of date.');
    }
    else if (s.cmd_rx_ready === false) {
      note('bad', 'The safe state cannot be cleared right now: the trailer ' +
                  'controller is not reachable. Contact maintenance.');
    }
    else { note(s.latched ? (s.needs_operator ? 'bad' : '') : 'ok',
                s.operator_note || ''); }
  }

  function offline(msg) {
    dot.className = 'dot';
    txt.textContent = 'Manager offline';
    txt.style.color = 'rgba(236,238,242,.5)';
    held.textContent = '';
    note('bad', 'Cannot reach the manager: ' + msg);
  }

  function poll() {
    fetch(HOST + '/v1/status', {cache: 'no-store'})
      .then(function (r) { return r.json(); })
      .then(render)
      .catch(function (e) { offline(e.message); });
  }

  function startPolling() {
    if (timer) { return; }
    poll();
    timer = setInterval(poll, POLL_MS);
  }

  function connect() {
    if (typeof EventSource === 'undefined') { startPolling(); return; }
    try {
      es = new EventSource(HOST + '/v1/events');
    } catch (e) {
      startPolling();
      return;
    }
    es.onmessage = function (ev) {
      if (timer) { clearInterval(timer); timer = null; }
      try { render(JSON.parse(ev.data)); } catch (e) { /* ignore a torn frame */ }
    };
    es.onerror = function () {
      startPolling();
    };
    startPolling();
  }
  connect();
})();
"""


# Must match safety_latch_manager.py. Fixed rather than configurable: this is an
# internal contract between the daemon and this panel, and a flag on either side
# would only create ways for the two to disagree about where to look. The
# environment override is the test suite's seam, read from the same variable the
# manager reads, and is not a deployment knob.
STATE_DIR = os.environ.get("ATL_LATCH_STATE_DIR", "/run/atl-latch")
STATUS_FILE = os.path.join(STATE_DIR, "status.json")
RELEASE_FIFO = os.path.join(STATE_DIR, "release")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    # How often the event stream re-stats the status file. Sets the added
    # latency between the manager publishing and the browser rendering; a stat()
    # every 50ms costs nothing measurable.
    WATCH_SEC = 0.05
    # Keep-alive on an idle stream, and how a browser that vanished silently is
    # discovered, since the write is what raises.
    HEARTBEAT_SEC = 15.0
    # Older than this and the status is reported stale rather than current. The
    # manager republishes every 2s even when nothing changed, so this is a
    # multiple of that heartbeat and not of how often the SDM happens to speak --
    # otherwise a quiet trailer reads as a dead manager.
    STALE_SEC = 8.0
    # How long to wait for the manager to record a request's outcome. Generous
    # next to its microsecond FIFO pickup, because the alternative -- giving up
    # early -- shows an operator "no answer" for a release that did happen.
    REPLY_TIMEOUT_SEC = 3.0
    # Each subscriber holds a ThreadingHTTPServer thread while connected, so the
    # count is capped: a browser stuck in a reload loop must not be able to
    # exhaust this process's threads and take the Release button with it.
    MAX_EVENT_CLIENTS = 6
    _event_clients = 0
    _event_lock = threading.Lock()

    def log_message(self, fmt, *a):
        pass

    def _send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self._send(204, b"", "text/plain")

    def _json(self, code, payload):
        self._send(code, json.dumps(payload).encode(), "application/json")

    def _status(self):
        """Current status, or a synthetic one describing why there is none.

        `stale_sec` comes from the file's mtime, which is why the manager only
        rewrites the file when the content changes: it lets a UI distinguish
        "nothing is happening" from "that manager died", something a refused
        connection never could.
        """
        try:
            st = os.stat(STATUS_FILE)
            with open(STATUS_FILE) as fh:
                snap = json.load(fh)
        except (OSError, ValueError):
            # One message for every way this can fail, because the operator's
            # situation is identical in all of them and none of the distinctions
            # are actionable by them. The detail goes to this process's stderr,
            # where whoever runs it can see it.
            return None, ("The safety system is not running. "
                          "Contact maintenance.")
        snap["stale_sec"] = round(max(0.0, time.time() - st.st_mtime), 1)
        snap["manager_stale"] = snap["stale_sec"] > self.STALE_SEC
        return snap, ""

    def _release(self):
        """Ask the manager to release, and return the answer it recorded.

        The FIFO carries no reply, so the answer is read back out of the status
        file: note the request seq before writing, then wait for a higher one.
        That makes the outcome durable -- it survives this panel dying mid-wait,
        and two panels asking see the same recorded answer.

        Every failure here reduces to one operator-facing sentence. The panel
        holds no policy: an accepted or refused release is always the manager's
        own words, passed through untouched.
        """
        down = "The safety system is not running. Contact maintenance."
        before, err = self._status()
        if before is None:
            return self._json(503, {"accepted": False, "reason": err})
        seq = (before.get("last_request") or {}).get("seq", 0)
        try:
            # O_NONBLOCK: with no manager holding the read end this raises ENXIO
            # at once instead of blocking this request thread forever.
            fd = os.open(RELEASE_FIFO, os.O_WRONLY | os.O_NONBLOCK)
        except OSError as exc:
            print(f"release failed: cannot open {RELEASE_FIFO}: {exc}",
                  file=sys.stderr, flush=True)
            return self._json(503, {"accepted": False, "reason": down})
        try:
            if os.write(fd, b"release\n") != len(b"release\n"):
                print("release failed: short write to the FIFO", file=sys.stderr,
                      flush=True)
                return self._json(503, {"accepted": False, "reason": down})
        except OSError as exc:
            print(f"release failed: cannot write {RELEASE_FIFO}: {exc}",
                  file=sys.stderr, flush=True)
            return self._json(503, {"accepted": False, "reason": down})
        finally:
            os.close(fd)

        deadline = time.monotonic() + self.REPLY_TIMEOUT_SEC
        while time.monotonic() < deadline:
            after, _ = self._status()
            got = (after or {}).get("last_request") or {}
            if got.get("seq", 0) > seq:
                # 409 for a refusal: the request was well formed and the state
                # simply forbids it, and the panel shows `reason` verbatim.
                return self._json(200 if got.get("accepted") else 409,
                                  {"accepted": bool(got.get("accepted")),
                                   "reason": got.get("reason", "")})
            time.sleep(0.02)
        print("release: no outcome recorded within "
              f"{self.REPLY_TIMEOUT_SEC:g}s", file=sys.stderr, flush=True)
        return self._json(504, {"accepted": False, "reason":
                                "No answer from the safety system. "
                                "Contact maintenance."})

    def _peer_gone(self):
        """True once the browser has closed this stream.

        Non-blocking throughout: select() with a zero timeout, and the peek only
        runs on an already-readable socket. Treats unexpected inbound bytes as
        gone too, since nothing should ever send on an event stream.
        """
        try:
            r, _, _ = select.select([self.connection], [], [], 0)
            if not r:
                return False
            return self.connection.recv(1, socket.MSG_PEEK) == b""
        except OSError:
            return True

    def _events(self):
        """Push the status to the browser whenever the file changes.

        This is the panel's own server doing panel work: the manager publishes a
        file and knows nothing about this. Watching an mtime rather than holding
        a connection to the manager is what keeps that separation, and it costs
        one stat() per interval.
        """
        # Reserved before any header is sent, so a refusal is a clean 503 the
        # widget falls back from rather than a stream that dies mid-frame.
        with Handler._event_lock:
            if Handler._event_clients >= self.MAX_EVENT_CLIENTS:
                return self._json(503, {"reason": "too many event subscribers"})
            Handler._event_clients += 1
        try:
            self.close_connection = True     # no Content-Length on a stream
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "close")
            self.send_header("X-Accel-Buffering", "no")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            last, last_write = None, 0.0
            while True:
                # Detected by reading, not by a failed write. A browser that
                # closes while the status is unchanged produces no write for up
                # to HEARTBEAT_SEC, and waiting that long to notice would hold
                # its subscriber slot the whole time -- so a few quick reloads
                # would exhaust the cap. EventSource never sends on this stream,
                # so anything readable here means the peer is gone.
                if self._peer_gone():
                    break
                snap, err = self._status()
                body = json.dumps(snap if snap is not None
                                  else {"manager_absent": True, "reason": err})
                now = time.monotonic()
                if body != last:
                    self.wfile.write(b"data: " + body.encode() + b"\n\n")
                    last = body
                elif now - last_write < self.HEARTBEAT_SEC:
                    time.sleep(self.WATCH_SEC)
                    continue
                else:
                    # An SSE comment, ignored by EventSource. The write is what
                    # reveals a browser that vanished without closing.
                    self.wfile.write(b": ping\n\n")
                self.wfile.flush()
                last_write = time.monotonic()
                time.sleep(self.WATCH_SEC)
        except (BrokenPipeError, ConnectionResetError, OSError, ValueError):
            pass                             # browser closed the panel
        finally:
            with Handler._event_lock:
                Handler._event_clients -= 1

    def do_GET(self):
        if self.path.startswith("/v1/events"):
            return self._events()
        if self.path == "/panel.js":
            return self._send(200, PANEL_JS.encode(),
                              "application/javascript; charset=utf-8")
        if self.path.startswith("/v1/status"):
            snap, err = self._status()
            if snap is None:
                return self._json(503, {"manager_absent": True, "reason": err})
            return self._json(200, snap)
        self._send(404, b"not found", "text/plain")

    def do_POST(self):
        if not self.path.startswith("/v1/release"):
            return self._send(404, b"not found", "text/plain")
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            n = 0
        if 0 < n <= 4096:
            self.rfile.read(n)
        self._release()


def main():
    p = argparse.ArgumentParser(description="Operator panel for the safety-latch manager",
                                allow_abbrev=False)
    p.add_argument("--bind", metavar="HOST:PORT", default="127.0.0.1:8771",
                   help="where to serve /panel.js, /v1/status, /v1/release and "
                        "/v1/events (default: 127.0.0.1:8771). The port must "
                        "match the injection snippet: the widget calls /v1/* "
                        "back on the origin it was served from. Use "
                        "0.0.0.0:8771 to inject it into a page opened from "
                        "another machine")
    args = p.parse_args()

    host, _, port_s = args.bind.rpartition(":")
    host = host or "127.0.0.1"
    if not port_s.isdigit() or not (1 <= int(port_s) <= 65535):
        print(f"error: --bind '{args.bind}' is not HOST:PORT with a valid port",
              file=sys.stderr)
        return 2

    try:
        srv = ThreadingHTTPServer((host, int(port_s)), Handler)
    except OSError as exc:
        print(f"error: cannot bind {args.bind}: {exc}", file=sys.stderr)
        return 2
    # A /v1/events subscriber holds its handler thread while connected;
    # block_on_close would make server_close() wait for an idle browser.
    srv.block_on_close = False

    print(f"panel on http://{host}:{port_s}  ->  {STATUS_FILE}", flush=True)
    if not os.path.exists(STATUS_FILE):
        print(f"note: nothing is publishing {STATUS_FILE} yet; the panel will "
              "show the safety system as down until the manager starts",
              file=sys.stderr)
    if host not in ("127.0.0.1", "localhost", "::1"):
        print("warning: panel is reachable off-host and has NO authentication",
              file=sys.stderr)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
