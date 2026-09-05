---
id: fnd-33-r1-f1
type: fnd
round: adt-33-r1
severity: P2
status: fixed
title: "#363: a CPU parked up to the tickless backstop over its own just-requeued thread"
surface: [sub-kernel-sched-smp]
threatens: [inv-i8, inv-i17]
fixed-by: chg-2026-07-05-33-sys-yield
regression: "none deterministic -- sched_tickless.tla has no self-requeue action, so the model cannot express it either; the witness is the work-conservation telemetry"
seam: seam-eevdf-math
created: 2026-08-01
---
## Prosecution

`sched()` picks `next` **before** it requeues `prev`.

So: a slice-expiry preempt (or a yield) of a thread whose local queue is
otherwise empty finds only the pinned in-tree idle to pick, dispatches
it, and *then* puts the preempted thread into `run_tree[NORMAL]`.

The dispatched idle does not restart its loop. It resumes **inside**
`sched_idle_park`, at the point past its own `sched()` call, headed
directly for the one-shot arm and the WFI — with no re-check of the local
queue. The CPU then parks for up to `TICKLESS_IDLE_BACKSTOP_NS` **over
its own RUNNABLE thread**.

Nothing rescues it: there is no IPI for a *local* self-requeue (the
notify paths target peers), and the one-shot arm has already deasserted
the periodic tick that would otherwise have re-polled within a
millisecond.

Cost: up to ~4 ms lost per 6 ms slice for a solo compute-bound thread.

## Fix

After `sched()` returns in `sched_idle_park`, loop
`while (cpu_has_surplus_for_kick(cs)) sched();` before committing to the
park — the #33 yield predicate (two relaxed head loads, deref-free)
applied at the park commit instead of at the syscall.

The deferred park is a **stutter** on [[spec-sched-tickless]]'s Park
action: `NoLostWake` and `ParkedImpliesRegistered` are untouched, which
is why the fix needed no model change — and also why the model would
never have caught the bug.

## What it corrected besides the code

- **A misread instrument.** The TI-4d multi-millisecond starved-park
  records — including a 103 ms maximum — had been attributed to peer
  backlog. They were this. The witness is the collapse: 9642 ms → 3404 ms
  of tickless starvation per boot, −65%.
- **A blind benchmark.** The `scale` bench measures a self-ratio, so a
  bug that slows every configuration equally is invisible to it.
- **A false premise in the new code's own justification.** #33's
  "the idle immediately switches back" benignity model rested on exactly
  the assumption the bug lived in. Four documentation sites were
  corrected.

Pre-existing — not introduced by #33. The yield syscall is what made
anyone look.
