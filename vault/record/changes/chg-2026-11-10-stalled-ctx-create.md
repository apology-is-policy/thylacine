---
id: chg-2026-11-10-stalled-ctx-create
type: chg
title: "A warp context create a stall met condemns its slot and owes a destroy, instead of wedging the slot"
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
Found by the vote-81 audit round 4 (F1, P1, pre-existing). A warp `CTX_CREATE` is a synchronous step whose fence retires behind other clients' queues; behind more than `SUBMIT_DEADLINE_MS` of draws it stalls, and lictor keeps the id's reservation (the outcome is unknown, GPU-DESIGN 4.5.15). `wctx_mint` returned without condemning the slot, so every later mint picked the same lowest free slot and lictor refused the reserved id: no GL context box-wide until reboot, from one stall an unprivileged client can arrange. `wctx_venus_ensure` had the same hole for the venus band. Now a create a stall met condemns its slot and owes the id a destroy (`warp_ctx_destroy_owed`, `warp_ctx_venus_destroy_owed`); `warp_destroy_owed` runs it once the engine is live, ordered after the create, and a completed destroy frees the slot. Lictor's `CtxDestroy` un-retires a context when the destroy left nothing (refused before staging by a standing stall, or by the device), so `wctx_finish`'s clean-arm destroys refused by a standing stall are owed rather than condemning for good. The released-chain record (vote 81) is forgotten only after a destroy completes, and no longer at create (round 4 F2: the create-side forget rested on a host property that does not hold). Host test `a_retirement_that_left_nothing_is_undone_and_only_by_its_own_ticket`. The guest witness (a stall shot across `ctx/new`) needs its own test lever and follows.
