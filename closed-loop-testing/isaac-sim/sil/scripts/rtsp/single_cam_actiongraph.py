# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Build a 1-camera RTSP streaming Action Graph for Isaac Sim 6.0.

Builds the same RTSP streaming graph as the manual UI walkthrough (verified on
a native Isaac Sim 6.0 UI session — RTSP h264 1280x720 @ 60fps stream OK at
rtsp://localhost:8554/stream).

**Usage** — run inside Isaac Sim 6.0 Script Editor (NOT standalone via
python.sh). The standalone SimulationApp wrapper has a lifecycle issue: server
bind events fire ~28s after app ready, after the script's main update loop has
already exited.

UI workflow:
1. Launch Isaac Sim 6.0: `./isaac-sim.sh`
2. Enable extension `isaacsim.streaming.rtsp` (Window > Extensions, search +
   toggle on), or launch with `--enable isaacsim.streaming.rtsp`.
3. (Optional) create a camera prim via Create > Camera if scene is empty. By
   default this script creates `/World/Camera` if missing.
4. Window > Script Editor > paste this script > Run.
5. Press Play (timeline). Server starts lazily on first rendered frame.
6. Verify from terminal: `ffprobe -v info -show_streams rtsp://localhost:8554/stream`

**Output**:
- Action Graph at `/World/RTSPGraph`
- 3 nodes: OnPlaybackTick -> IsaacCreateRenderProduct -> RTSPCameraHelper
- Camera prim at `/World/Camera` (auto-created if missing)
- Stream URL: `rtsp://<host>:8554/stream`

**Params** (edit the constants below if needed):
- CAMERA_PRIM_PATH
- RESOLUTION (width, height)
- RTSP_PORT
- MOUNT_PATH
- USE_RAW_ENCODING (False = NVENC h264, True = raw uncompressed for local debug)
"""

import omni.graph.core as og
import omni.kit.app
import omni.usd
from pxr import UsdGeom

# ---------- Config ----------
CAMERA_PRIM_PATH = "/World/Camera"
RESOLUTION = (1280, 720)
RTSP_PORT = 8554
MOUNT_PATH = "/stream"
USE_RAW_ENCODING = False
GRAPH_PATH = "/World/RTSPGraph"
# ----------------------------


def ensure_streaming_extension_enabled():
    """Enable isaacsim.streaming.rtsp + dependencies if not already enabled."""
    ext_mgr = omni.kit.app.get_app().get_extension_manager()
    for ext_id in ["isaacsim.core.nodes", "isaacsim.streaming.rtsp"]:
        if not ext_mgr.is_extension_enabled(ext_id):
            ext_mgr.set_extension_enabled_immediate(ext_id, True)
            print(f"[RTSP] Enabled {ext_id}")
        else:
            print(f"[RTSP] {ext_id} already enabled")


def ensure_camera_prim(stage, prim_path):
    """Create Camera prim at prim_path if it does not exist yet."""
    if not stage.GetPrimAtPath(prim_path):
        UsdGeom.Camera.Define(stage, prim_path)
        print(f"[RTSP] Created camera prim at {prim_path}")
    else:
        print(f"[RTSP] Camera prim already exists at {prim_path}")


def build_single_cam_graph(camera_prim_path, port, mount_path,
                            resolution=(1280, 720), use_raw_encoding=False,
                            graph_path="/World/RTSPGraph"):
    """Build 3-node Action Graph for 1 camera streaming over RTSP.

    Returns: og.Controller graph object.
    """
    width, height = resolution

    keys = og.Controller.Keys
    (graph, nodes, _, _) = og.Controller.edit(
        {"graph_path": graph_path, "evaluator_name": "execution"},
        {
            keys.CREATE_NODES: [
                ("OnPlaybackTick", "omni.graph.action.OnPlaybackTick"),
                ("CreateRenderProduct", "isaacsim.core.nodes.IsaacCreateRenderProduct"),
                ("RTSPHelper", "isaacsim.streaming.rtsp.RTSPCameraHelper"),
            ],
            keys.SET_VALUES: [
                ("CreateRenderProduct.inputs:cameraPrim", camera_prim_path),
                ("CreateRenderProduct.inputs:width", width),
                ("CreateRenderProduct.inputs:height", height),
                ("RTSPHelper.inputs:port", port),
                ("RTSPHelper.inputs:mountPath", mount_path),
                ("RTSPHelper.inputs:useRawEncoding", use_raw_encoding),
            ],
            keys.CONNECT: [
                ("OnPlaybackTick.outputs:tick", "CreateRenderProduct.inputs:execIn"),
                ("CreateRenderProduct.outputs:execOut", "RTSPHelper.inputs:execIn"),
                ("CreateRenderProduct.outputs:renderProductPath",
                 "RTSPHelper.inputs:renderProductPath"),
            ],
        },
    )

    print(f"[RTSP] Action Graph built at {graph_path}")
    print(f"[RTSP] Stream URL once Play pressed: rtsp://<host>:{port}{mount_path}")
    return graph


def main():
    """Entry point when running inside Isaac Sim Script Editor."""
    ensure_streaming_extension_enabled()

    stage = omni.usd.get_context().get_stage()
    if stage is None:
        raise RuntimeError("No USD stage open. Open a scene first (File > New, "
                           "or File > Open).")

    ensure_camera_prim(stage, CAMERA_PRIM_PATH)

    build_single_cam_graph(
        camera_prim_path=CAMERA_PRIM_PATH,
        port=RTSP_PORT,
        mount_path=MOUNT_PATH,
        resolution=RESOLUTION,
        use_raw_encoding=USE_RAW_ENCODING,
        graph_path=GRAPH_PATH,
    )

    print(f"[RTSP] Setup complete. Press Play in timeline → stream available at "
          f"rtsp://<host>:{RTSP_PORT}{MOUNT_PATH}")


if __name__ == "__main__":
    main()
