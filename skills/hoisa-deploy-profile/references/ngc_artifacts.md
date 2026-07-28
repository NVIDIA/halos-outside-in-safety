# NGC Artifacts (Halos)

The Halos compose files, profiles, and configs ship **in the OSS repo you cloned** —
there is no Halos compose package to download. From NGC you pull only:

| Artifact | NGC path | Needed by | Contains |
|----------|----------|-----------|----------|
| Halos SIL data | pinned as `MDX_DATA_RESOURCE` in `sil.env` / `hil.env` | `sil`, `hil` | Isaac Sim `collected-assets/` (warehouse / vehicle / character meshes), scenes, playback |
| PSF image | pinned as `PSF_IMAGE` in `deployments/profiles/<profile>.env` | `base`, `sil` | the Proactive Safety Framework container |
| Isaac Sim base | pinned as `ISAAC_SIM_IMAGE` in `sil.env` / `hil.env` (public) | `sil`, `hil` | base image the `isaac-sim` service builds from |

VSS Warehouse images are pulled when you deploy VSS, not here — via the `vss-deploy-profile` skill, or per the public VSS Warehouse docs (github.com/NVIDIA-AI-Blueprints/video-search-and-summarization).

> **Access**: the Halos packages are in the `nvidia/halos-outside-in` NGC team. If
> `ngc registry resource info` or `docker pull` returns `402` / `403`, your NGC key is not
> authorized for that org — confirm `ngc config set` and `docker login nvcr.io` with a key
> that has access.

---

## 1. Halos SIL data (`sil` / `hil`)

Your profile's `MDX_DATA_DIR` points at this directory; it must contain
`collected-assets/`, which the Isaac scenes reference. The tarball expands into a
`sil-data/` folder — extract it to the parent of your chosen `MDX_DATA_DIR`.

```bash
set -a; source deployments/profiles/sil.env; set +a   # or hil.env
cd /tmp
ngc registry resource download-version "$MDX_DATA_RESOURCE"

# Example: MDX_DATA_DIR=$HOME/sil-data  →  extract into $HOME
tar -xzf sample-sil-data_v*/halos-outside-in-sil-data.tar.gz \
  --directory="$HOME"
```

Verify:
```bash
ls "$MDX_DATA_DIR/collected-assets"   # → Characters  Vehicles  Warehouse
```

> `base` profile has no Isaac Sim and does not need sil-data.

---

## 2. PSF image (`base` / `sil`)

`PSF_IMAGE` is pinned in the repo's profile env (`deployments/profiles/<profile>.env`).
Source your filled run-env and pre-pull (pre-pulling in parallel with the VSS build
saves wall time):

```bash
set -a; source deployments/profiles/<profile>.env; set +a
docker pull "$PSF_IMAGE"
echo "PSF image: $PSF_IMAGE"
```

> When the Halos team rebuilds PSF, only `PSF_IMAGE` in the repo's profile env
> changes — re-pull; no skill edit needed.
> (`hil` runs PSF on Thor, not as a container here — see `halos_hil.md`.)

---

## 3. Isaac Sim base image (`sil` / `hil`)

The `isaac-sim` service builds from the public Isaac Sim base. Pre-pull:

```bash
set -a; source deployments/profiles/sil.env; set +a   # or hil.env
docker pull "$ISAAC_SIM_IMAGE"
```

---

## 4. Thor Safety Core package — `psf-tegra` (Thor `base` / `hil` only)

On IGX Thor the Safety Core runs partly as **host binaries** (not in the PSF container),
so it needs the aarch64 Safety Core `.deb` from NGC. x86 profiles do **not** need it
(the SDM runs in the container there). The NGC resource path is pinned in
`deployments/profiles/base-thor.env` — the single source of truth, the same way `PSF_IMAGE`
is. Source that env, then reference the variable:

| Env var | Needed by | Contains |
|---|---|---|
| `PSF_TEGRA_RESOURCE` | Thor `base` / `hil` | host binaries → `/opt/nvidia/psf/`: `atl_sdm`, `launch_hoisa.sh`, `safety_monitor`, sensor config |

```bash
set -a; source deployments/profiles/base-thor.env; set +a
```

### Install `psf-tegra` (host binaries — required for any Thor Safety Core)

```bash
ngc registry resource download-version "$PSF_TEGRA_RESOURCE"
sudo dpkg -i */psf-tegra.deb
ls /opt/nvidia/psf/bin/        # → atl_sdm, launch_hoisa.sh, safety_monitor, …
```

---

## Verify

Run only the checks that apply to your profile; all applicable checks must pass
before `docker compose up`:

```bash
ls "$MDX_DATA_DIR/collected-assets" >/dev/null 2>&1 && echo "sil-data OK"                     # sil / hil
docker image inspect "$PSF_IMAGE" >/dev/null 2>&1 && echo "PSF image OK"                       # base / sil
docker image inspect "$ISAAC_SIM_IMAGE" >/dev/null 2>&1 && echo "Isaac base OK"                # sil / hil
```
