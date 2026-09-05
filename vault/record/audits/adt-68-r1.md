---
id: adt-68-r1
type: adt
title: "#68 last-thread-out fd close, round 1"
date: 2026-07-14
scope: [sub-kernel-death]
reviewer: fable
model-start: "fable-5-max"
model-end: "fable-5-max"
verdict: dirty
counts: {p0: 0, p1: 1, p2: 1, p3: 2}
findings: [fnd-68-r1-f1, fnd-68-r1-f2]
round-of: chg-2026-07-14-68-last-thread-out-close
created: 2026-08-01
---
## Scope

The new close-at-exit window in `thread_exit_self` + `proc_close_handles_at_exit`,
against the death-path lineage (#788/#806/#860/#809/#811/#926).

## Convergence

`model-start == model-end` — no mid-run fallback. DIRTY: a P1 landed, and its
fix (a new per-Thread flag consulted by the death predicate) changes when the
death machinery is considered armed — squarely the dirty-class criterion, so a
round 2 was mandatory. Both findings share one root: the author's premise that
`group_exit_msg` set means the Proc was KILLED. It does not.
