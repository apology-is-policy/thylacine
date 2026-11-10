---
id: chg-2026-11-10-owed-barrier
type: chg
title: "A chain that can reach an owed free parks until the free is paid, so a busy context cannot hold its taken free off"
date: 2026-11-10
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
created: 2026-11-10
---
Vote 81 audit round 7 (Opus 5.5 on Opus 5.5): 0 P0, 1 P1, 0 P2, 0 P3. F1 (P1, an incomplete closure of r6 F1): after the vindication the context is live again, and the refused `CtxDetachResource` left its ledger link to the taken free's resource, so every new chain of the context reached the free and the repeat's hold parked it; a context kept rendering held the free off for good, while tapestryd had uncharged it. Making the repeat ignore those chains would be unsound, since until the detach runs a new chain can name the resource. Lictor now parks a chain that can reach an owed free, with its context's later chains, until the free is paid (`gpu_api::OwedReach`, an argument to `must_park` and `unpark_plan`), built before every completion pump, the only place a parked chain publishes (`Device::refresh_owed_reach`, from `Objects::held_by` and `Objects::holders`). Only frees no poisoned chain reaches hold chains back (`OwedReach::of`), since one a poisoned chain reaches waits for the vindication anyway and a chain parked behind it would have no clock. The repeat's hold leaves parked chains out (`Gpu::owed_free_hold`): none is on the device or publishes during the pass, and counting one that waits for the free would hold each on the other. Witness: the readback leg's BUSY arm takes a free as TAKEN does, has the pass skip it with the test lever `warp-defer-frees` (`Request::TestOwedDefer`, opcode 72: the caller's own context, four arms a boot of 2 s at most), vindicates the context, sends its next chain held, and passes iff the free is paid within 1 s with its id retired. Pi mutants M19 (no barrier), M20 (the repeat counts parked chains), M21 (the pump ignores the barrier), each BUSY=FAIL; M17 re-anchored. Doc: the owed-free pass adds at most about a second before the seat's next look at the chord, not "within about a second" of it.
