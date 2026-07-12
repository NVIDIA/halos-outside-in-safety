# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Halos SIL RTSP multi-camera Action Graph builder.

Builds a single OmniGraph with:
  - 1 OnPlaybackTick (shared trigger)
  - N pairs of (IsaacCreateRenderProduct, RTSPCameraHelper),
    one per camera in cameras.yaml

After Play, each camera streams h264 1280x720 @ 30fps with SEI
per-frame metadata (sim time, frame_num, ISO8601 timestamp,
UUID aa71e48f-0711-5d80-a247-cd31ca6fa49c) to its own port.

Fired from `run_actor_sdg.py`'s `SET_UP_SIMULATION_DONE_EVENT`
callback (or directly via the `__main__` block in Script Editor).

Requires the `camera_prim` paths to already exist on the stage (it
fail-fasts otherwise). The Halos SIL warehouse scene no longer bakes
cameras — run_actor_sdg.py spawns them via camera_loader before calling
this builder; a standalone Script Editor run must spawn them first too.

YAML schema (Halos SIL cameras.yaml 6.0):

    cameras:
      - name: Camera
        camera_prim: /World/Cameras/Camera
        port: 8554
        mount_path: /camera
      - name: Camera_01
        ...
    rtsp:
      host: ${HOST_IP}             # informational, used by VST URL builder

See also:
  - `closed-loop-testing/isaac-sim/sil/configs/cameras.yaml`
