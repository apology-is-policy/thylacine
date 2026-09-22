---
id: adt-b0poll-r6
type: adt
title: "Poll B-0 round 6: clean on the round, and the finding that mattered was self-found -- a per-thread sleep bound does not bound a CPU"
date: 2026-09-22
scope: [sub-kernel-poll, sub-kernel-sched]
reviewer: opus
model-start: "claude-opus-5"
model-end: "claude-opus-5"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 2}
findings: [fnd-b0poll-r6-s1]
round-of: chg-2026-09-22-poll-preemption-point
prior-round: adt-b0poll-r5
created: 2026-09-22
---
## Scope

Branch `browser-b0`: round 5's spin budget and backoff, the loop around them,
and the death/stop checks. FALLBACK tier (Fable unavailable).

## Convergence

The prosecutor returned 0/0/0/2 P3. The round's real finding was SELF-found
during the parallel self-audit and carries the round's number: the backstop
keys on the THREAD while the obligation belongs to the CPU
([[fnd-b0poll-r6-s1]]). It is recorded at P1 severity despite the round's
clean verdict, because it invalidates round 5's fix rather than qualifying it.
The two P3s were wording. The finding was not reproduced on a running system;
it was derived from the code and then checked FORMALLY by a new model rather
than by an attack, which is the honest provenance and is stated in ARCH 23.3.
