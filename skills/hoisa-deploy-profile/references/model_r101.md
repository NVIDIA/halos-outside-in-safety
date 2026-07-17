# R101 Sparse4D Model (3D profile)

The 3D profile runs **Sparse4D** multi-view 3D detection + tracking. The default backbone
is **ResNet-50 (R50)**; the 3D SIL profile uses the **ResNet-101 (R101)** backbone instead,
which gives **better small / occluded / distant object detection** (the harder cases in a
loading-dock scene) at the cost of somewhat lower FPS. This file is the recipe to fetch and
place the R101 model so the 3D perception `config.yaml` (see `vss_3d_overrides.md`) loads it.

> This is a **perception (VSS)** artifact, not a Halos artifact. It ships in the **public NGC
> TAO catalog** (`nvidia/tao/sparse4d_rn101`) and is pinned in the profile env as
> `R101_DEPLOYABLE_RESOURCE` / `R101_TRAINABLE_RESOURCE` (`deployments/profiles/sil.env` |
> `base.env`, 3D-profile-only). Both packages must be the **same version**.

---

## Two packages — you need BOTH

The R101 model ships as **two** NGC packages that must be paired:

| Package | Contains | Why you need it |
|---------|----------|-----------------|
| **Deployable** | the inference **ONNX** (the R101 Sparse4D network) | what the TensorRT engine builds from at deploy time |
| **Trainable** | training checkpoint **+ the kmeans anchor `.npy`** | the anchor file is a **deploy-time input**, and it ships only in the trainable package |

The kmeans anchor `.npy` encodes the initial 3D anchor distribution Sparse4D projects onto
each frame. Deployment fails (or detections are garbage) without it, and it is **not** in
the deployable package — so you must download the trainable package too, purely to extract
the anchor.

> **⚠️ Same version, same source.** The deployable ONNX and the trainable anchor must be the
> **same model version** — the anchor distribution is trained together with the network
> weights. Mixing an ONNX from one version with an anchor from another (or from the R50
> model) produces silently wrong detections. Download both from the **same** package version.

---

## 1. Download both packages

```bash
# Resource names are pinned in the profile env (deployments/profiles/sil.env | base.env):
#   R101_DEPLOYABLE_RESOURCE=nvidia/tao/sparse4d_rn101:deployable_v2.2   # inference ONNX
#   R101_TRAINABLE_RESOURCE=nvidia/tao/sparse4d_rn101:trainable_v2.2     # checkpoint + kmeans anchor .npy

# Deployable (ONNX)
ngc registry model download-version "$R101_DEPLOYABLE_RESOURCE"

# Trainable (checkpoint + kmeans anchor .npy)
ngc registry model download-version "$R101_TRAINABLE_RESOURCE"
```

Both packages must be the **same version** (pinned to `v2.2` in the env). To move versions,
bump `R101_DEPLOYABLE_RESOURCE` and `R101_TRAINABLE_RESOURCE` in `deployments/profiles/*.env`
**together** — a mismatched ONNX/anchor pair produces silently wrong detections.

---

## 2. Install the model at the bind-mount source

The model reaches the perception container through a **bind mount**, not through `config.yaml`
directly. `warehouse-3d-app.yml:259-260` mounts two specific host files onto the container paths
that `config.yaml`'s `onnx_file` / `anchor` keys read:

| `config.yaml` key | Container path (read by DeepStream) | Host file (bind-mount source) |
|---|---|---|
| `onnx_file` | `…/sparse4d/sparse4d_warehouse_v2.2.onnx` | `$VSS_DATA_DIR/models/sparse4d/ov/sparse4d_warehouse_v2.2.onnx` |
| `anchor` | `…/sparse4d/_ov_kmeans900_v2.2.npy` | `$VSS_DATA_DIR/models/sparse4d/ov/_ov_kmeans900_v2.2.npy` |

Install the downloaded ONNX and anchor at the **host** paths (right column), under the mount's
exact filenames. The mount references those filenames literally, so a file under any other name
is not mounted and the model is silently ignored.

```bash
MODEL_DIR="$VSS_DATA_DIR/models/sparse4d/ov"   # bind-mount source (warehouse-3d-app.yml:259-260)
mkdir -p "$MODEL_DIR"

# Back up any model already installed, then place the R101 files under the canonical names.
ts=$(date +%Y%m%d-%H%M%S)
for f in sparse4d_warehouse_v2.2.onnx _ov_kmeans900_v2.2.npy; do
  [ -e "$MODEL_DIR/$f" ] && cp -a "$MODEL_DIR/$f" "$MODEL_DIR/$f.bak-$ts"
done

cp <deployable_download>/*.onnx   "$MODEL_DIR/sparse4d_warehouse_v2.2.onnx"   # deployable package (ONNX)
cp <trainable_download>/**/*.npy  "$MODEL_DIR/_ov_kmeans900_v2.2.npy"         # trainable package (kmeans anchor)
```

> **Using a different filename.** The filenames are fixed by the bind mount, so renaming means
> updating all three references together: both mount lines in `warehouse-3d-app.yml` (host **and**
> container side) and the matching `onnx_file` / `anchor` in `config.yaml`. Installing in place
> under the canonical names above avoids that — nothing else needs to change.

---

## 3. Verify

```bash
ls -lh "$MODEL_DIR"    # expect: the R101 ONNX  +  the kmeans anchor .npy
```

- On first deploy the 3D perception service builds a TensorRT engine from the ONNX
  (~10-15 min) — this is the same one-time build as 2D, just heavier for R101.
- If the perception log shows an anchor / `.npy` **not found** error, the anchor wasn't
  placed or wasn't renamed to the config's expected filename — re-check step 2.
- If detections are present but consistently wrong (misplaced / wrong-size boxes), suspect a
  **version mismatch** between the ONNX and the anchor — re-download both at the same
  version.

---

## Notes

- **R101 vs R50 trade-off:** R101 improves recall on small / occluded / far objects but runs
  at a lower FPS than R50. For a safety SIL run, detection quality on the hard cases matters
  more than raw FPS — the 6 s `timeWindowSize` in `nvpss.conf` gives enough latency headroom
  that the lower frame rate rarely trips STALE drops.
- **2D profile:** R101 is a **3D-only** artifact. The 2D profile uses its own detector
  (`vss_2d_overrides.md`) and does not need this.
