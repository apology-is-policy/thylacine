---
id: adt-b0poll-r5
type: adt
title: "Poll B-0 round 5: the re-arm loop holds its CPU's interrupts under unprivileged noise"
date: 2026-09-21
scope: [sub-kernel-poll, sub-kernel-sched]
reviewer: opus
model-start: "claude-opus-5"
model-end: "claude-opus-5"
verdict: dirty
counts: {p0: 0, p1: 1, p2: 0, p3: 5}
findings: [fnd-b0poll-r5-f1]
round-of: chg-2026-09-22-poll-preemption-point
prior-round: adt-pouchb0-r4
created: 2026-09-22
---
## Scope

Branch `browser-b0`: the poll re-arm loop's death/stop fixes from round 4, and
the loop's behaviour under a producer that flags on every re-sample. Same
FALLBACK tier as round 4 (Fable unavailable; the same-family preamble applied).

## Convergence

Dirty on one P1: round 4's parting note -- that syscalls run IRQ-masked end to
end, so a noise-driven `poll(-1)` spins a CPU with interrupts off -- made
concrete and shown to need no privilege ([[fnd-b0poll-r5-f1]]). Fixed at the
time with a per-thread spin budget and a 1 ms backoff, which round 6 then
showed bounds the wrong thing. The P3s were perf residue and contract wording,
carried forward with round 4's F8.
