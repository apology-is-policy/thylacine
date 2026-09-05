---
id: fnd-33-r1-f2
type: fnd
round: adt-33-r1
severity: P3
status: fixed
title: "Three imprecise claims in the yield fast path's own justification"
surface: [sub-kernel-sched]
threatens: []
fixed-by: chg-2026-07-05-33-sys-yield
regression: "none -- comment corrections"
created: 2026-08-01
---
## Prosecution

Documentation, but each one would have licensed a wrong change:

1. **The peek's CPU-identity TOCTOU** was described as a live hazard. It
   is hypothetical-**future-caller**-only: syscalls run IRQ-masked end to
   end, so no preempt can land inside the peek from the SVC path, and the
   test callers run on the `cpu_pinned` kthread. Overstating it invites
   a "fix" — masking the peek — that costs something for nothing.
2. **The placement-kick argument** was stated as "the flag implies the
   peek sees the head". That is wrong under weak ordering: the flag can
   be visible while the head store is not. The correct argument is
   *consumer-under-the-lock* — `preempt_check_irq -> sched()` acquires
   `cs->lock`, which pairs with the placer's release.
3. **"served at the next IRQ"** — a fast-path yield with a pending flag
   is served at **this very syscall's return tail**, because
   `preempt_check_irq` runs on the EL0 sync-return path.

## Why a comment is worth a finding

Each of these is a claim a future reader would reason *from*. Number 2 is
the sharpest: an argument that happens to reach the right conclusion by a
wrong route is worse than no argument, because the next person extends
the wrong route to a case where it does not hold.

The pattern recurs in this arc's round 2 ([[fnd-33-r2-f1]]), where a
comment misattributing the [[inv-i9]] guarantee could have invited an
optimization that reintroduced the exact bug the chunk had just closed.
