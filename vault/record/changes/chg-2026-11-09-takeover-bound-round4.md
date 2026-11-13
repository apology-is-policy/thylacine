---
id: chg-2026-11-09-takeover-bound-round4
type: chg
title: "The takeover's bound lives inside one seat step (round 4)"
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
Round 4 over [[chg-2026-11-09-takeover-bound]] (Opus 5.5 on Opus 5.5, read-only; 0 P0, 0 P1, 1 P2, 4 P3). F1 (P2): the serve loop pumped completions between seat steps with the bound the last step left. The kernel fails an expired chord lazily, at a call, so a stall begun after the kernel's deadline and before the next STATUS was killed with only the restore waiting on it, and the seat returned without its display. `Seat::step` now clears the bound on every exit, and the only pump that sees it runs right after the step's STATUS. The witness is leg K of `ls-graphical-sak-chained`: once the bound has abandoned the lane, at 34 s the lever withholds a control stall and fails the seat behind it, and that stall must survive to its release and the restore. F2: six comments named only the 30 s bound. F3: the docs said the kernel's limit fails the chord after the bound, but an abandoned chain that retires first still lets the takeover through. The cost also covers a takeover's own command stalling after 33 s, which dies at the next pump. [[dec-2026-11-09-takeover-bound-from-chord]] is a record and stays as written; this note carries the correction. F4: the phase term of `waiting_chord` is redundant once the bound lives in the step, and no witness constructs a stall in phase 1 after an activation past 33 s (that needs the chord held). F5: the host test also reads `SEAT_QUIESCE_NS` and checks that a renewal a pass late still lands inside it.
