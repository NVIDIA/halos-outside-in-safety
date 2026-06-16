# AI Perception

Halos Outside-In Safety **consumes** a perception backend rather than implementing one.
The reference perception is **NVIDIA VSS Blueprint** (specifically the **Warehouse Operations** example) — partner-configurable and
**swappable**: any perception stack that satisfies the integration contract below can
drive the safety core.

## Reference backend: VSS Blueprint
- Repo: https://github.com/NVIDIA-AI-Blueprints/video-search-and-summarization
- Deployed via the `vss-deploy-profile` skill (see [`skills/`](../skills/)) or the public VSS Blueprint docs.

## Integration contract (the seam)
The Safety Core depends only on the **event stream**, not on perception internals:
- **Transport:** Kafka
- **Topics:** `mdx-events` (2D — ROI / tripwire behaviors) · `mdx-frames` (Analysis of each frame in context of detected objects and conditions in calibration)
- **Schema:** MDX protobuf messages — the Metropolis `mdx-messages` schema. The protobuf codec ships in `safety-core/adapters/vss/mdx-msg-codec/` (generated message code under `proto/gen/mdx-messages/`), consumed by the MDX client in `safety-core/adapters/vss/mdx-client/`.

To bring your own perception, publish events that match this contract; the input seam
lives in `safety-core/adapters/`.

<!-- TODO: expand — exact event schema, supported profiles (2D/3D),
     version compatibility matrix, and the swap-in procedure. -->
