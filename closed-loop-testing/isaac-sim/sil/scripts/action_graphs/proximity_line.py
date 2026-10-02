# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Halos SIL proximity pair line — the pair PSF just scored, drawn on the floor.

Driven by the top-level `proximity_line:` block of robots.yaml, off unless
`enabled: true`. Meaningful only under PSF_APP=pxc or both, where comm-layer
publishes /safety/proximity/pair.

For the latest motion decision on that topic it draws a line on the floor
between the two positions PSF sent (machine, person), in the decision's colour
— green NORMAL, amber REDUCE, red STOP — and, with `show_distance`, a label
such as "STOP 1.97 m" lying flat beside it, turned to face one camera
(`label_camera`, default the first in cameras.yaml).

The positions are drawn as they arrive: PSF reports them in the scene's world
frame (x, y in metres), so nothing is matched to the stage. They lag a moving
actor by perception's latency.

The line and label are geometry, so the RTSP streams and VSS carry them, and
perception sees them too. Keep it off for any run a detection or SRR figure is
taken from.

Nothing is drawn when no new decision arrived for `_STALE_S`, when the last
message was not a motion decision (heartbeat, fault, safe-release), or when a
slot was empty. A heartbeat republishes the held decision with the same
sequence, so it does not keep the line up.
"""

from __future__ import annotations

import json
import math
import os
import tempfile
import time

from .forklift_common import (
    DEFAULT_ROBOTS_YAML,
    clear_existing_graph,
    ensure_extensions_enabled,
    load_proximity_line_cfg,
)

GRAPH_PATH = "/World/ProximityLineGraph"
_ROOT = "/World/ProximityLine"
_LOOKS = _ROOT + "/Looks"

# PSF decides per processed frame while it sees a pair; the bridge polls at 10 Hz.
_STALE_S = 1.0

# A unit cube scaled between the two positions, just above the floor.
_LINE_WIDTH_M = 0.08
_LINE_HEIGHT_M = 0.02
_LINE_Z_M = 0.02

# The label: letter height on the floor, its gap from the line, and the glyph
# raster the atlas is drawn at.
_LABEL_HEIGHT_M = 0.4
_LABEL_GAP_M = 0.12
_LABEL_Z_M = 0.025
_GLYPH_PX = 96
_GLYPH_STROKE_PX = 5

_LABEL = {"normal": "NORMAL", "reduce_speed": "REDUCE", "stop": "STOP"}
_GLYPHS = "".join(sorted(set("".join(_LABEL.values()) + "0123456789.m")))

_FONTS = (
    os.path.join(os.environ.get("ISAAC_PATH", "/isaac-sim"),
                 "kit/resources/fonts/OpenSans-SemiBold.ttf"),
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
)

_LOG_EVERY_S = 5.0

_line = None
_update_sub = None


def build_proximity_line(config_path: str = DEFAULT_ROBOTS_YAML,
                         cameras_config_path: str | None = None) -> None:
    """Subscribe the pair topic, author the line, arm the per-frame update.

    No-op unless robots.yaml says `proximity_line: {enabled: true}`.
    `cameras_config_path` names the default camera the label faces.
    """
    global _line, _update_sub
    import omni.graph.core as og
    import omni.kit.app
    import omni.usd
    from pxr import Sdf

    cfg = load_proximity_line_cfg(config_path)
    stage = omni.usd.get_context().get_stage()
    if _line is not None:
        _update_sub = None
        _line.teardown()
        _line = None
    clear_existing_graph(stage, GRAPH_PATH)
    if not cfg["enabled"]:
        print("[pxc-line] proximity_line.enabled is false; not drawn", flush=True)
        return

    ensure_extensions_enabled()
    keys = og.Controller.Keys
    og.Controller.edit(
        {"graph_path": GRAPH_PATH, "evaluator_name": "execution"},
        {
            keys.CREATE_NODES: [
                ("OnPlaybackTick", "omni.graph.action.OnPlaybackTick"),
                ("ROS2Context", "isaacsim.ros2.bridge.ROS2Context"),
                ("SubscribePair", "isaacsim.ros2.bridge.ROS2Subscriber"),
            ],
            keys.SET_VALUES: [
                ("SubscribePair.inputs:messageName", "String"),
                ("SubscribePair.inputs:messagePackage", "std_msgs"),
                ("SubscribePair.inputs:topicName", cfg["topic"]),
            ],
            keys.CONNECT: [
                ("OnPlaybackTick.outputs:tick", "SubscribePair.inputs:execIn"),
                ("ROS2Context.outputs:context", "SubscribePair.inputs:context"),
            ],
        },
    )
    # Same pre-authoring as the indicator graphs: the dynamic output is only
    # bound reliably once its resolved type exists on the prim.
    sub_prim = stage.GetPrimAtPath(f"{GRAPH_PATH}/SubscribePair")
    if not sub_prim.GetAttribute("outputs:data"):
        sub_prim.CreateAttribute("outputs:data", Sdf.ValueTypeNames.String)

    camera = cfg["label_camera"] or _first_camera(cameras_config_path)
    _line = _PairLine(stage, cfg, camera)
    _line.build()
    _update_sub = (
        omni.kit.app.get_app()
        .get_update_event_stream()
        .create_subscription_to_pop(lambda _e: _line.update(), name="proximity_line")
    )
    print(f"[pxc-line] armed: topic={cfg['topic']}, "
          f"label={'on' if _line.has_label else 'off'}, label faces "
          f"{camera if _line.camera_xy else 'world +Y (no camera)'}", flush=True)


class _PairLine:
    def __init__(self, stage, cfg: dict, camera: str | None):
        from pxr import UsdGeom

        self._stage = stage
        self._cfg = cfg
        # PSF positions are metres; the stage may not be.
        self._mpu = float(UsdGeom.GetStageMetersPerUnit(stage) or 1.0)
        self._data_attr = None
        self._last_seq = None
        self._decision = None      # (mode, separation_m, machine xy, person xy, time)
        self._shown_mode = None
        self._shown_text = None
        self._last_log = 0.0
        self._atlas = None
        self._label_width = 0.0
        self.has_label = False
        # Where the camera the label faces stands, in stage units.
        self.camera_xy = _camera_xy(stage, camera)

    # ---------------------------------------------------------------- authoring

    def build(self) -> None:
        from pxr import Sdf, UsdGeom, UsdShade

        stage = self._stage
        if stage.GetPrimAtPath(_ROOT):
            stage.RemovePrim(_ROOT)
        root = UsdGeom.Xform.Define(stage, _ROOT)
        # Out of any navmesh re-bake, so a line across a walkway cannot carve it.
        root.GetPrim().CreateAttribute(
            "omni:navmesh:exclude", Sdf.ValueTypeNames.Bool, custom=True).Set(True)

        self._line_mat, self._line_color = self._emissive_material(f"{_LOOKS}/line")
        self._line_ops = self._xform(f"{_ROOT}/line", scale=True)
        cube = UsdGeom.Cube.Define(stage, f"{_ROOT}/line/geom")
        cube.CreateSizeAttr(1.0)
        UsdShade.MaterialBindingAPI.Apply(cube.GetPrim()).Bind(self._line_mat)

        if self._cfg["show_distance"]:
            try:
                self._build_label()
                self.has_label = True
            except Exception as exc:  # noqa: BLE001 - the line is still worth drawing
                print(f"[pxc-line] distance label disabled: {exc!r}", flush=True)
        self._set_visible(False)

    def _emissive_material(self, path: str):
        """Emission only, diffuse pinned to black: renders the authored colour
        under any lighting instead of washing out on the lit floor."""
        from pxr import Gf, Sdf, UsdShade

        mat = UsdShade.Material.Define(self._stage, path)
        shader = UsdShade.Shader.Define(self._stage, f"{path}/surface")
        shader.CreateIdAttr("UsdPreviewSurface")
        shader.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(1.0)
        shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.0))
        color = shader.CreateInput("emissiveColor", Sdf.ValueTypeNames.Color3f)
        mat.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), "surface")
        return mat, color

    def _build_label(self) -> None:
        """One glyph atlas per mode colour, and an empty mesh the text is laid into."""
        from pxr import Sdf, UsdGeom, UsdShade

        out_dir = tempfile.mkdtemp(prefix="halos_proximity_line_")
        self._label_mats = {}
        for mode, rgb in self._cfg["colors"].items():
            path = os.path.join(out_dir, f"glyphs_{mode}.png")
            self._atlas = _render_atlas(path, rgb)
            self._label_mats[mode] = self._texture_material(f"{_LOOKS}/label_{mode}", path)

        self._label_ops = self._xform(f"{_ROOT}/label")
        mesh = UsdGeom.Mesh.Define(self._stage, f"{_ROOT}/label/geom")
        mesh.CreateDoubleSidedAttr(True)
        mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
        self._label_mesh = mesh
        self._label_st = UsdGeom.PrimvarsAPI(mesh).CreatePrimvar(
            "st", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.faceVarying)
        self._label_binding = UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim())

    def _texture_material(self, path: str, texture: str):
        from pxr import Gf, Sdf, UsdShade

        stage = self._stage
        mat = UsdShade.Material.Define(stage, path)
        surface = UsdShade.Shader.Define(stage, f"{path}/surface")
        surface.CreateIdAttr("UsdPreviewSurface")
        surface.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(1.0)
        surface.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.0))
        # Cut out by the glyph alpha: the floor shows between the letters.
        surface.CreateInput("opacityThreshold", Sdf.ValueTypeNames.Float).Set(0.5)

        reader = UsdShade.Shader.Define(stage, f"{path}/st")
        reader.CreateIdAttr("UsdPrimvarReader_float2")
        reader.CreateInput("varname", Sdf.ValueTypeNames.Token).Set("st")

        tex = UsdShade.Shader.Define(stage, f"{path}/glyphs")
        tex.CreateIdAttr("UsdUVTexture")
        tex.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(texture)
        tex.CreateInput("sourceColorSpace", Sdf.ValueTypeNames.Token).Set("sRGB")
        tex.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(
            reader.ConnectableAPI(), "result")
        rgb = tex.CreateOutput("rgb", Sdf.ValueTypeNames.Float3)
        alpha = tex.CreateOutput("a", Sdf.ValueTypeNames.Float)
        surface.CreateInput("emissiveColor", Sdf.ValueTypeNames.Color3f).ConnectToSource(rgb)
        surface.CreateInput("opacity", Sdf.ValueTypeNames.Float).ConnectToSource(alpha)
        mat.CreateSurfaceOutput().ConnectToSource(surface.ConnectableAPI(), "surface")
        return mat

    def _xform(self, path: str, scale: bool = False) -> dict:
        from pxr import UsdGeom

        xf = UsdGeom.Xform.Define(self._stage, path)
        ops = {"t": xf.AddTranslateOp(UsdGeom.XformOp.PrecisionDouble),
               "r": xf.AddRotateZOp(UsdGeom.XformOp.PrecisionFloat)}
        if scale:
            ops["s"] = xf.AddScaleOp(UsdGeom.XformOp.PrecisionFloat)
        return ops

    # ---------------------------------------------------------------- per frame

    def update(self) -> None:
        try:
            self._update()
        except Exception as exc:  # noqa: BLE001 - a debug aid must not end the run
            now = time.monotonic()
            if now - self._last_log > _LOG_EVERY_S:
                self._last_log = now
                print(f"[pxc-line] update failed: {exc!r}", flush=True)

    def _update(self) -> None:
        now = time.monotonic()
        raw = self._read_topic()
        if raw:
            self._take(raw, now)
        if self._decision is None or now - self._decision[4] > _STALE_S:
            self._hide()
            return
        mode, sep, machine, person, _ = self._decision
        self._draw(machine, person, mode, sep)
        self._log(now, f"{_LABEL[mode]} {sep:.2f} m machine=({machine[0]:.2f}, "
                       f"{machine[1]:.2f}) person=({person[0]:.2f}, {person[1]:.2f})")

    def _read_topic(self):
        import omni.graph.core as og

        if self._data_attr is None:
            self._data_attr = og.Controller.attribute(f"{GRAPH_PATH}/SubscribePair.outputs:data")
        return og.Controller.get(self._data_attr)

    def _take(self, raw: str, now: float) -> None:
        """Keep a NEW motion decision with both slots filled; drop the rest."""
        try:
            msg = json.loads(raw)
        except ValueError:
            return
        if not isinstance(msg, dict):
            return
        seq = msg.get("sequence")
        if seq == self._last_seq:
            return
        self._last_seq = seq
        mode = msg.get("mode")
        by_role = {o.get("role"): o for o in msg.get("objects") or ()
                   if isinstance(o, dict) and not o.get("empty")}
        machine, person = by_role.get("machine"), by_role.get("person")
        if mode not in _LABEL or machine is None or person is None:
            self._decision = None
            return
        m = (float(machine["x"]), float(machine["y"]))
        p = (float(person["x"]), float(person["y"]))
        sep = msg.get("separation_m")
        if not isinstance(sep, (int, float)):
            sep = math.hypot(p[0] - m[0], p[1] - m[1])
        self._decision = (mode, float(sep), m, p, now)

    def _draw(self, a, b, mode: str, sep: float) -> None:
        from pxr import Gf

        k = 1.0 / self._mpu
        ax, ay, bx, by = a[0] * k, a[1] * k, b[0] * k, b[1] * k
        length = math.hypot(bx - ax, by - ay)
        heading = math.atan2(by - ay, bx - ax)
        mid = ((ax + bx) / 2.0, (ay + by) / 2.0)
        self._line_ops["t"].Set(Gf.Vec3d(mid[0], mid[1], _LINE_Z_M * k))
        self._line_ops["r"].Set(float(math.degrees(heading)))
        self._line_ops["s"].Set(Gf.Vec3f(max(length, 0.01 * k), _LINE_WIDTH_M * k,
                                         _LINE_HEIGHT_M * k))
        if self._shown_mode != mode:
            self._line_color.Set(Gf.Vec3f(*self._cfg["colors"][mode]))
        if self.has_label:
            self._draw_label(mid, (math.cos(heading), math.sin(heading)), mode,
                             f"{_LABEL[mode]} {sep:.2f} m", k)
        if self._shown_mode is None:
            self._set_visible(True)
        self._shown_mode = mode

    def _draw_label(self, mid, along, mode: str, text: str, k: float) -> None:
        from pxr import Gf

        if text != self._shown_text:
            self._label_width = self._lay_out(text, _LABEL_HEIGHT_M * k)
            self._shown_text = text
        if self._shown_mode != mode:
            self._label_binding.Bind(self._label_mats[mode])

        # Facing the camera: the letters' tops point along its line of sight to
        # the label, so the text reads straight across that camera's frame.
        if self.camera_xy:
            up = (mid[0] - self.camera_xy[0], mid[1] - self.camera_xy[1])
            norm = math.hypot(*up)
            up = (up[0] / norm, up[1] / norm) if norm > 1e-6 else (0.0, 1.0)
        else:
            up = (0.0, 1.0)
        right = (up[1], -up[0])

        # Beside the line, never across it: step off along the line's normal by
        # the label's own half-extent in that direction, on the side above the
        # line in the camera's frame, or right of it when the line runs at the
        # camera.
        normal = (-along[1], along[0])
        if (normal[0] * (up[0] + 0.5 * right[0])
                + normal[1] * (up[1] + 0.5 * right[1])) < 0:
            normal = (-normal[0], -normal[1])
        half = (abs(normal[0] * right[0] + normal[1] * right[1]) * self._label_width / 2
                + abs(normal[0] * up[0] + normal[1] * up[1]) * _LABEL_HEIGHT_M * k / 2)
        off = (_LINE_WIDTH_M / 2 + _LABEL_GAP_M) * k + half
        self._label_ops["t"].Set(Gf.Vec3d(mid[0] + normal[0] * off,
                                          mid[1] + normal[1] * off, _LABEL_Z_M * k))
        self._label_ops["r"].Set(float(math.degrees(math.atan2(right[1], right[0]))))

    def _lay_out(self, text: str, height: float) -> float:
        """One quad per glyph, centred on the label's origin, in the floor plane.

        Returns the label's width.
        """
        from pxr import Gf, Vt

        cells, cell_h = self._atlas
        space = cells["0"][2] * 0.5
        width = sum(cells[c][2] if c in cells else space for c in text) * height / cell_h
        x = -width / 2.0
        y0, y1 = -height / 2.0, height / 2.0
        points, st = [], []
        for ch in text:
            if ch not in cells:
                x += space * height / cell_h
                continue
            u0, u1, w_px = cells[ch]
            x1 = x + w_px * height / cell_h
            points += [Gf.Vec3f(x, y0, 0), Gf.Vec3f(x1, y0, 0),
                       Gf.Vec3f(x1, y1, 0), Gf.Vec3f(x, y1, 0)]
            st += [Gf.Vec2f(u0, 0), Gf.Vec2f(u1, 0), Gf.Vec2f(u1, 1), Gf.Vec2f(u0, 1)]
            x = x1
        n = len(points) // 4
        mesh = self._label_mesh
        mesh.GetPointsAttr().Set(Vt.Vec3fArray(points))
        mesh.GetFaceVertexCountsAttr().Set(Vt.IntArray([4] * n))
        mesh.GetFaceVertexIndicesAttr().Set(Vt.IntArray(list(range(4 * n))))
        mesh.GetExtentAttr().Set(Vt.Vec3fArray([Gf.Vec3f(-width / 2, y0, 0),
                                                Gf.Vec3f(width / 2, y1, 0)]))
        self._label_st.Set(Vt.Vec2fArray(st))
        return width

    def _hide(self) -> None:
        if self._shown_mode is not None:
            self._set_visible(False)
            self._shown_mode = None

    def _set_visible(self, visible: bool) -> None:
        from pxr import UsdGeom

        img = UsdGeom.Imageable(self._stage.GetPrimAtPath(_ROOT))
        img.MakeVisible() if visible else img.MakeInvisible()

    def _log(self, now: float, text: str) -> None:
        if now - self._last_log >= _LOG_EVERY_S:
            self._last_log = now
            print(f"[pxc-line] {text}", flush=True)

    def teardown(self) -> None:
        try:
            self._stage.RemovePrim(_ROOT)
        except Exception:  # noqa: BLE001
            pass


def _first_camera(cameras_config_path: str | None) -> str | None:
    """camera_prim of the first camera in cameras.yaml, the stream VST lists first."""
    if not cameras_config_path or not os.path.isfile(cameras_config_path):
        return None
    import yaml

    with open(cameras_config_path) as f:
        cameras = (yaml.safe_load(f) or {}).get("cameras") or []
    first = cameras[0] if cameras and isinstance(cameras[0], dict) else {}
    return first.get("camera_prim")


def _camera_xy(stage, camera: str | None):
    """World (x, y) of the camera prim, or None if there is no such camera."""
    from pxr import Usd, UsdGeom

    if not camera:
        return None
    prim = stage.GetPrimAtPath(camera)
    if not prim or not prim.IsValid():
        print(f"[pxc-line] label camera {camera} not on the stage", flush=True)
        return None
    pos = UsdGeom.Xformable(prim).ComputeLocalToWorldTransform(
        Usd.TimeCode.Default()).ExtractTranslation()
    return (float(pos[0]), float(pos[1]))


def _render_atlas(path: str, rgb_linear) -> tuple[dict, int]:
    """Write the label glyphs, one row, in this colour with a dark outline.

    Returns ({char: (u0, u1, width_px)}, cell height px). Rendered once at build
    time, so a label change only rewrites mesh points and UVs.
    """
    from PIL import Image, ImageDraw, ImageFont

    font = None
    for candidate in _FONTS:
        if os.path.isfile(candidate):
            font = ImageFont.truetype(candidate, _GLYPH_PX)
            break
    if font is None:
        font = ImageFont.load_default(size=_GLYPH_PX)

    pad = _GLYPH_STROKE_PX + 2
    ascent, descent = font.getmetrics()
    cell_h = ascent + descent + 2 * pad
    widths = {c: int(math.ceil(font.getlength(c))) + 2 * pad for c in _GLYPHS}
    total = sum(widths.values())
    img = Image.new("RGBA", (total, cell_h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    fill = tuple(int(round(_to_srgb(c) * 255)) for c in rgb_linear) + (255,)
    cells, x = {}, 0
    for c in _GLYPHS:
        draw.text((x + pad, pad), c, font=font, fill=fill,
                  stroke_width=_GLYPH_STROKE_PX, stroke_fill=(10, 10, 10, 255))
        cells[c] = (x / total, (x + widths[c]) / total, widths[c])
        x += widths[c]
    img.save(path)
    return cells, cell_h


def _to_srgb(c: float) -> float:
    """Linear 0-1 to the sRGB-encoded value a texture stores."""
    c = min(max(float(c), 0.0), 1.0)
    return 12.92 * c if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055
