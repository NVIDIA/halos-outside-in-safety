#!/usr/bin/env python3
"""Turn a Collect As bundle into a single .usd that references external assets.

Collect As localises every dependency next to the root layer, which is how the
40x20 two-dock scene arrived as 3318 files / 8.4 GB — far too large to commit and
tied to a Nucleus login. This flattens the layer stack into one file and rewrites
each asset path back to public S3 or to sil-data, so the result is a committable
scene that needs no Nucleus credentials. It produced
sil/scenes/warehouse_40x20_two_loading_dock.usd (188 KB), after which the bundle
was deleted — so re-running this needs a bundle re-collected from Nucleus first.

MUST run inside the Isaac Sim runtime for the omni.client resolver. Without it USD
treats an https:// URL as a filesystem path and normalises '//' to '/', which
silently corrupts every rewritten reference:

    docker exec isaac-sim bash -lc 'cd /isaac-sim && \
        ./python.sh sil/scripts/tools/flatten_collected_scene.py --report'

Defaults to --report, which only prints the classification table and writes
nothing. Pass --out <path> to export. The source scene is never overwritten.

How it works
------------
1. Open the source scene, Load() every payload, and collect the layers that
   actually compose the active prims. The 40x20 bundle held 3318 files but only
   345 (~2.1 GB) were in that set; the rest back inactive prims and never
   compose, so their references need no rewriting.
2. UsdUtils.FlattenLayerStack merges the root layer with its sublayers while
   KEEPING payloads and references as references. Stage.Flatten would instead
   inline all geometry and produce an enormous file.
3. During the merge every asset path is rewritten using a map derived from the
   working 20x20 scene: public S3 first, then sil-data.
"""
import argparse
import os
import sys
from collections import Counter, defaultdict

from isaacsim import SimulationApp

_HEADLESS = SimulationApp({"headless": True})

from pxr import Sdf, Tf, Usd, UsdUtils  # noqa: E402

DEFAULT_SCENES = "/isaac-sim/sil/scenes"
DEFAULT_SOURCE = (f"{DEFAULT_SCENES}/warehouse_40x20_2fl/"
                  "warehouse_40x20_Halos_two_loading_dock_split_doors.usd")
DEFAULT_REFERENCE = (f"{DEFAULT_SCENES}/"
                     "indicator_warehouse_20x20_layout_overflow_test_2fl.usd")

S3_HOST = "omniverse-content-production.s3-us-west-2.amazonaws.com"


def layer_asset_paths(path):
    """(kind, prim, assetPath) for one layer, subLayers included. None if unparseable."""
    try:
        layer = Sdf.Layer.FindOrOpen(path)
    except Exception:  # noqa: BLE001 - malformed layers raise several unrelated types
        return None
    if layer is None:
        return None

    out = [("subLayer", "/", p) for p in layer.subLayerPaths]

    def walk(spec):
        p = spec.path.pathString
        for op in ("explicitItems", "addedItems", "prependedItems",
                   "appendedItems", "orderedItems"):
            for r in getattr(spec.payloadList, op, []):
                out.append(("payload", p, r.assetPath))
            for r in getattr(spec.referenceList, op, []):
                out.append(("reference", p, r.assetPath))
        for prop in spec.properties:
            if prop.HasDefaultValue():
                v = prop.default
                if isinstance(v, Sdf.AssetPath):
                    out.append((f"attr:{prop.name}", p, v.path))
                elif isinstance(v, (list, tuple)) and v and isinstance(v[0], Sdf.AssetPath):
                    out.extend((f"attr:{prop.name}", p, a.path) for a in v)
        for c in spec.nameChildren:
            walk(c)

    for root in layer.rootPrims:
        walk(root)
    return out


def composed_layers(path):
    """Absolute paths of the layers that compose the ACTIVE prims."""
    stage = Usd.Stage.Open(path)
    stage.Load()
    found = set()
    for prim in stage.Traverse():
        for spec in prim.GetPrimStack():
            layer = spec.layer
            if layer and layer.realPath:
                found.add(layer.realPath)
    return found


def build_reference_map(reference_scene):
    """basename -> asset paths the reference scene uses (S3 or sil-data)."""
    entries = layer_asset_paths(reference_scene)
    if entries is None:
        sys.exit(f"cannot read reference scene: {reference_scene}")
    by_name = defaultdict(set)
    for _, _, ap in entries:
        if ap:
            by_name[os.path.basename(ap)].add(ap)
    return by_name


def target_loads(abs_path):
    """Whether this layer opens under the running USD version."""
    if not abs_path.lower().endswith((".usd", ".usda", ".usdc")):
        return os.path.isfile(abs_path), "not USD"
    if not os.path.isfile(abs_path):
        return False, "missing"
    try:
        layer = Sdf.Layer.FindOrOpen(abs_path)
    except Tf.ErrorException:
        return False, "unparseable by this USD version"
    return (layer is not None), "" if layer else "returned None"


