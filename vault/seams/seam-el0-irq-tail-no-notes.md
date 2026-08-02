---
id: seam-el0-irq-tail-no-notes
type: seam
title: "Notes are evaluated on only one of the two EL0 return tails, so Ctrl-C misses a CPU-bound loop"
status: open
surface: [sub-kernel-exception]
opened-by: chg-2026-08-02-entry-sweep
tracker: "task #21"
created: 2026-08-02
updated: 2026-08-02
---
## Owed

Give the EL0 interrupt slot its own return trampoline — exactly as the EL0
synchronous slot already has — and put note delivery in it. Then reconcile the
comment that already claims this is the case.

## The gap

`notes_deliver_at_el0_return` has **one** call site: the EL0 *synchronous*
return trampoline, reached from the syscall-and-fault vector slot. The EL0
*interrupt* slot does not call it.

So a Proc's note disposition is evaluated only when it returns from a syscall
or a fault. A thread that makes neither — a compute loop — takes timer
interrupts forever without the question ever being asked.

Three of the four EL0-return actions run on both tails. Only this one does not:

| Action | sync tail | IRQ tail |
|---|---|---|
| preemption check | yes | yes |
| group-terminate die-check | yes | yes |
| **note delivery / default-terminate** | **yes** | **no** |
| debugger and job stop-check | yes | yes |

## What still works, and what does not

The three actions that *are* on both tails are the reason this is narrow rather
than alarming. Against a syscall-free compute loop:

- **an outright kill works** — it sets the group-exit state, which the die-check
  reads on the interrupt tail
- **Ctrl-Z works** — job stop is applied at post time, and the stop-check reads
  it on the interrupt tail
- **a debugger stop works** — same path

What does not work is anything routed through the note queue:

- **Ctrl-C does not terminate it.** The default-terminate decision lives inside
  the delivery function, so it is never evaluated.
- **A registered handler never runs**, for the same reason.

The tree states this asymmetry itself, in the comment explaining why the stop's
catchability gate is evaluated at post time: *"the stop — unlike the terminate —
is applied post-side, not at the tail."* The consequence of the terminate being
at the tail was not followed through to the tail that lacks it.

## Why it is not caught

The wake machinery for a terminate-disposition note walks a Proc's threads and
wakes the *blocked* ones, so that they unwind to their return tail and die
there. A running thread needs no waking — and so is never touched. The
mechanism is complete for blocked threads and empty for running ones.

## The scripture says otherwise

Both the architecture document and the life-support document state the composed
property as:

> **Ctrl-C terminates any foreground command — CPU-bound, output-bound, or
> blocked in sleep / read — catchably.**

Output-bound and blocked are covered: both are sleeps, so the terminate-wake
reaches them. **CPU-bound has no mechanism.** The claim is composed of three
parts and the third part is the one with the gap.

## Why the fix is not a one-liner

The EL0 interrupt slot holds **thirty-one of its thirty-two** instructions.
Note delivery needs two more. It does not fit.

The synchronous slot solved exactly this by branching to its own tail
trampoline, which is why it has five instructions to spare while doing strictly
more work. The interrupt slot inlined its whole tail instead and has run out of
room. So the fix is: factor the tail out first, then add the call.

## Reachability

Bounded today. Anything built on the standard C library or the Go runtime
syscalls constantly — a timer, a futex, a write — and reaches the synchronous
tail within microseconds. It takes a deliberately syscall-free loop to hold the
gap open.

But that is precisely the program Ctrl-C exists for, and the tree has been here
before: a CPU-bound EL0 thread on a secondary CPU was once invisible to the
kernel entirely, because the per-CPU timer was never armed. That fix is what
made the interrupt tail reachable for a spinning thread in the first place. The
note hook was never added to it.
