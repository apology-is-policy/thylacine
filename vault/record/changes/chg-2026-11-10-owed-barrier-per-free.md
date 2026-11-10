---
id: chg-2026-11-10-owed-barrier-per-free
type: chg
title: "A free's barrier stands down only after a stall its own repeat met; the owed-free pass still stands down after any of its stalls"
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
Supersedes chg-2026-11-10-owed-stand-down. Vote 81 audit round 10 (Opus 5.5 on Opus 5.5): 0 P0, 1 P1, 0 P2, 4 P3, no soundness defect; two further P3s were withdrawn by the auditor after it re-derived the serve order. F1 (P1): the stand-down was one flag for the whole debt, so a stall the pass's repeat of any free met emptied the barrier for every free. A context kept busy enough to make the repeat of another client's free stall had its parked chains let go at each such revival, and refilled the queue for the window, so its own free parked again at the window's end and the other free stalled again: round 9's cycle, with the pass's own repeat as the slow command. The pass still stands down after any stall one of its commands met (the window goes to the requests), but a free's barrier stands down only after a stall its own repeat met (`OwedFree::stood_down`, `gpu_api::barrier_ids`, host-tested). F4 (P3): the sentence "the pass sends first, into a queue the stall emptied" was false, since the revival's pump publishes parked chains no barrier holds (a readback's run) before the pass sends; the docs now say the pass takes its turn ahead of the requests after whatever that pump published. F5 (P3): the HOST3D ring paragraph said the venus stream is decoded at submit; commands the ring carries are decoded later by the renderer's ring thread, and they miss because the free empties the context's resource table. F2 (P3): FREE's prompt-repay bound had no stall guard; a stall after the destroy's now makes the attempt unconstructed. F7 (P3): the barrier's half of the stand-down had no witness. `FreesOwed` and the warp ctl now count the pass's stand-downs (`frees-stood-down`). The readback leg gains APART (another client's free and its repeat each meet a stall shot while a chain waits parked behind a deferred free; the chain must stay parked) and LIFT (the parked-for free's own repeat meets a stall shot; the chain must go to the device in that window, the free unpaid while it is held). YIELD now needs the pass's stand-down count to move, and the stall-repay lever's arming waits out an earlier attempt's shot. Pi mutants: M25 (the barrier follows the pass's stand-down, round 10's code) APART=FAIL; M26 (no free's barrier stands down) LIFT=FAIL. Budgets: twenty tapestryd stall shots, twelve owed-stall shots and sixteen defer arms a boot.
