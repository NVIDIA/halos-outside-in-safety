# Halos Outside-In Safety — SIL Launchable

A one-click NVIDIA Brev **Launchable** that deploys the full **Halos Outside-In Safety**
closed loop in **SIL** (Software-in-the-Loop) mode: a user clicks *Deploy*, the GPU VM boots
with both repos cloned, opens JupyterLab, and runs
[`deploy_hoisa_launchable.ipynb`](./deploy_hoisa_launchable.ipynb) to bring the whole system up
and watch sim-driven safety decisions in the VST UI.

This doc has two audiences:
- **Users** running the Launchable → Sections 1–6.
- **Launchable maintainers** building/listing the template on Brev → Section 7.

> Verified end-to-end on a Brev VM: 2× RTX 6000 Ada (48 GB), 26 vCPU, 141 GB RAM,
> driver 580.126.09, Docker 29.1.5.

---

## 1. What it deploys

SIL runs the closed safety loop on a single host across **two Docker Compose stacks**:

```
Isaac Sim (forklift + humans) ──RTSP/HEVC (self-hosted per cam)──▶ VSS Warehouse 3.2.1 (AI perception, 3 cams)
        ▲                                                          │
        │ ROS2 /safety/is_muted                                    │ Kafka mdx-events
        │                                                          ▼
  forklift safety disc ◀── comm-layer ◀── Safety Core / PSF (MUTE / UNMUTE decision)
```

- **Stack 1 — VSS Warehouse 3.2.1** (perception backend), deployed first with SIL-specific overrides.
- **Stack 2 — Halos SIL**: `safety-core` (PSF), `comm-layer` (UDP→ROS bridge), `isaac-sim`, `forklift-controller`.

The forklift in Isaac Sim drives camera streams → perception → PSF decides MUTE (forklift in
trailer, no humans → loading allowed) / UNMUTE (human present or forklift exiting → safety active)
→ the decision is published on `/safety/is_muted` and the forklift's safety disc reacts. Full loop.

---

## 2. Prerequisites / recommended system profile

Measured footprint of a running SIL system (steady state, scenario active):

| Resource | Observed | Recommended for the Launchable |
|---|---|---|
| GPU (perception) | GPU0 ~2 GB, ~9 % util | any 24 GB+ GPU |
| GPU (Isaac Sim)  | GPU1 ~16 GB, ~65 % util | **GPU with RT cores + >20 GB** |
| Docker images    | ~63 GB | — |
| Disk used        | ~119 GB | **≥ 250 GB** |
| RAM peak         | ~35 GB | **≥ 64 GB** |
| CPU              | 26 vCPU | **≥ 16 vCPU** |

**GPU requirement (critical):** Isaac Sim needs **RT cores** for ray-traced rendering — compute-only
GPUs (**H100 / A100**) will not work for Isaac. Suitable: **RTX 6000 Ada, RTX PRO 6000 Blackwell,
RTX A6000, L40S, L40**. Both perception + Isaac fit on a **single RTX 6000 Ada (48 GB)** (~18 GB
total), so 1 GPU is viable; 2 GPUs give clean isolation (perception GPU0, Isaac GPU1).

Other:
- NVIDIA driver **≥ 580.95.05**
- Docker **[28.3.3, 29.5.0)** + Compose **≥ 2.39** (Docker 29.5.0+ breaks NGC image pulls)
- NVIDIA Container Toolkit
- An NGC API key with access to the **`nvidia/halos-outside-in`** team

---

## 3. How to run (user)

1. Launch the Launchable on Brev and open **JupyterLab** (port 8888).
2. Open `deployments/scripts/deploy_hoisa_launchable.ipynb`.
3. In **Section 1**, set `NGC_CLI_API_KEY` (or leave empty if the instance already has
   `~/.ngc/config`). Adjust `HARDWARE_PROFILE` if not RTX 6000 Ada.
4. **Run all cells top-to-bottom.** The notebook validates prereqs, fetches artifacts, applies
   all SIL overrides, deploys + verifies both stacks, runs the Isaac scenario, and prints the VST URL.

**Timings (first run ~30–45 min):**

| Phase | First run | Subsequent |
|---|---|---|
| VSS image pull + deploy | ~10–15 min | ~2–3 min |
| VSS TensorRT engine build | ~1–2 min (fast on Ada) | cached |
| Halos build + up | ~3–5 min | ~1 min |
| Isaac scene load + **RT shader compile** | **~5–12 min** | ~3–5 min (cached) |

---

## 4. Ready signals (how you know it works)

The notebook gates on these automatically; you can also check manually:

1. **VSS perception** — `vss-rtvi-cv` shows ~30 FPS on all 3 cameras; Kafka `mdx-events` flowing.
2. **PSF wired** — `$MDX_DATA_DIR/comm-layer/opc_server.log` shows `MUTE`/`UNMUTE`/`HEARTBEAT`.
3. **Isaac→VSS handoff** — DeepStream swaps the sample streams for 3 Isaac streams; each per-camera
   RTSP endpoint (`:8554`/`:8555`/`:8556`) answers `ffprobe`; Isaac GPU util jumps to ~50–65 %.
