---
id: spec-tail-order
type: spec
title: "tail_order.tla"
models: [sub-kernel-exception, sub-kernel-notes, sub-kernel-birth-hold]
pins: [inv-i39, inv-i19]
cfgs:
  - "tail_order.cfg -- clean, the EL0-return tail entered by a syscall or an IRQ: TypeOK + MeetsQueue + NoEretUnderOwnStop + TailEnds + RunMeets (46292 distinct with -workers 1, DEPTH 3)"
  - "tail_order_birth.cfg -- clean, the birth tail: the same (68418)"
  - "tail_order_buggy_notes_first.cfg -- the notes leg before the stop leg, the order before the vote: MeetsQueue (5622)"
  - "tail_order_buggy_birth_notes_first.cfg -- the same on the birth tail: MeetsQueue (3390)"
  - "tail_order_buggy_no_repass.cfg -- the stop arm reports no stop, so nothing parks the thread: NoEretUnderOwnStop (870)"
  - "tail_order_buggy_budget_first.cfg -- the budget break before the re-pass's die-check and stop leg: NoEretUnderOwnStop (8167)"
  - "tail_order_buggy_no_budget.cfg -- neither the passes nor the discards count: TailEnds, the invariants hold (18044)"
  - "tail_order_buggy_irq_no_notes.cfg -- the IRQ tail ends at the stop leg, as it did before #964: RunMeets (26886)"
gate: "any change to the tails' leg order (.Lel0_return and either vector slot that enters it, userland_enter_held), to notes_deliver_at_el0_return's loop, to what notes_deliver_tail returns or spends, or to NOTE_QUEUE_DEPTH as the tail's budget; specs/check-tail-order.sh"
created: 2026-10-06
updated: 2026-10-12
---
## Abstraction

A companion to [[spec-debug-stop]], which verifies the park and has no note
delivery. This model takes the park as given and checks *where the tails call
it*. One thread returns to EL0 through the die-check, the stop leg, the notes
leg and the `eret`, while an environment posts notes, stops and resumes it for
the debugger and for job control, kills its group and, on the birth tail,
releases its hold. The park is one step: death if the group is dying, hold
while a stop owner or the birth hold is set, proceed otherwise.

The operator voted on 2026-10-05 that the tail stops before it delivers notes,
as Plan 9's `notify` runs `procctl` before it looks at a note and as Linux's
signal-delivery-stop precedes the handler frame (DEBUG-FS-DESIGN 4.2). That
order makes one new mechanism necessary. A stop the notes leg applies itself,
the default action of an uncaught `tty:susp`, now has no stop check after it,
so the leg runs the die-check and the stop leg again, parks, and passes over
the queue afresh, inside one budget it shares with the discard loop. The audit
of that chunk found the re-pass outside every model (round 1, F6); this model
is the answer.

The queue holds four kinds of note, by what the leg does with each: `plain`
is discarded and the leg loops, `caught` builds a handler frame and ends the
leg, `term` ends the thread, and `susp` applies a stop and asks for a re-pass.

