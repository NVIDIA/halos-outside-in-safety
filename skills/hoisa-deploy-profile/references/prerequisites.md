# Prerequisites

Run each check in order. Report pass/fail. Fix before moving to next.

> **Host setup overlaps VSS Warehouse** (deployed first as the perception backend).
> The authoritative, detailed host install — pinned Docker, per-platform GPU driver
> versions, the NVIDIA Container Toolkit, and kernel/sysctl tuning — lives in the VSS
> prerequisites (the `vss-deploy-profile` skill); run those first. The checks below are
> HOISA's own lightweight gates plus the HOISA-specific additions.

---

## 1. GPU

```bash
nvidia-smi --query-gpu=index,name,driver_version,memory.total --format=csv,noheader
```

**Pass**: Driver >= 580.95.05, meets both perception and Isaac Sim GPU requirements.

### Perception GPU (VSS Warehouse)

Any GPU with 24 GB+ VRAM. `HARDWARE_PROFILE` must match `blueprint_config.yml`:

| GPU | HARDWARE_PROFILE |
|---|---|
| RTX PRO 6000 Blackwell | `RTXPRO6000BW` |
| RTX A6000 Ada Generation | `RTXA6000ADA` |
| RTX A6000 (non-Ada) | `RTXA6000` |
| H100 (NVL, SXM HBM3) | `H100` |
| L40S | `L40S` |
| L40 | `L40` |
| L4 | `L4` |

### Isaac Sim GPU

Requires a GPU with **RT cores** and **>20 GB VRAM** for ray-traced rendering.
Not all GPUs qualify — compute-only GPUs (e.g. H100) lack RT cores.

Suitable: RTX PRO 6000, RTX A6000 Ada, RTX A6000, L40S, L40.

### Recommended Setup

- **2+ GPUs**: GPU 0 for perception, GPU 1 (with RT cores) for Isaac Sim.
- **1 GPU**: Must have both RT cores and >24 GB VRAM (e.g. RTX A6000 Ada 48 GB).

**Fail** (`nvidia-smi` missing or not loaded): the agent does **not** auto-install GPU drivers.
- Installed but not loaded → load the module, no reboot: `sudo modprobe nvidia && sudo modprobe nvidia_uvm`
- Missing → the user installs it (cloud / Brev images already ship the driver). Per-platform pinned versions are in the VSS prerequisites (e.g. Ubuntu 24.04 → `580.105.08`, 22.04 → `580.65.06`); download: https://www.nvidia.com/en-us/drivers/

---

## 2. Docker + Compose

```bash
docker --version        # need 28.3.3+  AND  < 29.5.0
docker compose version  # need v2.39.x+
docker ps               # verify no sudo needed
```

**Pass**: `28.3.3 <= Docker < 29.5.0`, Compose >= v2.39.

> **⚠️ Upper bound `< 29.5.0`.** Docker Engine **29.5.0+ breaks NGC image pulls**
> (`error from registry: Incorrect Repository Format`, after the layers download). On
> `29.5.0`+: pin to **28.3.3** (the VSS prerequisites / `warehouse.md` have the
> pinned-install commands) or apply the containerd-snapshotter daemon workaround. Note
> `docker login` + `manifest inspect` still succeed on 29.5.x — only the actual image
> pull fails, so it won't surface until deploy.

**Fail** (`docker: command not found`): Guide to https://docs.docker.com/engine/install/ubuntu/

**Fail** (sudo needed):
```bash
sudo usermod -aG docker $USER && newgrp docker
```

---

## 3. NVIDIA Container Toolkit

```bash
docker info 2>/dev/null | grep -i "runtimes"
docker run --rm --gpus all nvidia/cuda:12.6.3-base-ubuntu24.04 nvidia-smi 2>&1 | head -8
```

**Pass**: `nvidia` runtime listed, GPU info from container.

**Fail** — **auto-install**:
```bash
curl -fsSL https://nvidia.github.io/libnvidia-container/gpgkey \
  | sudo gpg --dearmor -o /usr/share/keyrings/nvidia-container-toolkit-keyring.gpg
curl -s -L https://nvidia.github.io/libnvidia-container/stable/deb/nvidia-container-toolkit.list \
  | sed 's#deb https://#deb [signed-by=/usr/share/keyrings/nvidia-container-toolkit-keyring.gpg] https://#g' \
  | sudo tee /etc/apt/sources.list.d/nvidia-container-toolkit.list
sudo apt-get update && sudo apt-get install -y nvidia-container-toolkit
sudo nvidia-ctk runtime configure --runtime=docker
sudo systemctl restart docker
```

---

## 4. NGC CLI

```bash
ngc --version
```