4. **Closed loop** — `opc_server.log` shows **sim-driven MUTE↔UNMUTE** transitions, and the PSF log
   (`psf-log/pss.log`) shows `Forklift tripwire IN/OUT` events tied to the forklift cycle.
5. **Isaac subscribed** — Isaac kit log: `creating subscriber: /safety/is_muted` (the forklift reacts).

> "Working" = **sim-driven** transitions (the forklift cycle), not the VSS sample-video bootstrap
> traffic that appears before Isaac streams come up.

---

## 5. Access the VST UI

- **On Brev:** create a secure link for HAProxy port **7777** (any name) and open it with the `/vst/`
  path. The ingress allow-lists the Brev secure-link domains (`*.apps.run.brev.nvidia.com` and
  `*.brevlab.com`), so whichever host Brev assigns works — Section 7.5 of the notebook applies this.
- **Streaming limitation (live *and* recorded):** VST video playback uses **WebRTC (peer-to-peer UDP)**,
  which a Brev secure link (TCP/HTTPS only) cannot carry. The UI loads and stream lists/recordings are
  browsable, but **video frames will not render**. To see frames, **download a clip** (plain HTTP — works
  through the link) or pull the recorded `.mp4` from disk. The authoritative closed-loop proof is the
  MUTE/UNMUTE log check in Section 4 (Section 14 of the notebook).
- **Direct IP:** `http://<EXTERNAL_IP>:7777/vst/` (via HAProxy) or `:30888/vst/` (direct), if the
  firewall/security-group exposes the port. With direct (non-Brev) network access, WebRTC/UDP works and
  live video renders.

---

## 6. Troubleshooting (fixes the notebook already bakes in)

These were discovered during Brev validation and are handled automatically by the notebook; listed
here in case of a manual deploy:

| Symptom | Cause | Fix (automated) |
|---|---|---|
| Kafka/Redis/Elasticsearch crash-loop: *Permission denied* on log/data | app-data bind mounts + named volumes not writable by container UID | `chmod -R 777` app-data + `heal_infra_perms()` (chmod volume sources, drop stale `redis.log`, restart) |
| Elasticsearch won't start / Kafka unstable | `vm.max_map_count` / socket buffers too low | `sysctl -w vm.max_map_count=262144 net.core.rmem_max/wmem_max=5242880` — Section 2 |
| Perception never starts (`Active sources: 0`); `sdrc-wait-for-redis` stuck on *"waiting for redis…"*; `vss-configurator` loops on HTTP 400 adding sensors | Brev VMs ship **ufw active (default-deny inbound)**, which silently drops traffic from docker **bridge** containers to host-networked services (redis/kafka) reached via `HOST_IP` | `ufw allow from 172.16.0.0/12` + `192.168.0.0/16` when ufw is active (container-internal only) — Section 2 |
| A camera stuck at `0.00000` FPS; PSF drops events as STALE | Isaac RTSP carries no SEI; VSS expects it | DeepStream: disable SEI extraction + `attach-sys-ts-as-ntp=1` — Section 7 |
| `docker compose up` fails pulling NGC images | Docker **29.5.0+** breaks NGC pulls | Pin Docker to 28.3.3 (notebook warns; see VSS prereqs for the pin) |
| `402/403` on NGC resource / image | NGC key lacks `nvidia/halos-outside-in` access | Use a key authorized for that team |
| Brev secure link → **404 Not Found** on `/vst/` (yet `curl localhost:7777/vst/` = 200) | HAProxy ingress `known_host` ACL 404s any `Host` not in its allow-list; the Brev secure-link host wasn't listed | Allow-list the secure-link domains (`*.apps.run.brev.nvidia.com`, `*.brevlab.com`) by suffix in `haproxy.cfg.template` + recreate the ingress — Section 7.5 |
| NGC download → **403** from `awselb/2.0` (`curl` returns an HTML 403, not a zip) | The VM's egress IP/region/ASN is blocked by NGC (seen on **Verda / Helsinki, EU**); not curl/UA related | **Redeploy the Launchable in a US region** (validated reachable) — NGC CLI + `ngc registry` downloads then succeed |

---

## 7. Brev Launchable template spec (for the maintainer)

What to configure when creating/listing the Launchable on Brev. Mirror the setup used for the
**VSS** Brev Launchable for the exact console workflow, snapshotting, and secure-link setup.