class Rewriter:
    """Picks the new path for every asset path seen during the flatten."""

    def __init__(self, ref_map, out_dir, needed, bundle_dir, scenes_dir):
        self.ref_map = ref_map
        self.out_dir = out_dir
        self.needed = needed
        self.bundle_dir = bundle_dir
        self.scenes_dir = scenes_dir
        self.decisions = []      # (dest, reason, needed?, old, new)
        self.keep_local = set()
        self._cache = {}

    def decide(self, source_layer, asset_path):
        if not asset_path:
            return asset_path
        key = (source_layer.identifier if source_layer else "", asset_path)
        if key in self._cache:
            return self._cache[key]
        new, dest, reason, used = self._decide(source_layer, asset_path)
        self.decisions.append((dest, reason, used, asset_path, new))
        self._cache[key] = new
        return new

    def _abs_of(self, source_layer, asset_path):
        base = (os.path.dirname(source_layer.realPath or source_layer.identifier)
                if source_layer else self.bundle_dir)
        return os.path.normpath(os.path.join(base, asset_path))

    def _decide(self, source_layer, asset_path):
        abs_src = self._abs_of(source_layer, asset_path)
        used = abs_src in self.needed

        if asset_path.startswith(("http://", "https://", "omniverse://")):
            return asset_path, "unchanged", "already a URL", used

        # Collect As buries the S3 host name in the local path; undo that.
        if S3_HOST in asset_path:
            idx = asset_path.index(S3_HOST) + len(S3_HOST)
            return (f"https://{S3_HOST}{asset_path[idx:]}", "S3",
                    "recovered from host name", used)

        name = os.path.basename(asset_path)
        if name.lower().endswith(".mdl") and "/" not in asset_path:
            return asset_path, "unchanged", "built-in MDL", used

        candidates = self.ref_map.get(name, set())
        s3 = sorted(c for c in candidates if c.startswith("http"))
        if s3:
            return s3[0], "S3", "matched by file name", used

        sil = sorted(c for c in candidates if c.startswith("../"))
        if sil:
            abs_target = os.path.normpath(os.path.join(self.scenes_dir, sil[0]))
            loads, why = target_loads(abs_target)
            if loads:
                return (os.path.relpath(abs_target, self.out_dir), "sil-data",
                        "matched by file name", used)
            return self._keep_local(abs_src, asset_path, f"sil-data {why}", used)

        return self._keep_local(abs_src, asset_path, "no external source", used)

    def _keep_local(self, abs_src, asset_path, reason, used):
        if os.path.isfile(abs_src):
            if used:
                self.keep_local.add(abs_src)
            return os.path.relpath(abs_src, self.out_dir), "LOCAL", reason, used
        return asset_path, "UNRESOLVED", reason, used


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--source", default=DEFAULT_SOURCE,
                    help="root .usd of the Collect As bundle")
    ap.add_argument("--reference-scene", default=DEFAULT_REFERENCE,
                    help="working scene whose asset paths seed the rewrite map")
    ap.add_argument("--scenes-dir", default=DEFAULT_SCENES,
                    help="base the reference scene's relative paths resolve against")
    ap.add_argument("--out", help="output .usd (omit for a report only)")
    ap.add_argument("--report", action="store_true", help="report only, write nothing")
    args = ap.parse_args()

    bundle_dir = os.path.dirname(os.path.abspath(args.source))
    out_dir = os.path.dirname(os.path.abspath(args.out)) if args.out else bundle_dir
    print(f"source scene   : {args.source}")
    print(f"reference scene: {args.reference_scene}")
    print(f"output dir     : {out_dir}")

    needed = composed_layers(args.source)
    in_bundle = {p for p in needed if p.startswith(bundle_dir)}
    print(f"\nlayers composing active prims: {len(needed)} "
          f"({len(in_bundle)} inside the bundle)")

    ref_map = build_reference_map(args.reference_scene)
    print(f"names in the rewrite map     : {len(ref_map)}\n")

    stage = Usd.Stage.Open(args.source)
    rw = Rewriter(ref_map, out_dir, needed, bundle_dir, args.scenes_dir)
    flat = UsdUtils.FlattenLayerStack(stage, rw.decide, "flattened")

    print("=== references that actually compose ===")
    live = [d for d in rw.decisions if d[2]]
    dead = [d for d in rw.decisions if not d[2]]
    for dest, n in Counter(d[0] for d in live).most_common():
        print(f"  {n:5d}  {dest}")
    print(f"\n=== references from non-composing prims (ignorable): {len(dead)} ===")
    for dest, n in Counter(d[0] for d in dead).most_common():
        print(f"  {n:5d}  {dest}")

    print("\n=== every live reference (old -> new) ===")
    for dest, reason, _, old, new in live:
        print(f"  [{dest}] {reason}")
        print(f"      old: {old}")
        print(f"      new: {new}")

    unresolved = [d for d in live if d[0] == "UNRESOLVED"]
    if unresolved:
        print(f"\n!!! {len(unresolved)} live references did NOT resolve:")
        for d in unresolved[:10]:
            print(f"     {d[3]}   ({d[1]})")

    if rw.keep_local:
        total = sum(os.path.getsize(p) for p in rw.keep_local)
        print(f"\n=== local files that must be kept: {len(rw.keep_local)}, "
              f"{total/1048576:.1f} MB ===")
        for p in sorted(rw.keep_local)[:12]:
            print(f"     {os.path.relpath(p, bundle_dir)}")

    print(f"\nflattened layer: {len(flat.ExportToString())/1024:.1f} KB as text")

    if args.out and not args.report:
        flat.Export(args.out)
        print(f"WROTE: {args.out} ({os.path.getsize(args.out)/1024:.1f} KB)")
        print("\n=== source stage vs new stage ===")
        for label, path in (("source", args.source), ("new", args.out)):
            st = Usd.Stage.Open(path)
            st.Load()
            prims = meshes = points = 0
            for pr in st.Traverse():
                prims += 1
                if pr.GetTypeName() == "Mesh":
                    meshes += 1
                    pts = pr.GetAttribute("points").Get()
                    if pts:
                        points += len(pts)
            print(f"  {label}: prims={prims} meshes={meshes} points={points}")
    else:
        print("\n(report mode - nothing written)")


if __name__ == "__main__":
    main()
    _HEADLESS.close()
