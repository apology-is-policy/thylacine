---
id: chg-2026-11-09-takeover-bound-round6
type: chg
title: "The late shot leaves Lictor's phase where the kernel's expiry would (round 6)"
date: 2026-11-09
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-lictor
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-09
---
Round 6 over [[chg-2026-11-09-takeover-bound-round5]] (Opus 5.5 on Opus 5.5, read-only, on its one commit; 0 P0, 0 P1, 0 P2, 2 P3). The audit closes clean. Round 6 found that the code change behaves as before: the only pump inside the old window was the phase-1 reap. F1: leg K's late shot still reached FAILED through `fail()`, which sets Lictor's phase to 4, so round 4's end-of-step clear keyed to Lictor's phase (with the post-reap clear removed) stayed green. In production, the kernel can fail an expired chord inside a diagnostic's dropped STATUS while Lictor still reads phase 1. That shape would then kill a stall only the restore waits on. The late shot (test mode only) now sends FAIL to the kernel and prints the seat-failed line itself, leaving Lictor's phase at 1 until the next STATUS, which is the state the lazy expiry leaves. Mutant RV80g restores that wrapper and must go red at leg K, as RV80f does. F2: round 5's record said three code docs carried the phrase "dies at the next pump". They did not: their error was the bound's lifetime (`takeover_from` "while a trusted takeover waits", `bound_takeover` "None once it is up or gone", and `ControlThenCursor` silent on the late shot). Only TRUSTED-PATH and sub-lictor carried the phrase.
