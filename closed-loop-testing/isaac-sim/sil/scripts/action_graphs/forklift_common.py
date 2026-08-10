# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Shared helpers for the per-robot forklift OmniGraph builders.

The forklift logic that used to be baked into the scene USD
(ROS_Forklift_Control_Graph / Odometry_Graph / Safety_indicator_Graph)
is split across three sibling builder modules, all driven by robots.yaml:

  - forklift_control.py            -> build_control_graph
  - forklift_odometry.py           -> build_odometry_graph
  - forklift_safety_indicator.py   -> build_safety_graph

This module holds the bits they all share (YAML load/validate, extension
enable, prim verify, idempotent graph clear, relationship-target set), plus
`strip_baked_scene_graphs()` and the `build_forklift_graphs()` orchestrator.

YAML schema: see sil/configs/robots.yaml.
"""

from __future__ import annotations

import os

DEFAULT_ROBOTS_YAML = "/isaac-sim/sil/configs/robots.yaml"

# Baked OmniGraph prims this refactor replaces. The production scene
# (indicator_warehouse_20x20_layout_overflow_test.usd) baked
# everything into a single /World/ActionGraph; the older split-graph
# scene used the named graphs below. strip_baked_scene_graphs() removes
# whichever are present so the Python builders are the single source of
# truth and the logic does not run twice (baked + rebuilt).
#
# NOTE: this strips the scene's own `def OmniGraph` prims only. It does
# NOT touch `over "Nova_Carter_ROS"` (sensor/IMU/lidar/diff-drive graphs
# baked INTO the referenced robot asset) - those are asset-coupled and
# out of scope for this refactor.
_BAKED_GRAPH_PATHS = (
    "/World/ActionGraph",
    "/World/ROS_Forklift_Control_Graph",
    "/World/Safety_indicator_Graph",
    "/World/Old_Playback_Graph",
    "/World/Odometry_Graph",
    "/World/Clock_Publisher_Graph",
)


# Which control-graph topology a model needs. A value is not a label but a
# choice of builder: a differential-drive AMR is not a swivel truck with
# different numbers, it is a different graph. Registered in forklift_control.py;
# named here so the loader can reject an unknown value before anything is built.
KNOWN_DRIVE_TYPES = ("swivel",)
DEFAULT_DRIVE_TYPE = "swivel"

# Model keys that describe how the truck drives, and therefore land in the
# robot's `control` block. Everything here follows from the asset: two trucks
# built from one ForkliftB payload cannot disagree about their wheelbase.
_MODEL_CONTROL_KEYS = (
    "drive_type", "drive_joint", "swivel_joint",
    "wheelbase", "wheel_radius", "max_steer_deg", "reverse_logic",
)
_MODEL_INDICATOR_MESH_KEYS = ("radius", "segments", "height_offset")


def _merge_under(robot: dict, section: str, defaults: dict) -> None:
    """Apply model defaults to one robot section, instance keys winning.

    Merged leaf by leaf rather than section by section, so a robot can override
    a single number — `max_steer_deg` for a truck with a worn steering stop, say
    — without having to restate the whole model.
    """
    if not defaults:
        return
    current = dict(robot.get(section) or {})
    for key, value in defaults.items():
        if key not in current:
            current[key] = value
        elif isinstance(value, dict) and isinstance(current[key], dict):
            current[key] = {**value, **current[key]}
    robot[section] = current


def _apply_model(robot: dict, model: dict, yaml_path: str) -> None:
    """Fold a `models:` entry into one robot, in place.

    The robot ends up in exactly the flat shape the builders already read, so
    the model split is a loader concern and nothing downstream changes. That is
    also what keeps a config written before `models:` working untouched: it
    simply arrives already flat.
    """
    _merge_under(robot, "control",
                 {k: model[k] for k in _MODEL_CONTROL_KEYS if k in model})

    if "robot_front" in model:
        _merge_under(robot, "odometry", {"robot_front": model["robot_front"]})

    indicator = model.get("indicator") or {}
    if not isinstance(indicator, dict):
        raise ValueError(f"{yaml_path}: model indicator: must be a mapping, got {indicator!r}")

    mesh = {k: indicator[k] for k in _MODEL_INDICATOR_MESH_KEYS if k in indicator}
    if mesh:
        _merge_under(robot, "safety_indicator", {"mesh": mesh})

    # The disc path is the robot's prim plus wherever this model hangs its
    # bodywork — one string that used to be spelled out per truck and is the
    # kind of thing that goes wrong by a single typo.
    parent_rel = indicator.get("parent_prim_rel")
    if parent_rel:
        _merge_under(robot, "safety_indicator", {
            "indicator_prim": f"{robot['articulation_prim']}/{parent_rel.strip('/')}"
                              f"/safety_indicator",
        })

    if "asset_path" in model and isinstance(robot.get("spawn"), dict):
        _merge_under(robot, "spawn", {"asset_path": model["asset_path"]})


def _validate_models(cfg: dict, yaml_path: str) -> dict:
    models = cfg.get("models") or {}
    if not isinstance(models, dict):
        raise ValueError(f"{yaml_path}: 'models' must be a mapping of name -> model")
    for name, model in models.items():
        if not isinstance(model, dict):
            raise ValueError(f"{yaml_path}: models['{name}'] must be a mapping")
        # Required here but merely defaulted for a robot that names no model:
        # anyone writing the new construct states the topology explicitly, while
        # configs that predate the key keep the only behaviour they ever had.
        drive_type = model.get("drive_type")
        if drive_type is None:
            raise ValueError(
                f"{yaml_path}: models['{name}'] must state drive_type "
                f"(one of {', '.join(KNOWN_DRIVE_TYPES)}) — it selects which control "
                f"graph is built, and guessing it is how a truck ends up with a "
                f"steering graph it has no steering joint for"
            )
        if drive_type not in KNOWN_DRIVE_TYPES:
            raise ValueError(
                f"{yaml_path}: models['{name}'].drive_type is {drive_type!r}; known "
                f"values are {', '.join(KNOWN_DRIVE_TYPES)}. A new kinematic class "
                f"needs its own builder registered in forklift_control.py, not a new "
                f"set of numbers"
            )
    return models


def resolve_drive_type(robot: dict) -> str:
    """Which control-graph builder this robot needs.

    Absent means swivel, which is the only topology that has ever been built —
    so a config written before the key behaves as it always did. A robot that
    reaches here through a `models:` entry always has one, because the model
    schema requires it.
    """
    drive_type = (robot.get("control") or {}).get("drive_type", DEFAULT_DRIVE_TYPE)
    if drive_type not in KNOWN_DRIVE_TYPES:
        raise ValueError(
            f"robots.yaml: '{robot.get('name', '?')}' asks for drive_type "
            f"{drive_type!r}; known values are {', '.join(KNOWN_DRIVE_TYPES)}"
        )
    return drive_type


def load_and_validate_robots_yaml(yaml_path: str) -> tuple[list[dict], dict]:
    """Parse robots.yaml. Returns (robots_list, clock_cfg). Raises with a
    descriptive error on schema mismatch.

    A robot naming a `model:` is returned with that model already folded in, in
    the same flat shape a robot that spells everything out arrives in. Both
    schemas are therefore live at once, and every validation below runs on the
    merged result rather than on whichever half happened to be written.

    Anything reading robots.yaml for more than a robot's own keys must come
    through here. A private `yaml.safe_load` sees the unmerged file, and the
    symptom lands nowhere near the cause: a value that lives on the model reads
    as absent, and the consumer concludes the robot did not ask for the thing.
    """
    try:
        import yaml
    except ImportError as e:
        raise RuntimeError(
            "pyyaml not available; ensure the Isaac Sim python env has it"
        ) from e

    if not os.path.isfile(yaml_path):
        raise FileNotFoundError(f"robots yaml not found: {yaml_path}")

    with open(yaml_path) as f:
        cfg = yaml.safe_load(f)

    if not isinstance(cfg, dict) or "robots" not in cfg:
        raise ValueError(f"{yaml_path}: missing top-level 'robots' list")

    robots = cfg["robots"]
    if not isinstance(robots, list) or not robots:
        raise ValueError(f"{yaml_path}: 'robots' must be a non-empty list")

    models = _validate_models(cfg, yaml_path)

    seen_names: set[str] = set()
    for i, r in enumerate(robots):
        for field in ("name", "articulation_prim"):
            if field not in r:
                raise ValueError(f"{yaml_path}: robots[{i}] missing field '{field}'")
        name = r["name"]
        if name in seen_names:
            raise ValueError(f"{yaml_path}: duplicate robot name '{name}'")
        seen_names.add(name)
        if not str(r["articulation_prim"]).startswith("/"):
            raise ValueError(
                f"{yaml_path}: robots[{i}].articulation_prim must be an absolute prim path"
            )

        model_name = r.get("model")
        if model_name is not None:
            if model_name not in models:
                raise ValueError(
                    f"{yaml_path}: '{name}' names model {model_name!r}, which is not "
                    f"declared. Known models: {', '.join(sorted(models)) or '<none>'}"
                )
            _apply_model(r, models[model_name], yaml_path)

        resolve_drive_type(r)

    clock_cfg = cfg.get("clock", {}) or {}
    return robots, clock_cfg


# Safety-indicator appearance, shared by the graph builder (which recolours the
# disk) and indicator_loader.py (which authors it, initialised to the alarm
# colour so a disk looks the same before the first ROS message as after an
# unmute). Both read them from here, because a builder and a loader disagreeing
# about the palette shows up as a disk that changes colour at startup for no
# reason.
DEFAULT_COLOR_MUTED = (0.0, 1.0, 0.0)
DEFAULT_COLOR_ALARM = (1.0, 0.3, 0.0)


def resolve_indicator_prim(robot: dict) -> str:
    """Where this robot's indicator disk lives.

    The fallback appends ForkliftB's internal hierarchy, which is right only
    for that model — spell `indicator_prim` out in robots.yaml for anything
    else. Shared so the loader authors the disk at exactly the path the graph
    builder later verifies.
    """
    cfg = robot.get("safety_indicator", {}) or {}
    return cfg.get(
        "indicator_prim", f"{robot['articulation_prim']}/body/body/safety_indicator"
    )


# Must stay in step with SafetyRosBridge._robot_muted_topic() in comm-layer, which
# builds the same name from the same robot name. The two systems agree by
# convention rather than by sharing a file: comm-layer also runs in HIL, where
# robots.yaml does not exist. Deriving it on both sides means the string is
# written once per system and never typed into a config file, so the halves
# cannot drift through a typo — only through someone changing one of these two
# functions.
DEFAULT_SAFETY_TOPIC_PREFIX = "/safety"


def resolve_muted_topic(robot: dict) -> str:
    """Which topic this robot's indicator listens on.

    Defaults to `/<name>/safety/is_muted`, the per-robot mirror comm-layer
    publishes when its ROS_ROBOT_IDS lists this robot. Scenes that predate the
    mirrors keep working by naming the global `/safety/is_muted` explicitly.
    """
    cfg = robot.get("safety_indicator", {}) or {}
    topic = cfg.get(
        "muted_topic", f"/{robot['name']}{DEFAULT_SAFETY_TOPIC_PREFIX}/is_muted"
    )
    if not isinstance(topic, str) or not topic.startswith("/"):
        raise ValueError(
            f"robots.yaml: '{robot.get('name', '?')}'.safety_indicator.muted_topic "
            f"must be an absolute topic name starting with '/', got {topic!r}"
        )
    return topic


def resolve_odom_frames(robot: dict) -> tuple[str, str]:
    """(odom_frame_id, base_frame_id) for this robot's TF edge.

    Defaults are namespaced with the robot name, matching what `odom_topic`
    already does. The node defaults are the bare `odom` -> `base_link`, so
    every robot published the same edge onto one /tf and a consumer saw the
    pose flick between trucks. Namespacing by default fixes that for every
    scene at once rather than only where someone remembered to override it;
    the cost is that a single-robot scene also gets `forklift_b/odom` instead
    of the conventional bare `odom`, which explicit keys can restore.
    """
    cfg = robot.get("odometry", {}) or {}
    name = robot["name"]
    out = []
    for key, default in (("odom_frame_id", f"{name}/odom"),
                         ("base_frame_id", f"{name}/base_link")):
        value = cfg.get(key, default)
        if not isinstance(value, str) or not value:
            raise ValueError(
                f"robots.yaml: '{name}'.odometry.{key} must be a non-empty string, "
                f"got {value!r}"
            )
        if value.startswith("/"):
            # tf2 rejects leading slashes outright, and it fails at publish time
            # inside the bridge where the message is easy to miss.
            raise ValueError(
                f"robots.yaml: '{name}'.odometry.{key} must not start with '/' "
                f"(tf2 rejects it), got {value!r}"
            )
        out.append(value)
    if out[0] == out[1]:
        raise ValueError(
            f"robots.yaml: '{name}'.odometry frame ids are identical ({out[0]}); "
            f"a TF edge needs two distinct frames"
        )
    return out[0], out[1]


def resolve_indicator_colors(robot: dict) -> tuple[tuple[float, float, float],
                                                   tuple[float, float, float]]:
    """(muted, alarm) RGB for this robot's disk, defaults applied.

    Lives under `safety_indicator`, not under its `mesh:` block: a scene whose
    disk is still baked into the USD has no `mesh:` block and would otherwise
    be unable to change the colours.
    """
    cfg = robot.get("safety_indicator", {}) or {}
    name = robot.get("name", "?")
    out = []
    for key, default in (("color_muted", DEFAULT_COLOR_MUTED),
                         ("color_alarm", DEFAULT_COLOR_ALARM)):
        value = cfg.get(key)
        if value is None:
            out.append(default)
            continue
        if not isinstance(value, (list, tuple)) or len(value) != 3:
            raise ValueError(
                f"robots.yaml: '{name}'.safety_indicator.{key} must be [r, g, b], "
                f"got {value!r}"
            )
        for component in value:
            # bool is an int subclass; `true` in YAML must not read as 1.0.
            if isinstance(component, bool) or not isinstance(component, (int, float)):
                raise ValueError(
                    f"robots.yaml: '{name}'.safety_indicator.{key} components must be "
                    f"numbers, got {value!r}"
                )
            if not 0.0 <= float(component) <= 1.0:
                raise ValueError(
                    f"robots.yaml: '{name}'.safety_indicator.{key} components are "
                    f"0..1, not 0..255, got {value!r}"
                )
        out.append(tuple(float(c) for c in value))
    return out[0], out[1]


def ensure_extensions_enabled() -> None:
    """Enable isaacsim.core.nodes + isaacsim.ros2.bridge if not yet on.
    Idempotent.
    """
    import omni.kit.app

    ext_mgr = omni.kit.app.get_app().get_extension_manager()
    for ext_id in ("isaacsim.core.nodes", "isaacsim.ros2.bridge"):
        if not ext_mgr.is_extension_enabled(ext_id):
            ext_mgr.set_extension_enabled_immediate(ext_id, True)
            print(f"[forklift] Enabled extension: {ext_id}")


def verify_prim_exists(stage, prim_path: str, what: str) -> None:
    prim = stage.GetPrimAtPath(prim_path)
    if not prim or not prim.IsValid():
        raise RuntimeError(
            f"[forklift] {what} prim not found on stage: {prim_path}\n"
            f"Load the matching scene, or edit robots.yaml to match actual prim paths."
        )


def clear_existing_graph(stage, graph_path: str) -> None:
    """Idempotency guard - delete any prior graph at graph_path."""
    prim = stage.GetPrimAtPath(graph_path)
    if prim and prim.IsValid():
        stage.RemovePrim(graph_path)
        print(f"[forklift] Cleared existing graph at {graph_path}")


def set_rel_target(stage, node_path: str, rel_name: str, target_path: str) -> None:
    """Set a relationship target on a built OG node (targetPrim / chassisPrim).
    og.Controller.SET_VALUES does not handle relationships, so do it via USD.
    """
    from pxr import Sdf

    prim = stage.GetPrimAtPath(node_path)
    rel = prim.GetRelationship(rel_name)
    if not rel:
        rel = prim.CreateRelationship(rel_name, custom=True)
    rel.SetTargets([Sdf.Path(target_path)])


def strip_baked_scene_graphs(extra_paths: tuple[str, ...] = ()) -> list[str]:
    """Remove the baked OmniGraph prims this refactor replaces, so the
    Python builders are authoritative and logic does not double-run.

    Headless/SSH-safe alternative to deleting the prim in the OmniGraph
    editor and re-saving the (36k-line) scene USD. Idempotent: prims
    already absent are skipped. Returns the list of paths actually removed.

    Call BEFORE the per-robot builders.
    """
    import omni.usd

    stage = omni.usd.get_context().get_stage()
    if stage is None:
        raise RuntimeError("No USD stage open. Load a scene first.")

    removed: list[str] = []
    for path in (*_BAKED_GRAPH_PATHS, *extra_paths):
        prim = stage.GetPrimAtPath(path)
        if prim and prim.IsValid():
            stage.RemovePrim(path)
            removed.append(path)
            print(f"[forklift] Stripped baked graph: {path}")
    if not removed:
        print("[forklift] No baked scene graphs found to strip (already clean)")
    return removed


def build_forklift_graphs(config_path: str = DEFAULT_ROBOTS_YAML) -> None:
    """Strip the baked scene graph, then build control + odometry + safety
    graphs for every robot in the config (each gated by its per-block
    `enabled` flag). Call this from run_actor_sdg.py.
    """
    # Deferred imports avoid a circular import at module load (the builder
    # modules import this one).
    from .forklift_control import build_control_graph
    from .forklift_odometry import build_odometry_graph
    from .forklift_safety_indicator import build_safety_graph

    ensure_extensions_enabled()
    # Remove the baked /World/ActionGraph (and legacy named graphs) first
    # so the rebuilt Python graphs are the only ones running.
    strip_baked_scene_graphs()
    build_control_graph(config_path)
    build_odometry_graph(config_path)
    build_safety_graph(config_path)


if __name__ == "__main__":
    cfg = os.environ.get("ISAAC_ROBOTS_YAML", DEFAULT_ROBOTS_YAML)
    build_forklift_graphs(cfg)
