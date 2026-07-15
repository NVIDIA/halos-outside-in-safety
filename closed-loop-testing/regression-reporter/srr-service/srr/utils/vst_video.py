# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""VST video utilities — download cam0 for a time range, split a parquet
run into per-scene mp4 clips by reading scenes_manifest.csv.

CLI:
  # download a single time range:
  python3 -m srr.utils.vst_video download \\
      --cam Camera --start 2026-04-30T07:17:28.993Z --end 2026-04-30T07:22:29.827Z \\
      --out /path/to/out.mp4

  # full run + per-scene split (reads scenes_manifest.csv):
  python3 -m srr.utils.vst_video split-run \\
      --run-dir /app/runs/multi-test-XYZ/in-roi-5min \\
      --out-dir /data/sil-data/video/multi-test-XYZ/in-roi-5min \\
      [--cam Camera]

  # list recorded streams + timelines (debug):
  python3 -m srr.utils.vst_video list

Library:
  from srr.utils.vst_video import VstClient
  vc = VstClient()
  vc.download_clip("Camera", start_iso, end_iso, "/path/clip.mp4")
  vc.split_run("/app/runs/.../in-roi-5min",
               "/data/sil-data/video/.../in-roi-5min")
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import os
import sys
import time
from pathlib import Path
from typing import Iterable, Optional
from urllib.parse import quote
from urllib.request import urlopen, Request


def iso_z(epoch_seconds: float) -> str:
    """Convert Unix epoch seconds → ISO-8601 millisecond Z (matches VST API)."""
    s = dt.datetime.utcfromtimestamp(epoch_seconds).strftime("%Y-%m-%dT%H:%M:%S.%f")
    return s[:-3] + "Z"


def normalise_iso(s: str) -> str:
    """Replace +00:00 → Z so VST API accepts it."""
    return s.replace("+00:00", "Z")


