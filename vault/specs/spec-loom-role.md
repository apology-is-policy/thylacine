---
id: spec-loom-role
type: spec
title: "loom_role.tla"
models: [sub-kernel-loom, sub-kernel-ninep-client]
pins: [inv-i9, inv-i29]
cfgs:
  - "loom_role.cfg -- clean: the seven-conjunct safety set at two sync ops, one stop each"
  - "loom_role_liveness.cfg -- EnterReturns: the ENTER returns, or ends in the (E) blind recv for good"
  - "loom_role_wide.cfg -- the same at three sync ops: a two-link designation chain"
  - "loom_role_buggy_no_role_hook.cfg -- BUGGY_NO_ROLE_HOOK: the pre-fix ENTER sleeps on the CQ list only (EnterReturns)"
  - "loom_role_buggy_late_register.cfg -- BUGGY_ROLE_LATE_REGISTER: the hook trusts the pump's stale sample (NoMissedRoleWake)"
  - "loom_role_buggy_no_role_wake.cfg -- BUGGY_NO_ROLE_WAKE: the no-designee exit wakes nobody (NoMissedRoleWake)"
  - "loom_role_buggy_designates_parked.cfg -- BUGGY_DESIGNATES_PARKED: the handoff designates a stop-parked thread (NoMissedRoleWake)"
  - "loom_role_buggy_stop_keeps_designation.cfg -- BUGGY_STOP_KEEPS_DESIGNATION: a stopped designee parks without handing on (NoMissedRoleWake)"
  - "loom_role_residual_blind.cfg -- EXPECTED violation of NoBlindRecv: the OPEN-BUGS (E) blind recv is reachable"
gate: "any change to the reader election, the handoff, client_debug_stop_park, the role-waiter list, or loom_wait_for_completions' pump and sleep"
created: 2026-10-05
updated: 2026-10-05
---
## Abstraction

A Loom ENTER waits for the completion of an async operation on a shared 9P
client. Only the thread holding the client's reader role reads replies, and the
role can belong to another process's synchronous call, which hands it on only
to a synchronous waiter. The model is that wait: the role, its handoff with
both stop rules, the ENTER's two hooks (the ring's completion list and the
client's role-waiter list), and the wakes that reach them.

It is a sibling module, not an extension of the base Loom model, because the
base model assumes a reader exists. Its reply step is fair for any operation in
flight, which presumes some thread reads the reply. This module discharges that
premise for the ENTER's own pump. It is also the first model of the handoff
itself, so it carries the two stop rules the ENTER's wake depends on: the
handoff skips a thread parked for a stop, and a stopped designee hands the role
on before it parks.

## What it pins

- **`NoMissedRoleWake`**: the ENTER never sleeps while the role is free and no
  synchronous call is designated to take it, since nobody else would read its
  reply. It is stated without reference to the hook, so the pre-fix ENTER
  violates it too, and four buggy configurations each break it by a different
  path.
- **`BlindImpliesCq`**: the one carve-out covers only an ENTER whose completion
  is already posted. That pins the exception to the tracked residual (E): another
  reader posted the completion after the ENTER sampled, and the ENTER then took
  the free role and blocked in a receive with nothing due.
- **`EnterReturns`**: the ENTER returns, or ends in that blind receive for good.
  A blind state passed through on the way to some other strand does not satisfy
  it. The pre-fix ENTER fails it.

## What it cannot see

One client per ring. The ENTER drives the client of the ring's first in-flight
operation, and the model's one async operation lives on that client. A ring
whose operations span clients is a separate, pre-existing strand (OPEN-BUGS
2026-10-05 07:52Z).

The Proc's stop flags. The model has a parked phase but no flags, so a resume
followed by a re-stop that flips them while the thread never runs is not
representable. Code witnesses cover that class.

The server's good faith. Replies are fair, so every request is answered. A
server that defers a reply indefinitely, such as a parked socket read, breaks
that premise by design, and the liveness claim does not extend to it.

Also out of scope: session death (its own sibling module), the SQPOLL kthread,
a stop of the ENTER's own thread, the flood budget, and a second ENTER, which
has its own hooks on the same lists.

## Binding

`specs/SPEC-TO-CODE.md::loom_role.tla`. The handoff maps to the client's
handoff under the client lock. The role hook maps to the role-waiter register,
which re-samples the role under that lock. The ENTER's sleep maps to the
two-flag condition on its one Rendez.
