---
id: adt-68-r3
type: adt
title: "#68 last-thread-out fd close, round 3 — converged clean"
date: 2026-07-14
scope: [sub-kernel-death]
reviewer: fable
model-start: "fable-5-max"
model-end: "fable-5-max"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 2}
findings: [fnd-68-r3-f1, fnd-68-r3-f2]
round-of: chg-2026-07-14-68-last-thread-out-close
prior-round: adt-68-r2
created: 2026-08-01
---
## Scope

The round-2 fixes: the hoisted flag, the `live_peers` gate, and the
window's re-derived soundness argument.

## Convergence

CONVERGED CLEAN over three rounds. Both remaining P3s were documentation
that had gone stale UNDER the fix rather than defects in it — including the
lockless-safety justification in `kernel/handle.c`, the very file carrying
the #66c cross-Proc FOOTGUN warning, whose stated premise (`thread_count <= 1`)
the new gate had just invalidated.

The round's most useful output is its attack log, not its findings: double-close
mutual exclusion proved by contradiction; the recount extinction shown
unreachable from EL0; the window traced against a racing kill, a racing
`wait_pid`, orphan adoption, the LS-5 latch, the legate teardown, and the
srv/cap notifies; the flag shown to have no leak path and grep-complete
self-only callers. That set is the do-not-re-prosecute preamble.
