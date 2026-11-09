---
id: chg-2026-11-09-park-readbacks-r2p3
type: chg
title: "Vote 81 audit round 2 closed: the released stream latch, slot witnesses, stall retries, P3s"
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
Follows [[chg-2026-11-09-park-readbacks-r2f1]]; closes round 2's P3s. F5 replaces that change's owed count: paying it at the vindication resumed a stream lacking the released draws for a client that never waited inside the poisoned window. A released client chain is never counted; it raises `WarpCtx::released`, which ends the context's fence stream for good (both fence-file readers EOF, `warp_fenced_admit` and `verify` refuse E_IO, the compose path treats it as poisoned; the ctx ctl reports `released N`), while device liveness stays `fence_poisoned`'s, so vindication, reclamation and destroy are unchanged. F2: `comp_readback_retired` returns for a released compositor readback (`rb_dropped`) before charging `cost readback-wait`. F3: Lictor's `fenced_held` returns parked chains too, flagged (`gpu_api::HeldRow`; the wire gains the 8-tuple), and the warp ctl prints `fenced-parked` rows. F4: DEADLINE re-sentinels the backing and requires the compositor readback census unchanged across the arm. F6: ABANDON requires `fenced-free` back at its pre-hold value, and CLEAN waits, bounded, for rb-slot 0 and a full client pool and says PASS. F7: the readback say is throttled to doublings of the longest park or run said (`Controlq::rb_said`). F8: a process-wide stall-refusal count lets DEADLINE and ABANDON retry an attempt whose sends met an engine stall (up to four), rather than judging a readback that published unparked.
