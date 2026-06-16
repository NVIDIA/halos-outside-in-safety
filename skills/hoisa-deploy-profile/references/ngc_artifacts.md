# NGC Artifacts (Halos)

The Halos compose files, profiles, and configs ship **in the OSS repo you cloned** —
there is no Halos compose package to download. From NGC you pull only:

| Artifact | NGC path | Needed by | Contains |
|----------|----------|-----------|----------|
| Halos SIL data | pinned as `MDX_DATA_RESOURCE` in `sil.env` / `hil.env` | `sil`, `hil` | Isaac Sim `collected-assets/` (warehouse / vehicle / character meshes), scenes, playback |
| PSF image | pinned as `PSF_IMAGE` in `deployments/profiles/<profile>.env` | `base`, `sil` | the Proactive Safety Framework container |
| Isaac Sim base | pinned as `ISAAC_SIM_IMAGE` in `sil.env` / `hil.env` (public) | `sil`, `hil` | base image the `isaac-sim` service builds from |

VSS Warehouse images are pulled when you deploy VSS, not here — via the `vss-deploy-profile` skill, or per the public VSS Warehouse docs (github.com/NVIDIA-AI-Blueprints/video-search-and-summarization).

> **Access**: the Halos packages are in the `nvidia/halos-outside-in` NGC team (the gated FSI package is under `nvidia/outside-in-safety`). If
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

## 4. Thor Safety Core packages — `psf-tegra` + `psf-tegra-fsi` (Thor `base` / `hil` only)

On IGX Thor the Safety Core runs partly as **host binaries** (not in the PSF container),
so it needs the aarch64 Safety Core `.deb`s from NGC. x86 profiles do **not** need these
(the SDM runs in the container there). Both NGC resource paths are pinned in
`deployments/profiles/base-thor.env` — the single source of truth, the same way `PSF_IMAGE`
is. Source that env, then reference the variables:

| Env var | Needed by | Contains |
|---|---|---|
| `PSF_TEGRA_RESOURCE` | Thor `base` (CCPLEX **and** FSI host side) | host binaries → `/opt/nvidia/psf/`: `atl_sdm`, `launch_hoisa.sh`, `safety_monitor`, sensor config |
| `PSF_TEGRA_FSI_RESOURCE` | **FSI only** (`SDM_TARGET=fsi`) | the HOISA FSI firmware blobs (`fsi-ffw-t264.bin` for `atl`, + proximity) flashed to the FSI QSPI, **plus** the `fsicom-agent` bridge binary installed on the Thor. See `halos_thor.md` §5B |

```bash
set -a; source deployments/profiles/base-thor.env; set +a
```

### Install `psf-tegra` (host binaries — required for any Thor Safety Core)

```bash
ngc registry resource download-version "$PSF_TEGRA_RESOURCE"
sudo dpkg -i */psf-tegra.deb
ls /opt/nvidia/psf/bin/        # → atl_sdm, launch_hoisa.sh, safety_monitor, …
```

### Fetch `psf-tegra-fsi` (FSI only, `SDM_TARGET=fsi`)

`psf-tegra-fsi` provides the `fsicom-agent` binary (install on the Thor) and the FSI firmware
blob (stage + QSPI-flash, see `halos_thor.md` §5B):

```bash
ngc registry resource download-version "$PSF_TEGRA_FSI_RESOURCE"
# (a) on the Thor: install for fsicom-agent
sudo dpkg -i */psf-tegra-fsi.deb && ls /opt/nvidia/psf/bin/fsicom-agent
# (b) for the firmware blob (extract; flashing-host staging in halos_thor.md §5B)
dpkg -x */psf-tegra-fsi.deb /tmp/psf-fsi/
ls /tmp/psf-fsi/opt/nvidia/psf/etc/fsi-fw/atl/fsi-ffw-t264.bin   # the atl HOISA FSI firmware blob
```

Then follow `halos_thor.md` §5B to stage `fsi-ffw-t264.bin` into the BSP bootloader dir
(backing up the SEP default) and flash QSPI slot A.

---

## Verify

Run only the checks that apply to your profile; all applicable checks must pass
before `docker compose up`:

```bash
ls "$MDX_DATA_DIR/collected-assets" >/dev/null 2>&1 && echo "sil-data OK"                     # sil / hil
docker image inspect "$PSF_IMAGE" >/dev/null 2>&1 && echo "PSF image OK"                       # base / sil
docker image inspect "$ISAAC_SIM_IMAGE" >/dev/null 2>&1 && echo "Isaac base OK"                # sil / hil
```
