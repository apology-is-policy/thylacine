---
id: fnd-c9-r1-f1
type: fnd
round: adt-c9-r1
severity: P3
status: fixed
title: "The decision record reads the fp-pair figure as V state's cost, and a third of it is an ISB"
surface: [sub-kernel-sched-smp]
threatens: []
fixed-by: chg-2026-11-04-c9-r1
regression: "none: a figure"
created: 2026-11-04
---
## Prosecution

`dec-2026-11-04-switch-from-ttbr0` quotes 50 ns as an FP save and restore and derives 25 ns per load for the lazy restore it does not build. The instrument's piece is `fp_save_area` then `fp_restore_area`, which ends in an ISB. With the ISB at 17 ns, V state is about 33 ns and one load about 16, and the MSR that an equal value skips is about 7 (24 less 17).

## Disposition

`chg-2026-11-04-c9-r1` carries the figures, since a decision's body is append-only, and the instrument says so where the piece is defined. The decision stands: its correctness argument did not depend on the size.
