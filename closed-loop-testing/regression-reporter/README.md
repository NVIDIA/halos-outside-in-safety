# Regression Testing Reporter

**Regression Testing Reporter** for the SIL stack. Runs scenarios in Isaac Sim, records ground truth + safety-system state at 30 Hz, splits by forklift tripwire crossings, and produces graded per-clip reports + per-clip review videos.

The short name `srr` is used throughout the code, the service, and the paths below, retained from the tool's original name, Scenario Recorder + Reporter.

It lives at `closed-loop-testing/regression-reporter/`; the report skill is a sibling at `skills/hoisa-generate-regression-report/` and the browser viewer at `tools/srr-debug-viewer/`.

![SRR architecture](assets/SRR-arch.png)

![SRR debug viewer — clip page](assets/srr-debug-viewer-demo.jpeg)

*SRR debug viewer — per-clip page with scrub-synced Safety Core / GT / BA / pss.log panels.*

## Documentation

Setup, configuration, and the run/verify walkthrough are in the [Regression Testing Reporter docs](https://docs.nvidia.com/halos-outside-in/1.3/testing/srr/index.html); start with the [Quick Start Guide](https://docs.nvidia.com/halos-outside-in/1.3/testing/srr/quickstart.html). In-repo entry points:

- [`skills/hoisa-generate-regression-report/`](../../skills/hoisa-generate-regression-report/) — report skill (multi-test orchestration).
- [`tools/srr-debug-viewer/`](../../tools/srr-debug-viewer/) — browser viewer for per-clip MP4 + synced panels.
- [`halos-integration/README.md`](halos-integration/README.md) — how SRR plugs into the SIL stack.
