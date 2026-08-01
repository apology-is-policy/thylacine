---
id: adt-33-r1
type: adt
title: "#33 SYS_YIELD, round 1 — and #363 underneath it"
date: 2026-07-05
scope: [sub-kernel-sched, sub-kernel-sched-smp]
reviewer: fable
model-start: "fable-5"
model-end: "fable-5"
verdict: dirty
counts: {p0: 0, p1: 0, p2: 1, p3: 2}
findings: [fnd-33-r1-f1, fnd-33-r1-f2]
round-of: chg-2026-07-05-33-sys-yield
created: 2026-08-01
---
## Scope

The `SYS_YIELD` mechanism: the syscall, `sched_yield_hint`'s lock-free
fast path, the ABI across all five consumers, and the two new tests.
Fable-5 focused audit plus a concurrent self-audit.

## Verdict

**0 P0 / 0 P1 / 1 P2 / 2 P3** — and **dirty**, despite the counts. Not
because of the arithmetic (P1+P2 = 1) but because the P2's fix
**restructured the park-commit logic**, which is a wait/park protocol
change on the most bug-prone lineage in the tree. A round 2 ran on the
fix: [[adt-363-r2]].

## What audited sound

The yield mechanism itself. No I-8/I-9/I-17/I-18/I-21/I-24 or #360
violation; the ABI consistent across all five consumers (the Go runtime's
`osyield`, musl's `sched_yield` through the pouch seam, and the two
native wrappers); both tests non-vacuous in both directions.

## What it found instead

[[fnd-33-r1-f1]] — a **pre-existing** bug in `sched_idle_park`, not
introduced by #33: a CPU parking up to the tickless backstop over its
own just-requeued RUNNABLE thread (#363).

The finding's reach is what makes it notable. It did not just fix a
latency bug — it **corrected an interpretation**. The TI-4d multi-ms
starved-park records, including a 103 ms maximum, had been attributed to
peer backlog. They were this. And #33's own benignity model ("the idle
immediately switches back") rested on the same false premise the bug
lived in, so four documentation sites were corrected alongside the code.

[[fnd-33-r1-f2]] (P3) — three doc-precision corrections, including that
the peek's CPU-identity TOCTOU is hypothetical-future-caller-only, and
that a fast-path yield with a pending flag is served at *this* syscall's
return tail rather than at the next IRQ.

## Measured

The fix's witness: boot-wc tickless starved 9642 ms → 3404 ms per boot
(−65%) at an unchanged boot shape, 1026/1026 pass, boot OK, zero
extinctions. The residual is the genuine peer-backlog signal the counter
was built for.
