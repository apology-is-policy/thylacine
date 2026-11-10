---
id: chg-2026-11-09-park-readbacks-f1
type: chg
title: "A parked chain behind an abandoned chain is released, not kept (vote 81 audit F1)"
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
Follows [[chg-2026-11-09-park-readbacks]]. The vote-81 audit round's F1: a chain parked behind an abandoned chain that never retires stayed parked for good, since a parked chain has no abandonment clock, and it kept `fenced_outstanding` above zero, so tapestryd's serve loop polled at 1 ms indefinitely, the spin the abandonment clock exists to bound. Before vote 81 such a readback was on the device and was abandoned with its prefix. `gpu_api::unpark_plan` now answers `Unpark::Release` for every parked chain of a context with an abandoned, unretired chain (`ctx_poisoned`), and the pump releases it as refused (`ok = false`, not abandoned), returning its slot; the compositor-only release it replaces is the special case. tapestryd reports readback timings only for a readback that landed (`tag.ok`). The C-6 gate gains ABANDON: a held context's readback parks behind three heavy submits, the abandon must take exactly those three, and the context's fences in flight must reach zero within two seconds. Host test `a_poisoned_contexts_parked_chains_are_released_and_no_one_elses`, red with the release branch removed.
