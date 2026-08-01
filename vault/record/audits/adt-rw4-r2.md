---
id: adt-rw4-r2
type: adt
title: "HOLOTYPE RW-4 round 2 — the dirty-close re-prosecution of the fixes"
date: 2026-06-10
scope: [sub-kernel-stalk, sub-kernel-srvconn, sub-kernel-ninep-session]
reviewer: fable
model-start: "claude-fable-5"
model-end: "claude-fable-5"
verdict: clean
counts: {p0: 0, p1: 0, p2: 2, p3: 2}
findings: [fnd-rw4-rb-f1]
round-of: chg-2026-06-10-rw4-fixes
prior-round: adt-rw4-r1
created: 2026-08-01
---
## Scope

The round-1 fixes lifted a lock-order rule (`ns_lock`) and changed
three wait/wake paths → two Fable reviewers re-prosecuted the FIXES
(R-A the ns_lock; R-B the wait/wake trio) + an Opus round-2 self-audit.
CONVERGED CLEAN over the two rounds.

## Convergence

R-A verified the SA-F1 fix path-by-path (deferred-clunk total, every
goto sets rc, the six FROM_ROOT sites balance, two-thread interleavings
replayed) — its 1 P2 + 2 P3 were all doc/hygiene (the reference docs
taught the PRE-fix borrow contract; a stale comment; joey's bare
`root_spoor` read routed through `territory_root_ref`), fixed in place
and kept in this body per the doc-only bar. R-B confirmed the busy-guard
and SQPOLL-guard interleavings sound and CAUGHT THE REAL ONE
([[fnd-rw4-rb-f1]]): round-1's R3-F1 fail-closed latch was OVER-BROAD —
its "dispatch failure == protocol violation" premise is false for the
Twalk fid_bind-full leg (a LOCAL 256-fid exhaustion), so the fix would
have latched the whole shared root-FS session dead on the 257th
concurrent fid — WORSE than the leak it closed. The recorded LESSON:
latching a SHARED resource dead on a LOCAL/per-op condition is the
recurrent "fix more severe than the bug" dirty-close hazard —
distinguish protocol-violation (latch) from local-resource (per-op
error) at the classification site; never let a -1 token conflate them.
The R-B-F1 fix (classification refinement only — no new lock/wake) owed
no round 3. Posture: 814/814 + boot OK + the SMP gate 0 corruption,
run before AND after the R-B-F1 fix.
