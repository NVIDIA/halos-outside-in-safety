# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Build N-camera RTSP streaming Action Graph from YAML config — scene-agnostic.

Load cameras config from YAML file (format matches Halos SIL cameras.yaml 6.0
schema) -> build N pairs of (IsaacCreateRenderProduct, RTSPCameraHelper) sharing
a single OnPlaybackTick.

**Usage** — run inside Isaac Sim 6.0 Script Editor (NOT standalone python.sh —
the SimulationApp wrapper has a lifecycle issue with RTSP server bind timing).

1. Launch Isaac Sim 6.0: `./isaac-sim.sh --enable isaacsim.streaming.rtsp`
2. Open the target scene (warehouse scene / etc). The scene must already
   contain the `camera_prim` prims listed in the YAML — the Halos SIL
   warehouse scene no longer bakes them, so first spawn them by running
   camera_loader.py (or add `spawn:` blocks to the YAML and let
   run_actor_sdg.py spawn them).
3. Edit the `CONFIG_PATH` constant below to point at the right YAML for the
   scene, or override via env var ISAAC_RTSP_CAMERAS_YAML.
4. Window > Script Editor > paste + Run.
5. Press Play.
6. Verify: `ffprobe rtsp://<host>:<port><mount_path>` per camera.

**YAML schema** (per Halos SIL sil/configs/cameras.yaml 6.0):
```yaml
cameras:
  - name: <display name>           # required, unique
    camera_prim: </USD/prim/path>  # required, must exist on stage
    port: <int 1024-65535>         # required, unique per camera
    mount_path: </url-path>        # required, must start with /
rtsp:
  host: ${HOST_IP}                 # informational (VST URL building)
```
"""

import os
import omni.graph.core as og
import omni.kit.app
import omni.usd

try:
    import yaml
except ImportError:
    yaml = None

# ---------- Config ----------
# Default path when running from Script Editor inside the SIL container — edit
# if your cameras.yaml lives elsewhere. Override via env ISAAC_RTSP_CAMERAS_YAML.
CONFIG_PATH = "/isaac-sim/sil/configs/cameras.yaml"

RESOLUTION = (1280, 720)
USE_RAW_ENCODING = False
GRAPH_PATH = "/World/RTSPMultiGraph"
# ----------------------------


def load_cameras_yaml(yaml_path):
    """Load + validate cameras config from YAML. Return list of camera dicts."""
    if yaml is None:
        raise RuntimeError("pyyaml not available — pip install pyyaml or "
                           "ensure Isaac Sim python env has it.")
    if not os.path.isfile(yaml_path):
        raise FileNotFoundError(f"YAML config not found: {yaml_path}")

    with open(yaml_path, "r") as f:
        cfg = yaml.safe_load(f)

    if not isinstance(cfg, dict) or "cameras" not in cfg:
        raise ValueError(f"{yaml_path}: missing top-level 'cameras' list")

    cameras = cfg["cameras"]
    if not isinstance(cameras, list) or len(cameras) == 0:
        raise ValueError(f"{yaml_path}: 'cameras' must be a non-empty list")

    seen_ports = set()
    seen_mounts = set()
    for i, cam in enumerate(cameras):
        for field in ("name", "camera_prim", "port", "mount_path"):
            if field not in cam:
                raise ValueError(f"{yaml_path}: cameras[{i}] missing '{field}'")

        port = cam["port"]
        if not isinstance(port, int) or not (1024 <= port <= 65535):
            raise ValueError(f"{yaml_path}: cameras[{i}].port invalid: {port}")
        if port in seen_ports:
            raise ValueError(f"{yaml_path}: duplicate port {port}")
        seen_ports.add(port)

        mount = cam["mount_path"]
        if not isinstance(mount, str) or not mount.startswith("/"):
            raise ValueError(f"{yaml_path}: cameras[{i}].mount_path must start with '/'")
        if mount in seen_mounts:
            print(f"[RTSP] WARN: duplicate mount_path '{mount}' across cameras")
        seen_mounts.add(mount)

    print(f"[RTSP] Loaded {len(cameras)} cameras from {yaml_path}")
    return cameras


def ensure_streaming_extension_enabled():
    ext_mgr = omni.kit.app.get_app().get_extension_manager()
    for ext_id in ["isaacsim.core.nodes", "isaacsim.streaming.rtsp"]:
        if not ext_mgr.is_extension_enabled(ext_id):
            ext_mgr.set_extension_enabled_immediate(ext_id, True)
            print(f"[RTSP] Enabled {ext_id}")


def verify_camera_prims(stage, cameras):
    missing = [c["camera_prim"] for c in cameras
               if not stage.GetPrimAtPath(c["camera_prim"])]
    if missing:
        raise RuntimeError(
            f"Missing camera prims on stage: {missing}\n"
            f"Open the matching scene, or edit the YAML to match actual camera prims."
        )
    print(f"[RTSP] {len(cameras)} camera prims verified")


def build_multi_cam_graph(cameras, resolution=(1280, 720),
                           use_raw_encoding=False,
                           graph_path="/World/RTSPMultiGraph"):
    """Build N-camera Action Graph: 1 OnPlaybackTick + N pairs of
    (CreateRenderProduct, RTSPCameraHelper)."""
    width, height = resolution
    keys = og.Controller.Keys

    create_nodes = [
        ("OnPlaybackTick", "omni.graph.action.OnPlaybackTick"),
    ]
    set_values = []
    connect = []

    for cam in cameras:
        safe_name = cam["name"].replace("/", "_").replace(".", "_").replace(" ", "_")
        rp_node = f"RP_{safe_name}"
        rtsp_node = f"RTSP_{safe_name}"

        create_nodes.append((rp_node, "isaacsim.core.nodes.IsaacCreateRenderProduct"))
        create_nodes.append((rtsp_node, "isaacsim.streaming.rtsp.RTSPCameraHelper"))

        set_values.extend([
            (f"{rp_node}.inputs:cameraPrim", cam["camera_prim"]),
            (f"{rp_node}.inputs:width", width),
            (f"{rp_node}.inputs:height", height),
            (f"{rtsp_node}.inputs:port", cam["port"]),
            (f"{rtsp_node}.inputs:mountPath", cam["mount_path"]),
            (f"{rtsp_node}.inputs:useRawEncoding", use_raw_encoding),
        ])

        connect.extend([
            ("OnPlaybackTick.outputs:tick", f"{rp_node}.inputs:execIn"),
            (f"{rp_node}.outputs:execOut", f"{rtsp_node}.inputs:execIn"),
            (f"{rp_node}.outputs:renderProductPath",
             f"{rtsp_node}.inputs:renderProductPath"),
        ])

    (graph, _, _, _) = og.Controller.edit(
        {"graph_path": graph_path, "evaluator_name": "execution"},
        {
            keys.CREATE_NODES: create_nodes,
            keys.SET_VALUES: set_values,
            keys.CONNECT: connect,
        },
    )

    print(f"[RTSP] Multi-cam Action Graph built at {graph_path}")
    print(f"[RTSP] Streams (once Play pressed):")
    for cam in cameras:
        print(f"  - {cam['name']}: rtsp://<host>:{cam['port']}{cam['mount_path']}")

    return graph


def main():
    config_path = os.environ.get("ISAAC_RTSP_CAMERAS_YAML", CONFIG_PATH)
    print(f"[RTSP] Config: {config_path}")

    cameras = load_cameras_yaml(config_path)
    ensure_streaming_extension_enabled()

    stage = omni.usd.get_context().get_stage()
    if stage is None:
        raise RuntimeError("No USD stage open. Open a scene first.")

    verify_camera_prims(stage, cameras)

    build_multi_cam_graph(
        cameras=cameras,
        resolution=RESOLUTION,
        use_raw_encoding=USE_RAW_ENCODING,
        graph_path=GRAPH_PATH,
    )

    print(f"[RTSP] Setup complete. Press Play → {len(cameras)} streams available.")


if __name__ == "__main__":
    main()
