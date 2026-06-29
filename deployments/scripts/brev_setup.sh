#!/bin/bash
# Brev Launchable setup script — Halos Outside-In Safety (SIL)
#
# Brev runs this ONCE after the VM is ready (as root). The SIL notebook needs
# TWO repos present in the interactive user's home:
#   - halos-outside-in-safety        (this repo — the notebook + compose live here)
#   - video-search-and-summarization (VSS perception backend)
# Brev's "git repository" code option only clones ONE repo, so this script
# guarantees both are present before the user clicks "Run All".
set -euo pipefail

# --- resolve the interactive (non-root) user + home (Brev runs this as root) ---
TARGET_USER="${SUDO_USER:-}"
if [ -z "${TARGET_USER}" ] || [ "${TARGET_USER}" = "root" ]; then
  TARGET_USER="$(ls /home 2>/dev/null | head -1)"   # e.g. ubuntu
fi
TARGET_HOME="$(getent passwd "${TARGET_USER}" | cut -d: -f6)"
TARGET_HOME="${TARGET_HOME:-/home/${TARGET_USER}}"
echo "[brev_setup] target user=${TARGET_USER} home=${TARGET_HOME}"

command -v git >/dev/null 2>&1 || { apt-get update -y && apt-get install -y git; }

clone_repo() {  # $1=url  $2=dest
  if [ ! -d "$2/.git" ]; then
    echo "[brev_setup] cloning $1 -> $2"
    git clone --depth 1 "$1" "$2"
  else
    echo "[brev_setup] already present: $2"
  fi
  chown -R "${TARGET_USER}:${TARGET_USER}" "$2" 2>/dev/null || true
}

# Both repos are PUBLIC, so no token is needed.
# 1) VSS perception backend
clone_repo "https://github.com/NVIDIA-AI-Blueprints/video-search-and-summarization.git" \
           "${TARGET_HOME}/video-search-and-summarization"

# 2) Halos Outside-In Safety repo.
#    If Step-1 "git repository" already points Brev at this repo, it is cloned
#    already and the guard below skips it. Kept here so the script also works when
#    Step-1 is "I don't have any code files".
clone_repo "https://github.com/NVIDIA/halos-outside-in-safety.git" \
           "${TARGET_HOME}/halos-outside-in-safety"

echo "[brev_setup] done. In JupyterLab open:"
echo "  halos-outside-in-safety/deployments/scripts/deploy_hoisa_launchable.ipynb"
echo "  -> set NGC_CLI_API_KEY in Section 1, then Run All."
