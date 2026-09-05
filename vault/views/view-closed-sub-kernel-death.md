---
id: view-closed-sub-kernel-death
type: view
title: "Do-not-re-report preamble — sub-kernel-death"
query: closed:sub-kernel-death
---
# Do-not-re-report preamble — sub-kernel-death

Generated from `fnd-*` notes (`quaestor render`; also emitted
on-demand by `quaestor closed sub-kernel-death`). Paste or
transclude into a prosecutor prompt as the closed-findings preamble.

Read this WITH the verified-sound sets in [[adt-811-r1]] and
[[adt-68-r3]] — on this surface the traced-and-survived list is as
load-bearing as the findings, because it is what stops a round
re-deriving the I-9 close from scratch.

<!-- generated:begin -->
13 closed findings on [[sub-kernel-death]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-68-r1-f1]] [P1] A clean exit_group(0) makes the closer read as dying, silently dropping its staged writes (fixed) — FIXED: a per-Thread `bool exit_close_active`, checked FIRST in
- [[fnd-68-r1-f2]] [P2] The close-time Tclunk is never sent — a server-side fid leak per fd on every Go exit (fixed) — FIXED by the same `exit_close_active` flag. Recorded separately because the
- [[fnd-68-r2-f1]] [P1] The exits() close site is ALSO reachable with the death machinery armed (fixed) — FIXED by HOISTING the flag's set/clear INSIDE `proc_close_handles_at_exit`,
- [[fnd-68-r2-f2]] [P2] thread_count is not a live-thread count, so the gate skipped joined-then-exits Procs (fixed) — FIXED: the top-of-`exits()` gate DELETED, replaced by a `live_peers`-gated
- [[fnd-68-r2-f3]] [P3] The re-admitted wedged-server strand is not breakable by a further kill (documented) — DOCUMENTED, not fixed. The precondition is a wedged TRUSTED server — an
- [[fnd-68-r3-f1]] [P3] handle.c's lockless-safety justification went stale under the fix that needed it (fixed) — FIXED: reworded to the live_peers formulation — `thread_count` may exceed 1;
- [[fnd-68-r3-f2]] [P3] The killed-child regression's 'parked' wait was vacuous (fixed) — FIXED as an honesty change rather than a behavioural one: the test still
- [[fnd-811-r1-f1]] [P3] The group-terminate smoke test violated the lock contract #811 itself introduced (fixed) — FIXED: the two real-Proc calls wrapped in
- [[fnd-811-r1-f2]] [P3] A magic literal 2 for PROC_STATE_ZOMBIE in the death-interrupt test (fixed) — FIXED to the named constant. Noted at the time and NOT swept: `test_torpor`'s
- [[fnd-811-r1-f3]] [P3] torpor absorbed TSLEEP_INTR implicitly, via 'not TIMEDOUT means OK' (fixed) — FIXED with an explicit comment at the `return TORPOR_OK` documenting the
- [[fnd-926-r1-f1]] [P3] handle.c's lockless justification named a premise the new ALIVE-Proc call site broke (fixed) — FIXED: comment updated, plus a FOOTGUN flag for a future cross-Proc
- [[fnd-926-r1-f2]] [P3] A KILLED single-thread Proc still deferred its fd close to reap (fixed) — Documented at the time as a v1.x EXITING-protocol-restructure item — and
- [[fnd-926-r1-f3]] [P3] $errstr goes stale on a substitution spawn failure (documented) — CLOSED WITH JUSTIFICATION, no code change: the failure remains observable
<!-- generated:end -->
