---
id: chg-2026-11-10-owed-quiet
type: chg
title: "The owed-free pass and its barrier wait for a quiet engine, so a sustained load cannot take every revived window or freeze the owner's chains"
date: 2026-11-10
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-lictor
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-10
---
Vote 81 audit round 8 (Opus 5.5 on Opus 5.5) on the owed-free barrier: 0 P0, 0 P1, 0 P2, 4 P3, no soundness defect. F4 (P3): under a load that makes every synchronous command miss its deadline, each repeat of an owed free stalls, its answer is unknown and never counts toward giving up, so the barrier kept the owner's chains parked for as long as the load lasted, where before they ran in the revived windows; an ordinary destroy under such a load leaves that state with no poison (its detach stalls, so the link stays, and its free stalls and is owed). A self-found twin (P2, in this arc's unlanded code since the owed-free pass landed): the serve loop runs the pass after the seat's step and before the requests, so the pass re-stalled the engine at every revival before any request of that pass ran, and no request saw a live engine while the load lasted. Lictor's pass and its barrier now wait for a quiet engine (`gpu_api::owed_quiet`, `OWED_QUIET_MS`, a synchronous deadline, asserted equal): nothing while the engine is stalled, and nothing for a deadline after it revives (`Controlq::revived_at`). The requests keep the revived windows, the held-back chains go to the device, and the free is paid once the engine has been quiet for a deadline. Host-tested; no leg drives a sustained load. F1-F3 (P3) doc truths: the barrier's refresh is bounded by the serve pass (every pass begins with a pump), not by "every change ends in a pump", which a taken free's early return and the seat's restore both break; GPU-DESIGN 4.5.13's accounting bound and stalled-engine rule now name the owed-free keep; the compositor's readback of a client's BO waits behind that client's parked chains and holds the reserved slot meanwhile. The readback leg's BUSY arm now also requires the chain that waited for the free to run once the hold lifts.
