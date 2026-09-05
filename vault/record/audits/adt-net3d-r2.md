---
id: adt-net3d-r2
type: adt
title: "net-3d round 2: the fix prosecuted — converged clean"
date: 2026-06-17
scope: [sub-netd-server]
reviewer: opus
model-start: claude-opus-4-8
model-end: claude-opus-4-8
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 2}
findings: [fnd-net3d-r2-f1, fnd-net3d-r2-f2]
round-of: chg-2026-06-17-net3-server-side
prior-round: adt-net3d-r1
created: 2026-07-31
---
The dirty-close round-2 on the F1 fix (the gen guard, cancel-on-clunk,
the opened busy-mark, the loopback E2E). The fix is SOUND — key
confirmations: the strand survives NO path (clunk → cancel BEFORE
unref; Tversion/teardown → cancel-for-conn; walk-from/re-open blocked
by the busy-mark; the only rebind skipping cancel is complete_accept,
by which point the pending is drained); the guard's index access is
short-circuit-safe; N KEEPING its gen across the accept_swap re-arm is
REQUIRED (a naive re-arm bump would falsely drop a sibling pending on
the same N), and multiple pendings never double-accept one established
socket (the re-armed listener is in LISTEN, accept_ready false until a
genuine new SYN — verified in smoltcp); all six Slot literals are fully
field-specified (no `..` spread → no stale-defaulted field); all typed
gets remain proto-guarded; the loopback E2E is isolated, leak-free, and
bounded. Both P3s were comment-precision items, addressed in-round.
CONVERGED clean over 2 rounds.
