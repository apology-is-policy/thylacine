---
id: chg-2026-11-09-takeover-bound-round5
type: chg
title: "The takeover's bound lives across one reap (round 5)"
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
Round 5 over [[chg-2026-11-09-takeover-bound-round4]] (Opus 5.5 on Opus 5.5, read-only; 0 P0, 0 P1, 0 P2, 3 P3). F1: leg K's late shot reaches FAILED through Lictor's own `fail()`, so a narrower wrong fix (clear the bound only once Lictor's phase has moved) stayed green while the kernel's unadopted expiry still reached the serve loop's pump with the bound set. The bound is now set only across the phase-1 reap, the one pump after a STATUS that read QUIESCING, and cleared right after it; round 4's end-of-step clear and the clear at activation are gone, and no premise about paint or activation settling a stall remains. A clear placed after the reap runs before the late shot's `fail()`, so a clear keyed to Lictor's phase never runs there and the late shot catches it; mutant RV80f now removes the post-reap clear. The trigger row now names the wrong fix. F2: TRUSTED-PATH, sub-lictor and three code docs said a takeover's command stalling after 33 s "dies at the next pump"; it dies at the next seat step's reap if the chord is still live, otherwise the restore waits on it (this corrects round 4's record, which said the same). F3: leg K's third block never reset `renewed`, so its "no renewal behind the stalled lane" check could not fail; both legs' second waits now count their own renewals, leg L checks for one too, and the scripted guest has a fault row for each.
