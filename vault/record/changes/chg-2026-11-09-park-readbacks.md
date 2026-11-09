---
id: chg-2026-11-09-park-readbacks
type: chg
title: "Lictor parks a readback until its context's earlier chains retire (vote 81)"
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
Implements [[dec-2026-11-09-park-readbacks]]. The fenced lane parks a `TRANSFER_FROM_HOST_3D` while its context has a chain the device may still run, in flight or abandoned and unretired, and parks that context's later chains behind it (`gpu_api::must_park`). The completion pump publishes a parked readback once its context is clear, with the chains behind it up to the next readback (`gpu_api::unpark_plan`); the wire id is taken at publication (`Controlq::publish_staged`), the owner id at submission. A parked chain holds its slot and counts for its context and for the compositor's pump (`fenced_outstanding`), but not as device work (`fenced_in_flight`, `device_idle`, the abandonment clock). `retirement_hold` counts parked reaches as in flight. A dead engine releases parked chains as refused; a compositor readback parked behind an abandoned chain is released as refused so the reserved slot is not held for every surface; a retirement the harness hold swallowed does not hold a parked chain. Each `FenceTag` carries `parked_ms` and `exec_ms`, shown on the ctx ctl (`readback-parked-ms`, `readback-exec-ms`) and the warp ctl (`rb-last-parked-ms`, `rb-last-exec-ms`). The C-6 gate's DEEP check reads the fence timeline at issue, and its DEADLINE arm judges PARKED, ORDER, BOUND and HEALTH.