Since 2026-10-12 (#964) the IRQ-from-EL0 vector branches into the same tail
the synchronous vector takes, so a thread that computes at EL0 and never
enters the kernel on its own still meets its queue: `Enter` is the synchronous
entry, which a compute loop never makes, and `IrqEnter` is the tick's, which
comes however the thread runs (weak fairness on the thread's steps). Both run
the same legs. Which thread of a Proc takes a caught note is
[[spec-caught-target]]'s.

## What it checks

| Property | Obligation |
|---|---|
| `MeetsQueue` | a thread reaches the `eret` only with its queue empty, a handler frame built, or the budget spent: a note posted while the thread is stopped is met as the stop ends, not at some later checkpoint |
| `NoEretUnderOwnStop` | a thread never reaches the `eret` while a stop it applied itself is still in force |
| `TailEnds` | every tail leaves its legs, into EL0, a park or death; a flooded queue cannot hold the masked tail |
| `RunMeets` | a thread at EL0 with a note queued reaches the notes leg, a park or death, even one that never makes a syscall: the IRQ tail meets the queue |

`TailEnds` holds under weak fairness of the thread's own steps, so the
environment cannot starve it by never letting it move.

## The counterexamples

Each buggy configuration breaks the property named for it, and each trace was
read rather than counted.

- **notes first**, on either tail, is the order before the vote. The thread
  delivers, then parks for a stop; a note is posted during the park; the stop
  clears and the thread erets with the note still queued. That is the defect
  the vote removed: under the old order a note posted during a stop waited for
  the next synchronous entry.
- **no re-pass** has the stop arm report nothing, so the leg ends after
  applying the stop and the thread erets under it -- the S3 sabotage's shape.
- **budget first** spends the last pass before it runs the die-check and the
  stop leg, so a stop applied on the final pass is never parked for.
- **no budget** never counts a discard or a pass. Every invariant holds, and
  the liveness property fails on a lasso: posts keep pace with the discards,
  and the thread never leaves its notes leg.
- **IRQ no notes** is the IRQ tail before #964: the die-check and the stop
  leg, then the `eret`. `RunMeets` fails on a lasso in which the thread runs
  at EL0 with a note queued and only ever takes the tick, which never reaches
  the notes leg. (`MeetsQueue` would also fire; the cfg checks the liveness
  form, which is the defect a spinner showed.)

The first notes-first trace was not that defect. It posted the note while the
thread was in its final stop leg, after the notes leg had run -- the masked
window between the last decision and the `eret`, which waits for the next
checkpoint on any order. The post window now excludes that leg, so the
counterexample shows the order and not the window. A buggy configuration that
fails for a reason other than its own is a configuration that proves nothing.

## Action-site map

| Action | Site |
|---|---|
| `Enter` / `IrqEnter` | `arch/arm64/vectors.S`: the synchronous and the IRQ-from-EL0 vector slots, each branching into `.Lel0_return` |
| `DieCheck` / `StopLeg` / `Park` | `.Lel0_return`: `el0_return_die_check`, `el0_return_stop_check`, then `notes_deliver_at_el0_return`; `userland_enter_held`: the die-check, `el0_birth_park`, then the notes leg |
| `NotesLeg` | `kernel/notes.c::notes_deliver_tail`: the discard loop, a frame build, a terminating default, and the stop arm (`notes_stop_dequeue_locked` -> `proc_job_stop_self`, which returns true, as the orphan rule's discard does) |
| `Repass` / `Proceed` | `notes_deliver_at_el0_return`'s loop: the die-check, the stop check, then `++passes` against `NOTE_QUEUE_DEPTH` |
| `Spend` | the one counter `notes_deliver_tail` and the loop share |
| the environment | `notes_post`; `proc_debug_stop_deliver` / `proc_debug_resume`; `proc_job_stop_self` / `proc_job_cont_proc`; `proc_group_terminate`; `proc_birth_hold_release_locked` |

## Deliberately beneath the model

- the park itself, which is [[spec-debug-stop]]'s;
- a note posted in the masked window after the leg's last decision, as above;
- notes left for a self-managing Proc's fd reader;
- registers. That each pass re-reads the handler, the stack pointer and the
  signal table is the kernel tests' to show, and the audit withdrew the stale
  read on reading the code (round 1, W3).

## Holding the code to it

`rendez.tail_parks_for_the_stop_it_applies` runs the real notes leg on a kernel
thread with a queued `tty:susp` and requires it to park on its own
`debug_rendez` until the stop lifts. On the device, debug-probe's `resume` leg
meets a note posted during a stop on the very resume, and its `caught-step`
leg's step reports at the handler's first instruction. Sabotages one variable
from the code turned each red: the old order (`resume`), a stop arm returning
false (the kernel test), and a frame build that keeps `SPSR.SS`
(`caught-step`). The IRQ tail's notes leg is held by viv-pheno-probe L332 (a
main spinning with no syscall takes a child's `SIGCHLD`) and jc-probe's spinint
and spinnote rungs (a native spinner ended by `^C` after it settles); reverting
the vector's branch turns each red. `specs/check-tail-order.sh` runs all eight
configurations and fails unless the clean ones reach their pinned counts and
each buggy one violates its named property.
