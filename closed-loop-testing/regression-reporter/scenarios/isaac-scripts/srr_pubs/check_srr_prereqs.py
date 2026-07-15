# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""SRR — read-only pre-flight check for the /gt/* publishers (Isaac Sim 6.0 / IRA 1.6.x).

Paste into Isaac Sim's Script Editor with the scene loaded AND RUNNING (press
Play first): IRA spawns the pedestrians at runtime, so the target prims only
exist after setup. Prints PASS/FAIL per target so you can confirm the graph
`add_srr_gt_pubs.py` will build against the right prims.

Self-contained (does NOT import add_srr_gt_pubs.py, which arms an update-loop
subscription on import). Read-only: creates no nodes.

This is exactly the state add_srr_gt_pubs.py needs: it publishes each character
on a ROS2PublishRawTransformTree fed every frame from that ManRoot's
UsdGeom.ComputeLocalToWorldTransform (the check below computes the same pose), so
a PASS here means the /gt/character_<i> pump will track the walking character.

Checks:
  1. isaacsim.ros2.bridge extension enabled (else publishers won't work).
  2. Each SRR char group's ManRoot (the animated 6.0 skeleton root that BT
     MoveTo drives) is discovered under /World/Characters/<group>/<group>_0,
     is xformable, and reports a non-zero world pose (a zero pose usually means
     the char has not spawned / not been repositioned yet).
  3. Forklift /World/forklift_b exists + xformable.
Group order below MUST match add_srr_gt_pubs.DEFAULT_CHAR_GROUPS and the SRR
recorder's /gt/character_<i> topics.
"""
import omni.usd
from pxr import Usd, UsdGeom

# Keep in sync with add_srr_gt_pubs.DEFAULT_CHAR_GROUPS (positional -> /gt index).
CHAR_GROUPS = ["inspect_workers", "gather_workers", "pickup_workers"]
CHAR_ROOT_FMT = "/World/Characters/{group}/{group}_0"
SKEL_ROOT_NAME = "ManRoot"
FORKLIFT_PRIM = "/World/forklift_b"
NEW_GRAPH_PATH = "/World/SRRGraph"


def _ok(msg):   print(f"  PASS  {msg}")
def _bad(msg):  print(f"  FAIL  {msg}")
def _info(msg): print(f"  INFO  {msg}")


def _check_ext():
    print("[1] ROS2 bridge extension")
    try:
        import omni.kit.app
        mgr = omni.kit.app.get_app().get_extension_manager()
        ros2 = mgr.is_extension_enabled("isaacsim.ros2.bridge")
        if ros2:
            _ok("isaacsim.ros2.bridge enabled")
        else:
            _bad("isaacsim.ros2.bridge NOT enabled (add_srr_gt_pubs.py auto-enables it, "
                 "but a manual check helps)")
        return ros2
    except Exception as e:
        _bad(f"could not query extension manager: {e}")
        return False


def _find_skel_root(stage, group_root_path):
    root = stage.GetPrimAtPath(group_root_path)
    if not root or not root.IsValid():
        return None
    for prim in Usd.PrimRange(root):
        if prim.GetName() == SKEL_ROOT_NAME:
            return prim.GetPath().pathString
    return None


def _check_pose(stage, label, path):
    prim = stage.GetPrimAtPath(path)
    if not prim or not prim.IsValid():
        _bad(f"{label}: prim not found at {path}")
        return False
    _info(f"{label}: {path}  (type={prim.GetTypeName()})")
    xformable = UsdGeom.Xformable(prim)
    if not xformable:
        _bad(f"{label}: not Xformable — TF publisher will reject it")
        return False
    try:
        world = xformable.ComputeLocalToWorldTransform(Usd.TimeCode.Default())
        t = world.ExtractTranslation()
        _info(f"{label}: world pos = ({t[0]:.3f}, {t[1]:.3f}, {t[2]:.3f})")
        if abs(t[0]) < 1e-6 and abs(t[1]) < 1e-6 and abs(t[2]) < 1e-6:
            _bad(f"{label}: world pos is (0,0,0) — char likely not spawned/repositioned yet "
                 "(press Play + let IRA spawn), or wrong prim")
            return False
        _ok(f"{label}: xformable + non-zero world pose")
        return True
    except Exception as e:
        _bad(f"{label}: ComputeLocalToWorldTransform failed: {e}")
        return False


def main():
    print("=" * 60)
    print("SRR pre-flight check (IRA 6.0) — read-only, no graph created")
    print("=" * 60)

    ros2_ok = _check_ext()
    print()

    stage = omni.usd.get_context().get_stage()
    if stage is None:
        _bad("No stage loaded — open the SRR scene first")
        return

    sg = stage.GetPrimAtPath(NEW_GRAPH_PATH)
    if sg and sg.IsValid():
        _info(f"{NEW_GRAPH_PATH} already exists — add_srr_gt_pubs.py will rebuild it (idempotent)")
    print()

    print("[2] SRR character ManRoots (discovered under each IRA group)")
    results = []
    for i, group in enumerate(CHAR_GROUPS):
        group_root = CHAR_ROOT_FMT.format(group=group)
        skel = _find_skel_root(stage, group_root)
        if skel is None:
            _bad(f"character_{i} ({group}): no '{SKEL_ROOT_NAME}' under {group_root} "
                 "(char not spawned? press Play / check config groups)")
            results.append(False)
            continue
        results.append(_check_pose(stage, f"character_{i} ({group})", skel))
    print()

    print("[3] Forklift")
    results.append(_check_pose(stage, "forklift", FORKLIFT_PRIM))
    print()

    print("Summary")
    if ros2_ok and all(results):
        print("  ALL PASS — add_srr_gt_pubs.py will build /World/SRRGraph cleanly")
    else:
        print("  ATTENTION — fix the FAIL lines above (usually: press Play so IRA spawns "
              "the chars, or the config character groups don't match CHAR_GROUPS)")


main()
