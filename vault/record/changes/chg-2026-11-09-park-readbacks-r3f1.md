---
id: chg-2026-11-09-park-readbacks-r3f1
type: chg
title: "Lictor keeps the released-stream latch where the release happens (vote 81 audit r3 F1)"
date: 2026-11-09
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-lictor
  - sub-tapestryd
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-09
---
Follows [[chg-2026-11-09-park-readbacks-r2p3]]. Round 3's F1: tapestryd's `WarpCtx::released` latch takes effect only when its pump takes the released tag, but Lictor releases a poisoned context's parked chains in the reap that trails any request, `TakeCompletions` included, and in its own serve loop. In the pass where that happens tapestryd still admits the context's next chain from its older view, and Lictor, with nothing of that context parked any more, published it: it ran after a readback that never ran, and its completion (with the abandoned chain's vindication) carried the dense count to the readback's fence, so the winsys's `warp_fence_wait` returned success on bytes never written. A readback admitted after the vindication published the same way. Now `gpu_api::release_tag` records a released client chain's context in `gpu_api::Released` (one entry per device context, forgotten at `ctx_destroy` or a successful `ctx_create_capset`); `must_park` parks every later chain of that context and `unpark_plan` releases it. A compositor readback's release records nothing, as in tapestryd. Host tests `nothing_of_a_context_runs_after_its_released_chain_while_its_device_context_lives` and `a_released_record_is_one_per_device_context_and_never_a_compositor_readbacks`; red with either check removed or with the record dropped from `release_tag`.
