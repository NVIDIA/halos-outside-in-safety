#!/bin/bash
# Brev Launchable setup script — Halos Outside-In Safety (SIL)
#
# Brev runs this ONCE after the VM is ready (as root). The SIL notebook needs
# TWO repos present in the interactive user's home:
#   - halos-outside-in-safety        (this repo — the notebook + compose live here)
#   - video-search-and-summarization (VSS perception backend)
#
# Brev's "git repository" code option clones only ONE repo, and it does not
# reliably honor a branch whose name contains a "/" (e.g. a feat/* branch) from
# the blob URL — it tends to land on the default branch (main), which does not
# have the notebook. So this script guarantees BOTH repos are present AND the
# Halos repo is checked out on the branch that actually contains the notebook.
set -euo pipefail

# Branch of halos-outside-in-safety that holds the launchable deliverables.
# Tracks "develop" (where the launchable is merged); switch to "main" once it
# lands there. Override at launch with the HALOS_REF env var (e.g. to test a
# feature branch before it is merged).
HALOS_REF="${HALOS_REF:-develop}"

# --- resolve the interactive (non-root) user + home (Brev runs this as root) ---
TARGET_USER="${SUDO_USER:-}"
if [ -z "${TARGET_USER}" ] || [ "${TARGET_USER}" = "root" ]; then
  TARGET_USER="$(ls /home 2>/dev/null | head -1)"   # e.g. ubuntu
fi
TARGET_HOME="$(getent passwd "${TARGET_USER}" | cut -d: -f6)"
TARGET_HOME="${TARGET_HOME:-/home/${TARGET_USER}}"
echo "[brev_setup] target user=${TARGET_USER} home=${TARGET_HOME} halos_ref=${HALOS_REF}"

command -v git >/dev/null 2>&1 || { apt-get update -y && apt-get install -y git; }

# --- 1) VSS perception backend (public, default branch = main) ---------------
VSS_DIR="${TARGET_HOME}/video-search-and-summarization"
if [ ! -d "${VSS_DIR}/.git" ]; then
  echo "[brev_setup] cloning VSS -> ${VSS_DIR}"
  git clone --depth 1 \
    "https://github.com/NVIDIA-AI-Blueprints/video-search-and-summarization.git" \
    "${VSS_DIR}"
else
  echo "[brev_setup] already present: ${VSS_DIR}"
fi
chown -R "${TARGET_USER}:${TARGET_USER}" "${VSS_DIR}" 2>/dev/null || true

# --- 2) Halos repo — MUST end up on ${HALOS_REF} (the notebook lives there) ---
# Brev may have already cloned this at the default branch (main, no notebook),
# or not cloned it at all; handle both and force it onto ${HALOS_REF}.
HALOS_DIR="${TARGET_HOME}/halos-outside-in-safety"
if [ -d "${HALOS_DIR}/.git" ]; then
  echo "[brev_setup] ensuring ${HALOS_DIR} is on ${HALOS_REF}"
  git -C "${HALOS_DIR}" fetch --depth 1 origin "${HALOS_REF}"
  git -C "${HALOS_DIR}" checkout -B "${HALOS_REF}" FETCH_HEAD
else
  echo "[brev_setup] cloning Halos @ ${HALOS_REF} -> ${HALOS_DIR}"
  git clone --depth 1 --branch "${HALOS_REF}" \
    "https://github.com/NVIDIA/halos-outside-in-safety.git" "${HALOS_DIR}"
fi
chown -R "${TARGET_USER}:${TARGET_USER}" "${HALOS_DIR}" 2>/dev/null || true

echo "[brev_setup] done. In JupyterLab open:"
echo "  halos-outside-in-safety/deployments/scripts/deploy_hoisa_launchable.ipynb"
echo "  -> set NGC_CLI_API_KEY in Section 1, then Run All."