**Fail** — **auto-install**:
```bash
ARCH=$(uname -m)
if [ "$ARCH" = "x86_64" ]; then NGC_ZIP="ngccli_linux.zip"; else NGC_ZIP="ngccli_arm64.zip"; fi
curl -sLo /tmp/ngccli.zip "https://api.ngc.nvidia.com/v2/resources/nvidia/ngc-apps/ngc_cli/versions/4.10.0/files/$NGC_ZIP"
sudo mkdir -p /usr/local/lib
sudo unzip -qo /tmp/ngccli.zip -d /usr/local/lib
sudo chmod +x /usr/local/lib/ngc-cli/ngc
sudo ln -sfn /usr/local/lib/ngc-cli/ngc /usr/local/bin/ngc
ngc --version
```

---

## 5. NGC API Key + Docker Login

> **SECURITY: NEVER echo, print, or hardcode the NGC API key in commands.**
> Read it silently from `~/.bashrc` or `~/.ngc/config`. Do not show the
> key value in any tool output.

```bash
# Check env var (shows SET/NOT SET, never the actual key value)
if [ -n "$NGC_CLI_API_KEY" ]; then echo "NGC_CLI_API_KEY: SET"; else echo "NGC_CLI_API_KEY: NOT SET"; fi

# Check ~/.ngc/config
ngc config current 2>/dev/null | grep -q "apikey" && echo "NGC config: key present" || echo "NGC config: no key"

# Check ~/.bashrc (key may be exported there but not in current shell)
grep -q "NGC_CLI_API_KEY" ~/.bashrc 2>/dev/null && echo "bashrc: key present" || echo "bashrc: no key"
```

> **DO NOT** use `echo "${VAR:+SET}${VAR:-NOT SET}"` — when `VAR` is set,
> `${VAR:-NOT SET}` expands to the **value** of `VAR`, leaking the key into
> stdout. Always use the `if [ -n "$VAR" ]` form above.

**Pass**: Key present in any of: env var, `~/.ngc/config`, or `~/.bashrc`.

If key is in `~/.bashrc` but not in current shell, source it silently:
```bash
eval "$(grep NGC_CLI_API_KEY ~/.bashrc)"
```

**Fail** (no key in any source) — guide user:
1. https://ngc.nvidia.com → Setup → API Keys → Generate Personal Key
2. Set NGC Catalog permission
3. User adds the key themselves — do NOT ask user to paste the key into chat

**Docker login** (after key is available):
```bash
docker login nvcr.io -u '$oauthtoken' -p "$NGC_CLI_API_KEY"
```

### Verify Access — NGC Resources + Docker Images

Check both NGC CLI resource access and Docker image pull access:

Resource paths and image tags are pinned in the profile env (the single source of truth) —
source the profile you will deploy, then check (`sil` / `hil` shown; `base` needs only the
PSF image, verified at its `docker pull` step):

```bash
set -a; source deployments/profiles/sil.env; set +a   # or hil.env

# NGC CLI — Halos SIL data
ngc registry resource info "$MDX_DATA_RESOURCE" > /dev/null 2>&1 \
  && echo "PASS: Halos SIL data accessible via NGC CLI" \
  || echo "FAIL: Cannot access Halos SIL data — check NGC key / org access"

# Docker — Isaac Sim image
docker manifest inspect "$ISAAC_SIM_IMAGE" > /dev/null 2>&1 \
  && echo "PASS: Isaac Sim image accessible" \
  || echo "FAIL: Cannot access Isaac Sim image"
```

**Any FAIL on `outside-in-safety` resources**: your NGC key is not authorized for that org.
Confirm `ngc config set` and `docker login nvcr.io` with a key that has access, then re-run
the checks.

**Do NOT proceed until all checks pass** — deployment will fail at
download or `docker compose up` without proper access.

---

## 6. Kernel settings — sysctl + cgroup (VSS Elasticsearch + Kafka)

VSS components HOISA depends on (Elasticsearch, Kafka) require these. Apply before deploying:
```bash
sudo sysctl -w vm.max_map_count=262144
sudo sysctl -w net.core.rmem_max=5242880
sudo sysctl -w net.core.wmem_max=5242880
```
Verify Docker uses the cgroupfs driver:
```bash
cat /etc/docker/daemon.json | grep cgroupfs   # expect: "exec-opts": ["native.cgroupdriver=cgroupfs"]
```
> The VSS prerequisites persist these to `/etc/sysctl.d/` and cover the full set — see there.

---

## 7. System Resources

```bash
nproc                    # need 16+
free -g | grep Mem       # need 32 GB+
df -h / | tail -1        # need 200 GB+ free
```

**Pass**: 16+ cores, 32 GB+ RAM, 200 GB+ SSD.

---

## Summary

```
Prerequisites:
  1. GPU (24GB+ VRAM, driver 580+; Isaac needs RT cores >20GB)  ✅/❌
  2. Docker 28.3.3+ AND < 29.5.0 / Compose 2.39+                ✅/❌
  3. Container Toolkit                                          ✅/❌
  4. NGC CLI                                                    ✅/❌
  5. NGC API Key + Docker Login                                 ✅/❌
  5a. NGC Resources + Images (outside-in-safety)                 ✅/❌
  6. Kernel sysctl + cgroup (VSS ES/Kafka)                      ✅/❌
  7. System Resources                                          ✅/❌
```
