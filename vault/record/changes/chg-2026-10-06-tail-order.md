---
id: chg-2026-10-06-tail-order
type: chg
title: "Tail order: the EL0-return tail stops before it delivers notes"
date: 2026-10-06
arc: arc-go-ide
commits: ["bbc7ab90"]
touched:
  - sub-kernel-exception
  - moc-kernel-entry
  - sub-kernel-notes
  - sub-kernel-birth-hold
  - sub-kernel-death
  - sub-kernel-jobctl
  - sub-kernel-devproc
  - sub-kernel-rendez
  - sub-kernel-hwdebug
  - seam-el0-irq-tail-no-notes
  - spec-debug-stop
  - spec-debug-step
  - spec-tail-order
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-06
---
The operator voted on 2026-10-05 that the EL0-return tail stops before it
delivers notes, as Plan 9's `notify` runs `procctl` first and Linux's
signal-delivery-stop precedes the handler frame. The synchronous and birth
tails now run the die-check, the stop leg and then the notes leg
([[sub-kernel-exception]], [[moc-kernel-entry]], [[sub-kernel-birth-hold]]),
so a note posted during a stop is taken as the stop clears
([[sub-kernel-death]], [[sub-kernel-jobctl]]); the IRQ tail still delivers
none ([[seam-el0-irq-tail-no-notes]]). The order made three things necessary.
A stop the notes leg applies itself -- an uncaught `tty:susp` -- has no later
stop check, so the leg runs the die and stop checks again, parks and passes
over the queue afresh, inside one budget shared with the discard loop
([[sub-kernel-notes]], [[sub-kernel-rendez]]). A step-resume can now meet a
note with a handler, so both frame builders clear `SPSR.SS` before saving the
context and the step reports at the handler's entry, Linux's rule
([[sub-kernel-hwdebug]], [[spec-debug-step]]). And only a re-stop completes a
step: a step whose target died or whose slot was released fails with
`T_E_SRCH` ([[sub-kernel-devproc]]). `tail_order.tla` checks the order, the
re-pass and the budget with five buggy configurations ([[spec-tail-order]]);
[[spec-debug-stop]] names the device witnesses. Audit round 1, Fable 5.1 on
Opus 5.5, found nothing above P3; five of its six P3s were fixed and the
sixth, a witness for the Linux-phenotype builder's SS clear, is tracked.
Verified: `tools/test.sh` 1865/1865 with debug-probe's resume, death-step and
caught-step legs; six sabotages, each red where designed;
`specs/check-tail-order.sh` as claimed.