"""

from __future__ import annotations

import os

# Default container path to the canonical Halos cameras.yaml. Override
# via env var ISAAC_RTSP_CAMERAS_YAML or by passing config_path to the
# function directly.
DEFAULT_CAMERAS_YAML = "/isaac-sim/sil/configs/cameras.yaml"

# Default graph path under /World/. Operators looking for the graph in
# the Stage panel will find it predictably here.
DEFAULT_GRAPH_PATH = "/World/RTSPMultiGraph"

# Default render resolution. Matches Halos 5.1 (`--width 1920 --height 1080`)
# and DS [streammux] expected input width=1920, height=1080. Mismatch shifts
# bbox overlay on the VST UI.
DEFAULT_RESOLUTION = (1920, 1080)

# False -> NVENC h264 (recommended, what production uses).
# True  -> raw uncompressed (debug only, not consumable across host).
DEFAULT_USE_RAW_ENCODING = False


def _load_and_validate_cameras_yaml(yaml_path: str) -> list[dict]:
    """Parse cameras.yaml and validate per-camera fields. Returns the
    cameras list. Raises with a descriptive error on schema mismatch.
    """
    try:
        import yaml
    except ImportError as e:
        raise RuntimeError(
            "pyyaml not available; pip install pyyaml or ensure Isaac Sim "
            "python env has it"
        ) from e

    if not os.path.isfile(yaml_path):
        raise FileNotFoundError(f"cameras yaml not found: {yaml_path}")

    with open(yaml_path) as f:
        cfg = yaml.safe_load(f)

    if not isinstance(cfg, dict) or "cameras" not in cfg:
        raise ValueError(f"{yaml_path}: missing top-level 'cameras' list")

    cameras = cfg["cameras"]
    if not isinstance(cameras, list) or not cameras:
        raise ValueError(f"{yaml_path}: 'cameras' must be a non-empty list")

    seen_ports: set[int] = set()
    for i, cam in enumerate(cameras):
        for field in ("name", "camera_prim", "port", "mount_path"):
            if field not in cam:
                raise ValueError(f"{yaml_path}: cameras[{i}] missing field '{field}'")
        port = cam["port"]
        if not isinstance(port, int) or not (1024 <= port <= 65535):
            raise ValueError(f"{yaml_path}: cameras[{i}].port invalid: {port}")
        if port in seen_ports:
            raise ValueError(f"{yaml_path}: duplicate port {port}")
        seen_ports.add(port)
        mount = cam["mount_path"]
        if not isinstance(mount, str) or not mount.startswith("/"):
            raise ValueError(
                f"{yaml_path}: cameras[{i}].mount_path must start with '/'"
            )

    return cameras


def _ensure_extensions_enabled() -> None:
    """Enable isaacsim.core.nodes + isaacsim.streaming.rtsp if not yet
    on. Idempotent.
    """
    import omni.kit.app

    ext_mgr = omni.kit.app.get_app().get_extension_manager()
    for ext_id in ("isaacsim.core.nodes", "isaacsim.streaming.rtsp"):
        if not ext_mgr.is_extension_enabled(ext_id):
            ext_mgr.set_extension_enabled_immediate(ext_id, True)
            print(f"[rtsp-cameras] Enabled extension: {ext_id}")


def _verify_camera_prims_exist(stage, cameras: list[dict]) -> None:
    missing = [c["camera_prim"] for c in cameras if not stage.GetPrimAtPath(c["camera_prim"])]
    if missing:
        raise RuntimeError(
            f"[rtsp-cameras] Missing camera prims on stage: {missing}\n"
            f"Open the matching scene, or edit cameras.yaml to match actual prim paths."
        )


def _clear_existing_graph(stage, graph_path: str) -> None:
    """Idempotency guard - delete any prior graph at graph_path so
    re-running setup in UI mode does not produce duplicate-node errors.
    """
    prim = stage.GetPrimAtPath(graph_path)
    if prim and prim.IsValid():
        stage.RemovePrim(graph_path)
        print(f"[rtsp-cameras] Cleared existing graph at {graph_path}")


def build_rtsp_graph(
    config_path: str = DEFAULT_CAMERAS_YAML,
    *,
    graph_path: str = DEFAULT_GRAPH_PATH,
    resolution: tuple[int, int] = DEFAULT_RESOLUTION,
    use_raw_encoding: bool = DEFAULT_USE_RAW_ENCODING,
):
    """Build the N-camera RTSP Action Graph. Returns the og.Graph
    object so caller can introspect / debug.

    Idempotent: if a graph already exists at `graph_path`, it is
    removed before the new one is built.

    Args:
        config_path: path to cameras.yaml (default: container-canonical
            path inside the SIL container).
        graph_path: USD path where the graph prim is created.
        resolution: (width, height) of the render product per camera.
        use_raw_encoding: pass-through to RTSPCameraHelper.inputs:useRawEncoding.
    """
    import omni.graph.core as og
    import omni.usd

    cameras = _load_and_validate_cameras_yaml(config_path)
    print(f"[rtsp-cameras] Loaded {len(cameras)} cameras from {config_path}")

    _ensure_extensions_enabled()

    stage = omni.usd.get_context().get_stage()
    if stage is None:
        raise RuntimeError("No USD stage open. Load a scene first.")

    _verify_camera_prims_exist(stage, cameras)
    _clear_existing_graph(stage, graph_path)

    width, height = resolution
    keys = og.Controller.Keys

    # OnPlaybackTick fires at the render-loop rate (~60 Hz). We feed it through
    # `IsaacSimulationGate` with `step=2` so downstream IsaacCreateRenderProduct
    # only fires every 2nd tick (= 30 Hz), matching the sim/BT rate. Without
    # this divider the encoder duplicates frames on the RTSP wire and the
    # downstream DS/VST pipeline backlogs.
    create_nodes = [
        ("OnPlaybackTick", "omni.graph.action.OnPlaybackTick"),
        ("RateDivider", "isaacsim.core.nodes.IsaacSimulationGate"),
    ]
    set_values = [
        ("RateDivider.inputs:step", 1),
    ]
    connect = [
        ("OnPlaybackTick.outputs:tick", "RateDivider.inputs:execIn"),
    ]

    for cam in cameras:
        safe = cam["name"].replace("/", "_").replace(".", "_").replace(" ", "_")
        rp_node = f"RP_{safe}"
        rtsp_node = f"RTSP_{safe}"

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
            ("RateDivider.outputs:execOut", f"{rp_node}.inputs:execIn"),
            (f"{rp_node}.outputs:execOut", f"{rtsp_node}.inputs:execIn"),
            (f"{rp_node}.outputs:renderProductPath", f"{rtsp_node}.inputs:renderProductPath"),
        ])

    (graph, _, _, _) = og.Controller.edit(
        {"graph_path": graph_path, "evaluator_name": "execution"},
        {
            keys.CREATE_NODES: create_nodes,
            keys.SET_VALUES: set_values,
            keys.CONNECT: connect,
        },
    )

    print(f"[rtsp-cameras] Action Graph built at {graph_path}")
    print(f"[rtsp-cameras] Streams (once Play is pressed):")
    for cam in cameras:
        print(f"  - {cam['name']}: rtsp://<host>:{cam['port']}{cam['mount_path']}")

    return graph


if __name__ == "__main__":
    # Allow paste-into-Script-Editor invocation for manual testing.
    cfg = os.environ.get("ISAAC_RTSP_CAMERAS_YAML", DEFAULT_CAMERAS_YAML)
    build_rtsp_graph(cfg)
