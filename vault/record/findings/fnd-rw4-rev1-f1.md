---
id: fnd-rw4-rev1-f1
type: fnd
title: "RW-4 R1-F1: clone_walk_zero accepted a phantom-step 0-element walk"
round: adt-rw4-r1
severity: P3
status: fixed
surface: [sub-kernel-stalk]
threatens: []
fixed-by: chg-2026-06-10-rw4-fixes
created: 2026-08-01
---
## Prosecution

`clone_walk_zero` validated `w && w->spoor == q` but not `w->nqid == 0`
— a parity gap against the main loop's `nqid != 1` rigor. A Dev
returning the reused clone with a phantom step (`nqid != 0`) would hand
back a crossed root carrying a qid set during a step that never
happened.

## Disposition

Fixed: the guard tightened to `|| w->nqid != 0`, with the rationale
comment in place. Not live (all real Devs honor nname==0 → nqid==0) —
defense-in-depth parity, the same class as [[fnd-stalk1-r1-f2]].
