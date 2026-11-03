---
id: fnd-thread-c7-r1-f3
type: fnd
round: adt-thread-c7-r1
severity: P3
status: fixed
title: "5h credited the step's fire and focus to a kernel test that cannot fire"
surface: [sub-kernel-devproc]
threatens: []
fixed-by: chg-2026-10-29-thread-model-c7
created: 2026-10-29
---
## Prosecution

The synthetic fixture parks no thread a resume can wake, so no kernel leg fires a step. DEBUG-FS-DESIGN 5h now credits the kernel test with the arm only and points the fire and its focus at debug_step.tla and the debug-probe threads leg.
