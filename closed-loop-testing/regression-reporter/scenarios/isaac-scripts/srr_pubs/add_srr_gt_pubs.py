# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""SRR ground-truth `/gt/*/tf` publisher Action Graph — Isaac Sim 6.0 / IRA 1.6.x.

Builds `/World/SRRGraph`: one `OnPlaybackTick` + one `ROS2Context`, then
  - the FORKLIFT (scene-baked) on a `ROS2PublishTransformTree` (reads the prim
    transform straight from Fabric — works because the payload is present in
    Fabric from load), publishing `/gt/forklift/tf` (child frames body + lift);
  - each SRR PEDESTRIAN on a `ROS2PublishRawTransformTree`, publishing
    `/gt/character_<i>/tf` (parent "world"). The raw node takes an EXPLICIT
    translation input, which we feed each frame from the character's live
    FABRIC world transform (see the pump below).
SRR-OWNED; Halos' `run_actor_sdg.py` is NOT modified.

Why the characters need the RAW node + a FABRIC-fed pump:
  IRA 6.0 spawns the walking pedestrians at RUNTIME under
  `/World/Characters/<group>/<group>_0`. Their locomotion (MoveTo + motion
  library) is applied in FABRIC only — the USD *authored* transform stays at
  the spawn pose, so `UsdGeom.ComputeLocalToWorldTransform(Default)` reads a
  FROZEN position. That silently broke /gt: the chars looked static in the
  recording while the render + VSS perception saw them walk 30-90 m (every
  char detection then mismatched GT -> false-positive / tracker-miss). The
  live pose lives on the skinned render meshes' `omni:fabric:worldMatrix`.
  `ROS2PublishTransformTree` (PoseTree) can't resolve these runtime-spawned
  prims (`getObjectType` eInvalid), so we read the Fabric world transform
  ourselves via usdrt and push it onto the raw node each frame. The baked
  forklift IS in Fabric for PoseTree, so it keeps the simpler
  ROS2PublishTransformTree.

TWO ways this runs (both call `build_srr_graph()`):
  1. Automated (run_multi.sh): Isaac is launched with
        ./python.sh run_actor_sdg.py ... --exec scripts/isaac/add_srr_gt_pubs.py
     `--exec` is a Kit startup arg; run_actor_sdg.py parses its own flags with
     argparse.parse_known_args() and forwards leftover argv to Kit, so Kit runs
     THIS file at boot — no Halos code change. `--exec` fires at boot, BEFORE IRA
     spawns the characters, so the module bottom arms an app-update-loop poll and
     builds ONCE the chars are on stage; that SAME subscription then stays alive
     and pumps the per-frame USD transforms into the raw publishers.
  2. Manual: paste into Isaac's Script Editor with the scene loaded + running.

Prim targets:
  - Characters: the `ManRoot` prim discovered under each
    `/World/Characters/<group>/<group>_0` (group order below == /gt index).
  - Forklift: `/World/forklift_b` (child frame `body`, which the recorder reads).
