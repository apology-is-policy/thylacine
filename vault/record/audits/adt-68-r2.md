---
id: adt-68-r2
type: adt
title: "#68 last-thread-out fd close, round 2 (on the round-1 fixes)"
date: 2026-07-14
scope: [sub-kernel-death]
reviewer: fable
model-start: "fable-5-max"
model-end: "fable-5-max"
verdict: dirty
counts: {p0: 0, p1: 1, p2: 1, p3: 2}
findings: [fnd-68-r2-f1, fnd-68-r2-f2, fnd-68-r2-f3]
round-of: chg-2026-07-14-68-last-thread-out-close
prior-round: adt-68-r1
created: 2026-08-01
---
## Scope

The round-1 fixes: the `exit_close_active` flag and its placement, plus the
gate that decides which thread performs the close.

## Convergence

`model-start == model-end`. DIRTY again, and instructively so: round 1's own
premise — "the `exits()` site is safe because `group_exit_msg` is unset
there" — was FALSIFIED in round 2. The LS-5 interrupt default-terminate path
calls `exits()` with the terminate latch deliberately still queued, and a
racing cross-Proc kill can set the flag mid-close. Two consecutive rounds,
two confident premises about when the death machinery is armed, both wrong.

The R2-F2 gate finding is the same shape one level down: `thread_count` was
being read as a live count and is not one.
