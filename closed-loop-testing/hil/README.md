# HIL — Hardware-in-the-Loop

The `hil` profile splits the `sil` closed loop across two machines: an **x86 stimulus host**
(Isaac Sim + comm-layer + forklift-controller, `COMPOSE_PROFILES=hil`) and an **IGX Thor
safety host** running VSS Warehouse perception and the Thor Safety Core. The safety decision
crosses the network back to the x86 comm-layer, closing the loop over two hosts.

Full documentation: [Hardware-in-the-Loop Testing](https://docs.nvidia.com/halos-outside-in/1.3/testing/hil/index.html) — [Deploy the HIL Closed Loop](https://docs.nvidia.com/halos-outside-in/1.3/testing/hil/quickstart.html) is the two-host bring-up.

Runbook: [`skills/hoisa-deploy-profile/references/halos_hil.md`](../../skills/hoisa-deploy-profile/references/halos_hil.md) —
prerequisites, deploy order (start the Isaac scenario **before** VSS on the Thor), the
scene-clear start gate, ready signals, and hil-specific troubleshooting.

Configuration:

- `deployments/profiles/hil.env` — the x86 stimulus side (`docker compose --env-file profiles/hil.env up -d`)
- `deployments/profiles/hil-thor.env` — the Thor side, consumed by `closed-loop-testing/scripts/launch_thor_safety.sh hil-thor`
- `closed-loop-testing/scripts/hil_preflight.sh` — two-host preflight, run from the x86 host before bring-up
