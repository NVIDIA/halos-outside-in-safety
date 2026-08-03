# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Halos SIL post-setup_simulation USD-prim modifications.

These are NOT OmniGraph ActionGraphs. They are one-shot USD stage
modifications that fire from `run_actor_sdg.py`'s
`SET_UP_SIMULATION_DONE_EVENT` callback to fix things IRA does not
allow via config (deterministic char positions, prim deactivation, etc).

Companion to `sil/scripts/action_graphs/`:
  - action_graphs/   builds OmniGraph ActionGraphs (RTSP, SRR, etc)
  - runtime_patches/ tweaks USD prim state (deactivate, set xform, etc)

Both packages share the same lifecycle hook (post-setup_simulation,
pre-Play) but solve different problems and have different shapes.

Each module here exposes a single `apply_<name>_patches()` function
that is idempotent (safe to call multiple times) and prints
`[<module-tag>] ...` lines so the operator sees progress.

Adding a new patch:
  1. Create `<your_name>_patches.py` in this directory.
  2. Implement `apply_<your_name>_patches() -> None`.
  3. Add the public name to `__all__` below.
  4. Wire a CLI flag (`--no-<your-name>`) and an invocation in
     `run_actor_sdg.py`'s setup-done callback.
"""

from .halos_runtime_patches import apply_halos_runtime_patches

__all__ = [
    "apply_halos_runtime_patches",
]
