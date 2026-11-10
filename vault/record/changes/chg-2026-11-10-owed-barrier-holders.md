---
id: chg-2026-11-10-owed-barrier-holders
type: chg
title: "A free's stand-down lets every context holding it go, the owed-free pass serves contexts with parked chains in turn, and a destroyed context's blob births are cleared"
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
Supersedes chg-2026-11-10-owed-barrier-per-free. Vote 81 audit round 11 (Opus 5.5 on Opus 5.5) on the per-free barrier: 0 P0, 0 P1, 1 P2, 3 P3, no soundness defect. F1 (P2): the barrier built its context list from every free not standing down, so a context holding two owed frees stayed parked through each one's window, the two standing down in turn, for as long as a load stalled every repeat; two destroys under such a load leave exactly that. Now `gpu_api::owed_barrier` returns the frees that hold and the one standing down, and `OwedReach::of` takes every context that holds a free standing down out of the list, whatever else it holds; a transfer naming a free that holds still parks. A free is tried only while no chain on the device can reach it, so the work of the contexts its stand-down lets go cannot have caused its stall, which keeps round 10's F1 closed. F1's addendum (P2): under that load the pass tries one free a window, and in rotation a context waited for its own free's turn once per owed free, which another client could make hundreds by owing cheap frees on an idle context. Each pass now first tries one free a parked chain waits on, taking the contexts with parked chains in turn (`gpu_api::OwedPass`, `Gpu::owed_turn`), then the rest in rotation, so a context waits once per context with a parked chain; `FreesOwed` counts those turns (`Frees::parked_first`, warp ctl `frees-parked-first`). F2 (P3): LIFT read a stall the pass did not meet as FAIL; now unconstructed. F3 (P3): nothing witnessed the end of a free's window. LIFT now takes two frees on one context, has the pass try the first as that context's turn and stall on it, requires the context's chain on the device in the window though it holds the other free, and requires its next chain, sent after the window, to park. F4 (P3, pre-existing): a completed context destroy left the births of blobs minted under it, so a context later given the same id held another client's owed host-blob frees; `Objects::context_destroyed` now clears them. Pi mutants: M27 (no holder release), M28 (the barrier reads the engine as refusing, so a free stays down until another stall), M29 (the pass is given no parked chains), each LIFT=FAIL; M25 and M26 re-anchored on `owed_barrier`.