### 7.1 Compute
- **GPU**: RTX 6000 Ada / RTX PRO 6000 Blackwell / RTX A6000 / L40S — **must have RT cores + ≥ 48 GB**
  (1 GPU works; 2 GPUs for isolation). **Do NOT offer H100/A100** (no RT cores → Isaac can't render).
- **Disk**: **≥ 250–300 GB on the ROOT disk** (where `/var/lib/docker` lives). Footprint ≈ 63 GB images
  + 26 GB data + volumes/engine cache ≈ 119 GB used; size up for headroom.
  - Requesting a large root disk in the Brev hardware step is enough — **no Docker data-root
    relocation needed** (unlike the VSS notebook, which moves storage to `/ephemeral` to cope with a
    small root disk). Only if Brev gives the big disk as a *separate* mount and keeps root small would
    you need to point Docker's data-root at it.
- **vCPU / RAM**: ≥ 16 vCPU, ≥ 64 GB.

### 7.2 Base image
Brev GPU base with: Ubuntu 22.04/24.04, NVIDIA driver ≥ 580.95.05, Docker **in [28.3.3, 29.5.0)**,
NVIDIA Container Toolkit, JupyterLab. The Docker default-runtime should be `nvidia`.

### 7.3 Repos to clone on launch
The deploy needs **two** repos in `$HOME` (the notebook auto-detects `~/<repo>` or `~/halos/<repo>`),
both **public** (no token needed):
- `https://github.com/NVIDIA/halos-outside-in-safety` (this repo — notebook + compose live here)
- `https://github.com/NVIDIA-AI-Blueprints/video-search-and-summarization` (perception)

Brev's "git repository" code option clones only **one** repo, so point it at this repo (Halos) and let
the setup script [`brev_setup.sh`](./brev_setup.sh) clone the second (VSS) in wizard Step 2. The
notebook does **not** clone anything itself — cloning is entirely the template's job.

> The deliverables (notebook, `brev_setup.sh`, this doc) and the SIL config edits must be
> **committed and pushed** to the repo branch the Launchable clones — the Launchable pulls from
> GitHub, not from a local working copy.

### 7.4 Access point
- **JupyterLab** on **8888** as the entry point, opened to
  `deployments/scripts/deploy_hoisa_launchable.ipynb`.
- **Secure link / exposed port: 7777** (HAProxy ingress → `/vst/`, `/kibana/`).
- ⚠️ **Reserve 8888 for Jupyter only.** Don't map any other service to 8888.

### 7.5 Pre-baking to cut first-run time (recommended, optional)
First run is dominated by ~250 GB of pulls/builds + Isaac shader compile. To make the Launchable feel
instant, bake into the image/snapshot (requires NGC access at build time):
- Pulled images: the VSS image set, **PSF** (`PSF_IMAGE`), **Isaac Sim** (`ISAAC_SIM_IMAGE`).
- Downloaded data: VSS `vss-warehouse-app-data` and Halos `sil-data` (with `collected-assets/`).
- A warmed `sil-data/isaac-cache` (compiled RT shaders) so Isaac starts in ~3–5 min instead of ~12.

The notebook is **idempotent** — it detects pre-existing data/images and skips the downloads.

### 7.6 Secrets
- The user supplies their **NGC API key** in the notebook (or pre-configured `~/.ngc/config`).
  Never bake a key into the public template.

### 7.7 Open questions for the Launchable maintainers
- The snapshot/pre-bake workflow for the VSS Launchable (to reuse for the larger Halos data/images).
- Secure-link configuration for port 7777 and whether VST live video can be made to render.
- Whether a single-GPU (1× RTX 6000 Ada) SKU should be the default to reduce cost.

### 7.8 Brev Console wizard — field-by-field

Creating the Launchable at `brev.nvidia.com/launchables/create`:

**Step 1 — Code files + runtime**
- *How would you like to provide your code files?* → **"I have code files in a git repository"**, URL =
  `https://github.com/NVIDIA/halos-outside-in-safety/blob/develop/deployments/scripts/deploy_hoisa_launchable.ipynb`
  (both repos are public; Brev clones Halos and opens this notebook). Point the URL at whatever
  **branch** actually has the deliverables committed (`develop` today; `main` once it lands there).
- *Runtime* → **VM Mode**. (The wizard itself recommends VM Mode for launchables that use private
  container registries / API keys — we pull NGC images with an NGC key.) **Not** Container/K8s.

**Step 2 — Setup script + machine image**
- *Do you want to run a setup script?* → **Yes**, paste [`brev_setup.sh`](./brev_setup.sh)
  (starts with `#!/bin/bash`, ~1 KiB; no token needed). It clones the **VSS** repo (the 2nd repo Brev
  didn't clone) into the user's home.
- *Do you want to use a machine image?* → skip for v1 (optional pre-baked image later — see §7.5).

**Step 3 — Jupyter & networking**
- *Jupyter experience* → **Yes** (Brev installs Jupyter + a one-click "Open Notebook" button on 8888).
- *Secure Links / ports* → expose **7777** (VST UI). Reserve **8888** for Jupyter only (don't map anything else to 8888).

**Steps 4–5** — name the Launchable, set the GPU SKU (RT-core GPU ≥ 48 GB, disk ≥ 250 GB), then
**Generate** and share the link.

> The notebook embeds the NGC-image auth steps (Section 4 docker login), matching the wizard's note
> *"document image authentication steps in your uploaded notebook file"* for VM Mode.