class VstClient:
    """Thin client over the VST HTTP API for clip download + listing."""

    def __init__(
        self,
        base_url: str = None,
        host_ip: str = None,
        port: int = None,
        timeout: int = 60,
    ) -> None:
        # Precedence (highest first):
        #   1. Explicit `base_url` arg
        #   2. Explicit `host_ip` / `port` args (any one set)
        #   3. $VST_BASE_URL env (typically inherited from Halos compose .env)
        #   4. $HOST_IP / $VST_PORT envs
        #   5. Built-in defaults (127.0.0.1 / 30888)
        if base_url:
            self.base = base_url.rstrip("/")
        elif host_ip is not None or port is not None:
            host = host_ip or os.environ.get("HOST_IP", "127.0.0.1")
            p = port if port is not None else int(os.environ.get("VST_PORT", "30888"))
            self.base = f"http://{host}:{p}/vst/api"
        elif os.environ.get("VST_BASE_URL"):
            self.base = os.environ["VST_BASE_URL"].rstrip("/")
        else:
            host = os.environ.get("HOST_IP", "127.0.0.1")
            p = int(os.environ.get("VST_PORT", "30888"))
            self.base = f"http://{host}:{p}/vst/api"
        self.timeout = timeout
        self._stream_cache: Optional[dict[str, str]] = None

    # ---- low-level ----
    def _get_json(self, path: str) -> object:
        req = Request(self.base + path, headers={"Accept": "application/json"})
        with urlopen(req, timeout=self.timeout) as r:
            return json.loads(r.read().decode())

    def _download(self, url: str, out_path: str | os.PathLike) -> int:
        out_path = Path(out_path)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        with urlopen(url, timeout=self.timeout) as r, open(out_path, "wb") as f:
            n = 0
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
                n += len(chunk)
            return n

    # ---- streams + timelines ----
    def list_streams(self) -> dict[str, dict]:
        """Return {streamId: stream_info_dict}."""
        out: dict[str, dict] = {}
        data = self._get_json("/v1/record/streams")
        for entry in data:
            for sid, infos in entry.items():
                if infos:
                    out[sid] = infos[0]
        return out

    def stream_id_by_name(self, name: str) -> str:
        """Map a friendly camera name (e.g. 'Camera', 'Camera_01') to its UUID."""
        if self._stream_cache is None:
            self._stream_cache = {info["name"]: sid
                                  for sid, info in self.list_streams().items()}
        if name not in self._stream_cache:
            raise KeyError(f"camera {name!r} not found in VST streams; "
                           f"have: {sorted(self._stream_cache)}")
        return self._stream_cache[name]

    def timelines(self, name: str) -> list[dict]:
        sid = self.stream_id_by_name(name)
        return self._get_json(f"/v1/record/{sid}/timelines")

    # ---- clip download ----
    def get_clip_url(self, name: str, start_iso: str, end_iso: str,
                     container: str = "mp4") -> str:
        """Ask VST for a temporary download URL for [start, end]."""
        sid = self.stream_id_by_name(name)
        path = (f"/v1/storage/file/{sid}/url"
                f"?startTime={quote(normalise_iso(start_iso))}"
                f"&endTime={quote(normalise_iso(end_iso))}"
                f"&container={container}")
        meta = self._get_json(path)
        if not isinstance(meta, dict) or "videoUrl" not in meta:
            raise RuntimeError(f"unexpected response: {meta!r}")
        return meta["videoUrl"]

    def download_clip(self, name: str, start_iso: str, end_iso: str,
                      out_path: str | os.PathLike, container: str = "mp4") -> int:
        url = self.get_clip_url(name, start_iso, end_iso, container)
        return self._download(url, out_path)

    # ---- run-level helpers ----
    def split_run(
        self,
        run_dir: str | os.PathLike,
        out_dir: str | os.PathLike,
        cam: str = "Camera",
        full_video: bool = True,
        scenes: bool = True,
    ) -> dict:
        """Read run_dir/scenes/scenes_manifest.csv + run_dir/run-*.parquet,
        download cam0 full + per-scene clips into out_dir.

        Returns {"full": path or None, "scenes": {"scn_0000": path, ...}}.
        """
        run_dir = Path(run_dir); out_dir = Path(out_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        result: dict = {"full": None, "scenes": {}}

        if full_video:
            # Full window = first scene start → last scene end from manifest.
            # (avoids pandas dependency on the host).
            try:
                manifest = run_dir / "scenes" / "scenes_manifest.csv"
                rows = list(csv.DictReader(open(manifest)))
                if not rows:
                    raise ValueError("empty manifest")
                start = normalise_iso(rows[0]["iso_start"])
                end = normalise_iso(rows[-1]["iso_end"])
                full_out = out_dir / "cam0.mp4"
                bytes_ = self.download_clip(cam, start, end, full_out)
                result["full"] = str(full_out)
                print(f"  full: {full_out}  ({bytes_/1e6:.1f} MB)  [{start} → {end}]")
            except FileNotFoundError:
                print(f"  manifest not found at {manifest}; skipping full download")
            except Exception as e:
                print(f"  full download FAILED: {e}")

        if scenes:
            manifest = run_dir / "scenes" / "scenes_manifest.csv"
            if not manifest.exists():
                print(f"  manifest not found: {manifest}")
                return result
            with open(manifest, newline="") as f:
                rdr = csv.DictReader(f)
                for row in rdr:
                    scn = row["scenario_id"]
                    s = normalise_iso(row["iso_start"])
                    e = normalise_iso(row["iso_end"])
                    out_path = out_dir / f"{scn}.mp4"
                    try:
                        b = self.download_clip(cam, s, e, out_path)
                        result["scenes"][scn] = str(out_path)
                        print(f"  {scn}: {out_path}  ({b/1e6:.1f} MB)")
                    except Exception as exc:
                        print(f"  {scn} FAILED: {exc}")
        return result


# ---- CLI ----
def _cmd_list(args, vc: VstClient):
    print("Available VST streams:")
    for sid, info in vc.list_streams().items():
        print(f"  {info['name']:14s}  {sid}  url={info.get('url','')}")
    print("\nTimelines:")
    for sid, info in vc.list_streams().items():
        try:
            tls = vc.timelines(info["name"])
            for tl in tls:
                print(f"  {info['name']:14s}  {tl['startTime']} → {tl['endTime']}")
        except Exception as e:
            print(f"  {info['name']:14s}  ERR {e}")


def _cmd_download(args, vc: VstClient):
    n = vc.download_clip(args.cam, args.start, args.end, args.out, args.container)
    print(f"saved {args.out} ({n/1e6:.1f} MB)")


def _cmd_split_run(args, vc: VstClient):
    vc.split_run(args.run_dir, args.out_dir, cam=args.cam,
                 full_video=not args.no_full, scenes=not args.no_scenes)


def main(argv: Optional[list[str]] = None) -> None:
    ap = argparse.ArgumentParser(prog="srr.utils.vst_video")
    ap.add_argument("--host", help="VST host IP (defaults to $HOST_IP, default 127.0.0.1)")
    ap.add_argument("--port", type=int,
                    help="VST port (defaults to $VST_PORT, default 30888)")
    ap.add_argument("--base-url",
                    help="Override base URL entirely (defaults to $VST_BASE_URL).")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("list", help="list VST streams + timelines")
    sp.set_defaults(func=_cmd_list)

    sp = sub.add_parser("download", help="download a single time-range clip")
    sp.add_argument("--cam", default="Camera",
                    help="friendly name (Camera, Camera_01, Camera_02)")
    sp.add_argument("--start", required=True, help="ISO start (e.g. 2026-04-30T07:17:28.993Z)")
    sp.add_argument("--end", required=True)
    sp.add_argument("--out", required=True)
    sp.add_argument("--container", default="mp4")
    sp.set_defaults(func=_cmd_download)

    sp = sub.add_parser("split-run", help="download cam0 full + per-scene clips for a run dir")
    sp.add_argument("--run-dir", required=True,
                    help="contains run-*.parquet and scenes/scenes_manifest.csv")
    sp.add_argument("--out-dir", required=True,
                    help="cam0.mp4 + scn_*.mp4 will be written here (flat layout)")
    sp.add_argument("--cam", default="Camera")
    sp.add_argument("--no-full", action="store_true", help="skip full cam0.mp4")
    sp.add_argument("--no-scenes", action="store_true", help="skip per-scene clips")
    sp.set_defaults(func=_cmd_split_run)

    args = ap.parse_args(argv)
    vc = VstClient(base_url=getattr(args, "base_url", None),
                   host_ip=args.host, port=args.port)
    args.func(args, vc)


if __name__ == "__main__":
    main()
