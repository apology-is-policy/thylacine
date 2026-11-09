---
id: chg-2026-11-09-trusted-round3
type: chg
title: "The trusted-path round 3: an abandoned lane renews nothing, leg X asserts the stop, an unjudged deadline is not a pass"
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
Round 3 over [[chg-2026-11-09-trusted-round2]] (Opus 5.5 on Opus 5.5; 0 P0, 0 P1, 2 P2, 6 P3). F1 (P2), open: the takeover's paint and activation wait for an idle cursor lane, so a cursor wait and then the control queue's run in series, each bounded 30 s from its own start, under one 35 s cap from the chord; a client that holds both can end a chord at the cap with the engine alive and no trusted screen. It goes to the operator as vote 80. F2 (P2): leg X counted renewals but never asserted that they stop at the abandonment; one alternation now fails a renewal after it. F8: `Gpu::takeover_waiting` renews for a stalled cursor lane, or for the control queue only with the lane idle, so an abandoned or failed lane renews nothing, and the seat logs once per chord that the lane holds the takeover (leg X requires the line). F3: legs 1 and C read the release and a renewal of one pass through one alternation. F4: warp-prove's C6-RB DEADLINE prints UNJUDGED and ends INCOMPLETE when the readback stood no longer than the bound, instead of PASS. F5: renewal at first sight rests on the host test alone (dossier). F6: the trusted-seat audit row prosecutes renewal. F7: stale prose. Self-found: a takeover that failed before it displaced anything left a held cursor chain held. warp-prove's chain-hold prints a TRACE line in readback mode, for the Pi's sak-chain red on this branch.