Frames: chars -> parent "world", child "character_<i>"; forklift -> parent
"world", children "body"/"lift". The SRR recorder latches whatever child frame
arrives under parent "world", so the exact child string is not load-bearing.
"""

from __future__ import annotations

import os

# Default graph path under /World/ so operators find it in the Stage panel.
DEFAULT_GRAPH_PATH = "/World/SRRGraph"

# Character group -> /gt topic index mapping. Order MUST match
# runtime_patches._HALOS_CHAR_SPAWN_TARGETS (Halos repositions these) and the SRR
# recorder's CHAR_TOPICS = [/gt/character_0, /gt/character_1, /gt/character_2].
# Index is positional: groups[i] -> /gt/character_<i>/tf.
DEFAULT_CHAR_GROUPS = ["inspect_workers", "gather_workers", "pickup_workers"]

# Container path to the IRA spawned-character root for a given group.
_CHAR_ROOT_FMT = "/World/Characters/{group}/{group}_0"

# Skeleton-root prim name to target under each character root (6.0 == "ManRoot").
_SKEL_ROOT_NAME = "ManRoot"

# Forklift ground-truth publisher.
_FORKLIFT_PRIM = "/World/forklift_b"
_FORKLIFT_NAMESPACE = "/gt/forklift"

# The recorder only latches a character's child frame when the parent frame is
# exactly this string (see srr-service service.py WORLD_FRAME).
_WORLD_FRAME = "world"


def _ensure_extensions_enabled() -> None:
    """Enable isaacsim.ros2.bridge if not yet on. Idempotent."""
    import omni.kit.app

    ext_mgr = omni.kit.app.get_app().get_extension_manager()
    for ext_id in ("isaacsim.ros2.bridge",):
        if not ext_mgr.is_extension_enabled(ext_id):
            ext_mgr.set_extension_enabled_immediate(ext_id, True)
            print(f"[srr-gt] Enabled extension: {ext_id}")


def _find_skel_root(stage, group_root_path: str) -> str | None:
    """Return the path of the first descendant prim named `_SKEL_ROOT_NAME`
    under `group_root_path`, or None if the group root is missing / has no
    such descendant.
    """
    from pxr import Usd

    root = stage.GetPrimAtPath(group_root_path)
    if not root or not root.IsValid():
        return None
    for prim in Usd.PrimRange(root):
        if prim.GetName() == _SKEL_ROOT_NAME:
            return prim.GetPath().pathString
    return None


def _resolve_targets(stage, char_groups: list[str]) -> tuple[list[tuple[str, str]], str]:
    """Resolve publisher targets on the live stage.

    Returns (char_targets, forklift_path) where char_targets is an ordered list
    of (nodeNamespace, ManRoot_prim_path) for character_0, character_1, ...
    Raises RuntimeError listing every missing prim so failures surface loudly
    rather than silently misaligning the /gt/character_<i> indices the SRR
    recorder depends on.
    """
    char_targets: list[tuple[str, str]] = []
    missing: list[str] = []

    for i, group in enumerate(char_groups):
        group_root = _CHAR_ROOT_FMT.format(group=group)
        skel = _find_skel_root(stage, group_root)
        if skel is None:
            missing.append(f"{group_root} (or its '{_SKEL_ROOT_NAME}' descendant)")
            continue
        char_targets.append((f"/gt/character_{i}", skel))

    if not stage.GetPrimAtPath(_FORKLIFT_PRIM):
        missing.append(_FORKLIFT_PRIM)

    if missing:
        raise RuntimeError(
            "[srr-gt] Missing prims on stage:\n  - "
            + "\n  - ".join(missing)
            + "\nThe SRR graph must be built AFTER setup_simulation() + "
            "apply_halos_runtime_patches(). Check IRA spawned the character "
            "groups and the scene contains the forklift."
        )
    return char_targets, _FORKLIFT_PRIM


def _clear_existing_graph(stage, graph_path: str) -> None:
    """Idempotency guard - delete any prior graph at graph_path."""
    prim = stage.GetPrimAtPath(graph_path)
    if prim and prim.IsValid():
        stage.RemovePrim(graph_path)
        print(f"[srr-gt] Cleared existing graph at {graph_path}")


# --- per-frame Fabric -> raw-publisher pump ---------------------------------
# The characters publish via ROS2PublishRawTransformTree, which takes an
# explicit translation/rotation. IRA drives the walking pose in FABRIC only
# (the USD *authored* transform stays frozen at spawn), so each app-update
# frame we read the character's live Fabric worldMatrix (via usdrt) and push
# it onto the node inputs. This is the whole fix: reading USD-Default froze
# /gt at spawn while render + perception saw the char walk 30-90 m.
_pump: list[dict] = []      # [{src, ns, t_attr, r_attr}, ...]
_meters_per_unit = 1.0
_rt_stage = None            # cached usdrt stage handle


def _find_fabric_pump_prim(stage, skel_root_path: str) -> str:
    """Pick the prim whose FABRIC worldMatrix tracks the walking character.

    IRA applies locomotion in Fabric, and the skinned render meshes carry the
    live world transform (their `omni:fabric:worldMatrix` moves with the body,
    origin on the ground plane == the character's x/y). We prefer the first
    UsdGeom.Mesh under the skeleton root; if there is none we fall back to the
    skel root path itself (still read from Fabric below).
    """
    from pxr import Usd, UsdGeom

    root = stage.GetPrimAtPath(skel_root_path)
    if root and root.IsValid():
        for prim in Usd.PrimRange(root):
            if prim.IsA(UsdGeom.Mesh):
                return prim.GetPath().pathString
    return skel_root_path


def _cache_pump(stage, graph_path: str, char_targets: list[tuple[str, str]]) -> None:
    """Cache the OG input-attribute handles + Fabric source prim for each char."""
    import omni.graph.core as og
    from pxr import UsdGeom

    global _pump, _meters_per_unit, _rt_stage
    _pump = []
    _rt_stage = None  # re-attach lazily against the current stage id
    _meters_per_unit = float(UsdGeom.GetStageMetersPerUnit(stage) or 1.0)
    for idx, (ns, skel_path) in enumerate(char_targets):
        node = f"{graph_path}/RawPub{idx}"
        src = _find_fabric_pump_prim(stage, skel_path)
        _pump.append({
            "src": src,
            "ns": ns,
            "t_attr": og.Controller.attribute(f"{node}.inputs:translation"),
            "r_attr": og.Controller.attribute(f"{node}.inputs:rotation"),
        })
        print(f"[srr-gt] pump {ns} <- Fabric worldMatrix of {src}")


def _pump_frame(stage) -> None:
    """Push each character's live FABRIC world transform onto its raw node.

    Reads `omni:fabric:worldMatrix` via usdrt (the animated runtime transform,
    NOT the frozen USD-authored one). Translation is row 3; rotation is derived
    from the upper 3x3 (IJKR = [x, y, z, w]). The SRR recorder only latches the
    translation, so rotation is best-effort and defaults to identity on error.
    """
    import omni.usd
    import usdrt
    from pxr import Gf

    global _rt_stage
    try:
        if _rt_stage is None:
            _rt_stage = usdrt.Usd.Stage.Attach(omni.usd.get_context().get_stage_id())
        rt = _rt_stage
    except Exception:
        return

    mpu = _meters_per_unit
    for tgt in _pump:
        try:
            prim = rt.GetPrimAtPath(tgt["src"])
            if not prim or not prim.IsValid():
                continue
            attr = prim.GetAttribute("omni:fabric:worldMatrix")
            if not attr or not attr.HasValue():
                continue
            m = attr.Get()
            r3 = m.GetRow(3)  # translation lives in the last row
            tgt["t_attr"].set([r3[0] * mpu, r3[1] * mpu, r3[2] * mpu])
            try:
                r0, r1, r2 = m.GetRow(0), m.GetRow(1), m.GetRow(2)
                gm = Gf.Matrix4d(
                    r0[0], r0[1], r0[2], 0.0,
                    r1[0], r1[1], r1[2], 0.0,
                    r2[0], r2[1], r2[2], 0.0,
                    0.0, 0.0, 0.0, 1.0,
                )
                q = gm.RemoveScaleShear().ExtractRotationQuat()
                im = q.GetImaginary()
                tgt["r_attr"].set([im[0], im[1], im[2], q.GetReal()])
            except Exception:
                tgt["r_attr"].set([0.0, 0.0, 0.0, 1.0])
        except Exception:
            # Never wedge the update loop on a transient stage / Fabric state.
            pass


def build_srr_graph(
    config_path: str | None = None,
    *,
    graph_path: str = DEFAULT_GRAPH_PATH,
    char_groups: list[str] | None = None,
):
    """Build the SRR ground-truth /gt/*/tf publisher Action Graph.

    Characters -> ROS2PublishRawTransformTree (fed each frame from the live
    Fabric worldMatrix, see _pump_frame); forklift -> ROS2PublishTransformTree
    (reads Fabric directly via PoseTree).

    Args:
        config_path: accepted for signature parity with the Halos builders
            (build_rtsp_graph etc.); unused today. The character groups +
            forklift prim are resolved from the live stage.
        graph_path: USD path where the graph prim is created.
        char_groups: ordered character group names mapped positionally to
            /gt/character_<i>. Defaults to DEFAULT_CHAR_GROUPS.

    Returns the og.Graph object so callers can introspect / debug.
    Idempotent: an existing graph at `graph_path` is removed first.
    """
    import omni.graph.core as og
    import omni.usd
    import usdrt

    groups = char_groups if char_groups is not None else DEFAULT_CHAR_GROUPS

    _ensure_extensions_enabled()

    stage = omni.usd.get_context().get_stage()
    if stage is None:
        raise RuntimeError("[srr-gt] No USD stage open. Load a scene first.")

    char_targets, forklift_path = _resolve_targets(stage, groups)
    _clear_existing_graph(stage, graph_path)

    keys = og.Controller.Keys
    create_nodes = [
        ("OnTick", "omni.graph.action.OnPlaybackTick"),
        ("Context", "isaacsim.ros2.bridge.ROS2Context"),
    ]
    set_values = []
    connect = []

    # Characters: raw publishers driven from USD (Fabric-independent).
    for idx, (namespace, _prim_path) in enumerate(char_targets):
        node = f"RawPub{idx}"
        create_nodes.append((node, "isaacsim.ros2.bridge.ROS2PublishRawTransformTree"))
        set_values.extend([
            (f"{node}.inputs:nodeNamespace", namespace),
            (f"{node}.inputs:topicName", "tf"),
            (f"{node}.inputs:parentFrameId", _WORLD_FRAME),
            (f"{node}.inputs:childFrameId", f"character_{idx}"),
        ])
        connect.extend([
            ("OnTick.outputs:tick", f"{node}.inputs:execIn"),
            ("OnTick.outputs:time", f"{node}.inputs:timeStamp"),
            ("Context.outputs:context", f"{node}.inputs:context"),
        ])

    # Forklift: scene-baked -> Fabric read works, keep the transform-tree node.
    create_nodes.append(("PubForklift", "isaacsim.ros2.bridge.ROS2PublishTransformTree"))
    set_values.extend([
        ("PubForklift.inputs:nodeNamespace", _FORKLIFT_NAMESPACE),
        ("PubForklift.inputs:topicName", "tf"),
        ("PubForklift.inputs:targetPrims", [usdrt.Sdf.Path(forklift_path)]),
    ])
    connect.extend([
        ("OnTick.outputs:tick", "PubForklift.inputs:execIn"),
        ("OnTick.outputs:time", "PubForklift.inputs:timeStamp"),
        ("Context.outputs:context", "PubForklift.inputs:context"),
    ])

    (graph, _, _, _) = og.Controller.edit(
        {"graph_path": graph_path, "evaluator_name": "execution"},
        {
            keys.CREATE_NODES: create_nodes,
            keys.SET_VALUES: set_values,
            keys.CONNECT: connect,
        },
    )

    _cache_pump(stage, graph_path, char_targets)

    print(f"[srr-gt] Action Graph built at {graph_path} "
          f"({len(char_targets)} raw char pubs + 1 forklift, "
          f"metersPerUnit={_meters_per_unit})")
    for namespace, prim_path in char_targets:
        print(f"  - {namespace}/tf  (raw, Fabric-pumped)  <-  {prim_path}")
    print(f"  - {_FORKLIFT_NAMESPACE}/tf  (transform-tree)  <-  {forklift_path}")
    print("[srr-gt] Press Play; then `ros2 topic echo /gt/character_0/tf` on the host.")

    return graph


# --- auto-trigger + pump ----------------------------------------------------
# `--exec` fires at Kit boot, before IRA spawns the pedestrians, so we can't just
# call build_srr_graph() here. Instead subscribe to the app update loop and poll
# cheaply each frame until all SRR char groups + the forklift are on the stage,
# then build ONCE. After building, the SAME subscription keeps running and pumps
# the per-frame live Fabric transforms into the raw char publishers (_pump_frame).
# On a non-SRR run the gate never trips (chars absent) -> a no-op that
# self-cancels after a frame budget. We poll rather than bind IRA's internal
# SET_UP_SIMULATION_DONE_EVENT (private API, not importable this early).
_MAX_FRAMES = int(os.environ.get("SRR_GT_BOOTSTRAP_MAX_FRAMES", "36000"))  # ~10 min @60fps
_sub = None  # keep the update subscription alive (module-global)
_frames = 0
_built = False


def _drop_subscription() -> None:
    global _sub
    _sub = None  # dropping the last ref cancels the update subscription


def _stage_ready(stage) -> bool:
    """True once every SRR char group's ManRoot + the forklift are on the stage
    (exactly what build_srr_graph() needs to succeed / not raise)."""
    for group in DEFAULT_CHAR_GROUPS:
        if _find_skel_root(stage, _CHAR_ROOT_FMT.format(group=group)) is None:
            return False
    fk = stage.GetPrimAtPath(_FORKLIFT_PRIM)
    return bool(fk and fk.IsValid())


def _on_update(_event) -> None:
    global _frames, _built

    import omni.usd

    stage = omni.usd.get_context().get_stage()

    # Built already: keep pumping Fabric -> raw char publishers every frame.
    if _built:
        if stage is not None:
            _pump_frame(stage)
        return

    _frames += 1
    if stage is not None and _stage_ready(stage):
        try:
            build_srr_graph()
            print("[srr-gt] SRRGraph built (SRR chars detected on stage); "
                  "now pumping live Fabric transforms into raw char publishers")
        except Exception as err:  # never wedge the run
            print(f"[srr-gt] build_srr_graph failed: {err}")
        _built = True
        return

    if _frames >= _MAX_FRAMES:
        print(
            f"[srr-gt] SRR chars not found after {_frames} frames — "
            "no-op (non-SRR run?); cancelling."
        )
        _drop_subscription()


def _arm() -> None:
    global _sub
    import omni.kit.app

    _sub = (
        omni.kit.app.get_app()
        .get_update_event_stream()
        .create_subscription_to_pop(_on_update, name="srr_gt_add_pubs")
    )
    print("[srr-gt] armed; will build /World/SRRGraph once SRR chars spawn, "
          "then pump USD transforms each frame")


# Runs on --exec (Kit boot) and on Script-Editor paste alike.
_arm()
