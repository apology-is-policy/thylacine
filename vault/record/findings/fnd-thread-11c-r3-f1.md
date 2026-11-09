---
id: fnd-thread-11c-r3-f1
type: fnd
round: adt-thread-11c-r3
severity: P3
status: fixed
title: "The walk hold's commit said a union skip sat below the flag, and one is only on neither list"
surface: [sub-kernel-stalk]
threatens: []
fixed-by: chg-2026-11-03-item-11c
regression: "none: text, corrected in 7fe98d3ef's body and the closed list"
created: 2026-11-03
---
## Prosecution

`stalk_union_child` runs under the hold. `stalk_union_member_holding` runs after `stalk_core` has given the flag back, and is safe only because rename and `viv_union_member` are on neither interruptible list. The same body said 26 sites where there are 22 RPC sites; the claim about each held. A committed body is frozen, so the correction is in the next commit. A later interruptible mutation that resolves a path would have to add the hold there.
