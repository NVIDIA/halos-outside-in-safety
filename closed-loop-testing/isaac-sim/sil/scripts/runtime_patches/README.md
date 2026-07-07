# `sil/scripts/runtime_patches/` — post-setup USD prim modifications

This package contains the one-shot USD stage tweaks that
`run_actor_sdg.py` fires from its `SET_UP_SIMULATION_DONE_EVENT`
callback, *not* OmniGraph ActionGraphs. (For ActionGraph builders see
the sibling `sil/scripts/action_graphs/` package.)

## What lives here

| Module | Public entry point | Purpose |
|---|---|---|
| `halos_runtime_patches.py` | `apply_halos_runtime_patches()` | Deactivate 3 legacy 5.1 baked Character prims + move 3 IRA-spawned chars to canonical positions. The only path today to deterministically place IRA chars. |
| *(future)* `<your_name>_patches.py` | `apply_<your_name>_patches()` | New patch — see "Adding a new patch" below. |

## Invocation contract

Each entry point:

1. **Imports omni.* lazily** inside the function body. The module is imported at the top of `run_actor_sdg.py`, which runs before `SimulationApp` is instantiated — `omni.usd`, `pxr` etc. are not safe to import at module load time.
2. **Takes no arguments** (or a single optional config_path if the patch needs external data). Acts on the currently-open USD stage.
3. **Is idempotent.** Calling twice in the same Kit session produces the same end state.
4. **Prints `[<module-tag>] ...`** progress lines.
5. **Returns `None`.**

## Adding a new patch

1. Create `<name>_patches.py` here. Implement `apply_<name>_patches() -> None`.
2. Add the public name to `__all__` in `__init__.py`.
3. In `run_actor_sdg.py`:
   - Add a CLI flag (`--no-<name>-patches`).
   - Import + call inside the setup-done block when the flag is set.

## Difference from `action_graphs/`

| | `action_graphs/` | `runtime_patches/` (this package) |
|---|---|---|
| What it does | Builds an OmniGraph at `/World/<Name>Graph` | Tweaks USD prim state (active flag, xform, schema, etc) |
| Output | `og.Graph` returned | `None` |
| Lifetime | The graph ticks every frame after Play | One-shot, runs once in setup-done callback |
| Re-running effect | Old graph deleted + rebuilt | Idempotent attribute re-author |

Both packages share the same setup-done lifecycle hook and the same
"flat module-scope function" pattern.
