---
id: spec-net-poll-teardown
type: spec
title: "net_poll_teardown.tla"
models: [sub-kernel-ninep-dev9p-poll]
pins: [inv-i9]
cfgs:
  - "net_poll_teardown.cfg -- Fix=TRUE clean (SafetyInvariants incl. NoUseAfterFreePs: the kthread never touches a freed poll-state)"
  - "net_poll_teardown_liveness.cfg -- Fix=TRUE: the netd slot is freed (the ready-fd clunk delivered) with NO kthread-fairness assumption"
  - "net_poll_teardown_buggy_leak.cfg -- Fix=FALSE: liveness VIOLATED -- the #294 permanent-slot-leak counterexample (the op pins the Spoor, the clunk waits on the kthread GC, the GC never fires)"
gate: "Pre-commit re-run for ANY change to the readiness-op pin set or the dev9p_close/priv_release teardown ordering."
created: 2026-07-31
updated: 2026-07-31
---
## Abstraction

The #294 design verifier — modeled BECAUSE the bug was a Heisenbug
(in-guest instrumentation shifted the GC window and hid the leak), so the
model, not a boot, is the reliable witness. It models the op's pin set
(poll-state + session, NOT the Spoor), the close-grab vs kthread-collect
ownership race, and the clunk's delivery at UserClose. Pinned properties:
NoUseAfterFreePs (safety) + slot-freed (liveness). No dedicated §28
number — the module serves [[inv-i9]]'s relay end-to-end by guaranteeing
the teardown cannot strand it.

## Action-site map

Grab/cancel/free → `dev9p_poll_priv_release`; collect/reap →
`dev9p_poll_service_once` phase 1/2; the refcount → `dev9p_poll_state.refs`
(priv + per-op). **Below the abstraction and caught by the kernel TEST
instead**: the session-core clunk precondition (`any_outstanding_on_fid`
counting `awaiting_flush` — the #294 F-self-2 would-be-P1, fixed in
[[sub-kernel-ninep-session]]); the model specifies "the clunk IS delivered
at UserClose", the impl had to make the session honor it. The spec-first
lesson as recorded: model the design; a real-wire test catches the
impl-level bug beneath it.
