# AI Perception

Halos Outside-In Safety **consumes** a perception backend rather than implementing one.
The reference perception is **NVIDIA VSS Blueprint** (specifically the **Warehouse Operations** example) — partner-configurable and
**swappable**: any perception stack that satisfies the integration contract below can
drive the safety core.

## Reference backend: VSS Blueprint
- Repo: https://github.com/NVIDIA-AI-Blueprints/video-search-and-summarization
- VSS 3.3 (GitHub release `v3.3.0rc0`): deployed with the VSS repo's [`vss-build-vision-ai`](https://github.com/NVIDIA-AI-Blueprints/video-search-and-summarization/tree/v3.3.0rc0/skills/vss-build-vision-ai) skill, warehouse profile, plus the Halos settings in [`vss_2d_overrides.md`](../skills/hoisa-deploy-profile/references/vss_2d_overrides.md) / [`vss_3d_overrides.md`](../skills/hoisa-deploy-profile/references/vss_3d_overrides.md).

## Integration contract (the seam)
The Safety Core depends only on the **event stream**, not on perception internals:
- **Transport:** Kafka
- **Topics:** `mdx-events` (2D — ROI / tripwire behaviors) · `mdx-frames` (Analysis of each frame in context of detected objects and conditions in calibration)
- **Schema:** MDX protobuf messages — the Metropolis `mdx-messages` schema. The protobuf codec ships in `safety-core/adapters/vss/mdx-msg-codec/` (generated message code under `proto/gen/mdx-messages/`), consumed by the MDX client in `safety-core/adapters/vss/mdx-client/`.

To bring your own perception, publish events that match this contract; the input seam
lives in `safety-core/adapters/`. The rule format that turns those MDX messages into
Safety Core events is documented in [Event Mapping Configuration](https://docs.nvidia.com/halos-outside-in/1.3/reference/event-mapping-configuration.html).

<!-- TODO: expand — exact event schema, supported profiles (2D/3D),
     version compatibility matrix, and the swap-in procedure. -->
