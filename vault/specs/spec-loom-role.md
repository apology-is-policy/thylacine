---
id: spec-loom-role
type: spec
title: "loom_role.tla"
models: [sub-kernel-loom, sub-kernel-ninep-client]
pins: [inv-i9, inv-i29]
cfgs:
  - "loom_role.cfg -- clean: the seven-conjunct safety set, one client, two sync ops, one stop each"
  - "loom_role_liveness.cfg -- EnterReturns: the ENTER returns"
  - "loom_role_wide.cfg -- the same at three sync ops: a two-link designation chain"
  - "loom_role_multi.cfg -- two clients, the first's async reply deferred forever: the ENTER returns through the second"
  - "loom_role_buggy_no_role_hook.cfg -- BUGGY_NO_ROLE_HOOK: a held role is not hooked, the pre-09-30 ENTER (EnterReturns)"
  - "loom_role_buggy_first_client_only.cfg -- BUGGY_FIRST_CLIENT_ONLY: the waiter sees only the first in-flight client, the pre-10-06 pick (EnterReturns)"
  - "loom_role_buggy_unready_pump.cfg -- BUGGY_UNREADY_PUMP: the pump takes a free role over an empty stream, the pre-10-06 pump (NoBlindRecv)"
  - "loom_role_buggy_late_register.cfg -- BUGGY_ROLE_LATE_REGISTER: a client goes on the role list without the role re-sampled (NoMissedWake)"
  - "loom_role_buggy_no_role_wake.cfg -- BUGGY_NO_ROLE_WAKE: the no-designee exit wakes nobody (NoMissedWake)"
  - "loom_role_buggy_designates_parked.cfg -- BUGGY_DESIGNATES_PARKED: the handoff designates a stop-parked thread (NoMissedWake)"
  - "loom_role_buggy_stop_keeps_designation.cfg -- BUGGY_STOP_KEEPS_DESIGNATION: a stopped designee parks without handing on (NoMissedWake)"
  - "loom_role_buggy_no_ready_hook.cfg -- BUGGY_NO_READY_HOOK: a free role with nothing to read is not hooked (NoMissedWake)"
  - "loom_role_buggy_ready_late_register.cfg -- BUGGY_READY_LATE_REGISTER: the readiness hook trusts the scan's stale sample (NoMissedWake)"
  - "loom_role_buggy_ready_hook_when_held.cfg -- BUGGY_READY_HOOK_WHEN_HELD: a held role hooks readiness and misses the holder leaving over a frame (NoMissedWake)"
gate: "specs/check-loom-role.sh, on any change to the reader election, the handoff, client_debug_stop_park, the role-waiter list, recv_ready, the pumps, or a fan-in waiter's scan, hooks and sleep"
created: 2026-10-05
updated: 2026-10-06
---
## Abstraction

A waiter for a Loom ring's completions reads for every 9P client the ring has
work on. Only the thread holding a client's reader role reads that client's
replies, and the role can belong to another process's synchronous call, which
hands it on only to a synchronous waiter. Nothing reads for an asynchronous
operation, so its reply is read by whoever holds the role when it arrives, or
by the waiter.

The model is that wait, over any number of clients. The waiter scans them and
reads from one whose role is free and whose stream holds a frame. With nothing
to read it hooks each one: a held role on the client's role-waiter list, a free
role on the transport's readiness list. Then it sleeps on all the hooks and the
ring's completion list. The model also carries the role's handoff with both
stop rules. The ENTER in the model stands for all three waiters that run this
fan-in: the ENTER, the SQPOLL kernel thread and the dev9p poll pump.

It is a sibling module, not an extension of the base Loom model, because the
base model assumes a reader exists: its reply step is fair for any operation
in flight. This module discharges that premise.

## What it pins

- **`NoMissedWake`**: the waiter never sleeps while some client has a frame
  waiting, a free role and no synchronous call designated to take it, since
  nobody else would read it. It is stated without reference to the hooks, so
  seven buggy configurations each break it by a different path.
- **`NoBlindRecv`**: the waiter takes a role only over a waiting frame, so it
  never blocks in a receive with nothing due. Until 2026-10-06 this was a
  tracked residual the liveness claim had to except.
- **`EnterReturns`**: the waiter returns whenever some client's reply comes,
  even while another client's reply is held forever. Both the pre-09-30 ENTER
  and the pre-10-06 first-client pick fail it.

## What it cannot see

The Proc's stop flags. The model has a parked phase but no flags, so a resume
followed by a re-stop that flips them while the thread never runs is not
representable. Code witnesses cover that class.

A frame that has only partly arrived. A ready transport is taken to hold a
whole frame; a reader that meets a partial one blocks through its body, which
rests on the trusted server, as every reader's frame does.

Also out of scope: session death (its own sibling module), a stop of the
waiter's own thread, the flood budget, a second waiter (its own hooks on the
same lists), and a user-space holder of a pipe transport's read end that
steals the waiting bytes, which desyncs its own mount anyway.

The ring's generation. A completion read by another thread posts its CQE
before it records a multishot re-arm or a chain gate, and the waiter sleeps
only while the ring's `drive_gen` has not moved since its loop top. The model
has no re-arm and no chain, so it cannot see that window; prose and the audit
carry it (LOOM.md 8.6, OPEN-BUGS 2026-10-06 16:20Z).

## Binding

`specs/SPEC-TO-CODE.md::loom_role.tla`. The handoff maps to the client's
handoff under the client lock. The scan maps to the readiness-gated pump, which
samples the role and the transport under that lock. The hook maps to the
client's hook call, which files the waiter on the role list or, through the
transport's readiness op, on the backend's list with the sample. The sleep maps
to the any-flag condition over all the hooks on one Rendez.
