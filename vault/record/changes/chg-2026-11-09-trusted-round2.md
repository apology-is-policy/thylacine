---
id: chg-2026-11-09-trusted-round2
type: chg
title: "The trusted-path round 2: the cursor lane renews, renewal at first sight, the readback judge"
date: 2026-11-09
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-lictor
established:
  - dec-2026-11-09-cursor-lane-wait
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-09
---
Round 2 over [[chg-2026-11-09-trusted-audit-close]] (Opus 5.5 on Opus 5.5; 0 P0, 1 P1, 1 P2, 1 P3), merged with the implementer's own pass (2 P3). R2-F1: a cursor chain held by a client's readback kept the takeover out with nothing renewing, so the chord failed at five seconds; [[dec-2026-11-09-cursor-lane-wait]] (vote 79) renews while the lane's stall stands (`Gpu::takeover_waiting`) and ends the wait at the lane's abandonment. With it, a first renewal armed a second late, so a hold first seen after four seconds missed the deadline: `gpu_api::renew_due` renews at first sight (host test). Witnesses: `ls-graphical-sak-dead` legs C (a cursor chain held 8 s: the takeover completes) and X (held past the abandonment: the chord fails, the lane revives 10 s later and a pointer update goes through). R2-F2: warp-prove's C6-RB judge read the readback's standing time after the ten presents; it now samples the retirement after each present and judges from the first. R2-F3: the latch lever's release clears a chain's ownership only when its hold consumed the entry. Self-found: the staged-echo bound in two scripture rows (one take per hold since the mover stops), and a CR LF terminal's LF after the verdict (cosmetic; withdrawn). Clippy's `never_loop` in corvus's verb dispatch, May's code, is now an `if`.
