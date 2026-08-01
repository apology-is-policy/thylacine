---
id: spec-poll
type: spec
title: "poll.tla"
models: [sub-kernel-poll]
pins: [inv-i9]
cfgs:
  - "poll.cfg -- clean: NoMissedPoll + NoStaleHook, HAS_TIMEOUT"
  - "poll_notimeout.cfg -- poll(-1), the infinite wait; safety holds"
  - "poll_liveness.cfg -- Spec_Live: a poll call eventually returns"
  - "poll_buggy_check_before_register.cfg -- sample-then-register: a readiness edge in the gap reaches no hook (NoMissedPoll counterexample)"
  - "poll_buggy_no_wake.cfg -- producer sets the flag but never signals the Rendez (NoMissedPoll counterexample)"
  - "poll_buggy_lazy_unregister.cfg -- poll returns still-listed (NoStaleHook counterexample)"
gate: "any change to the register/sample atomicity, the sweep, or a producer wake site"
created: 2026-08-01
updated: 2026-08-01
---
## Abstraction

Two fds, one poller, flag-parameterized buggy variants. What neither
[[spec-scheduler]] (one Rendez) nor [[spec-tsleep]] (one deadline)
covers: ONE thread waiting on N readiness sources whose state lives
behind N DIFFERENT locks, with the `poll_waiter` flag as the
cross-lock handoff.

## What it pins

- **NoMissedPoll** — [[inv-i9]] across N fds: never asleep while a
  registered fd is ready. The register-then-observe order
  (`dev->poll` installs + samples in one locked step) is the
  mechanism under proof.
- **NoStaleHook** — a returned poll holds no hook; the hooks are
  stack memory, so a leftover is a dangling pointer the next
  readiness walk dereferences. In the impl this is the sweep — which
  must run on EVERY exit, including the `TSLEEP_INTR` death arm.

## What it cannot see

The hook-lifetime UAF class ([[fnd-rw2-2cf1]]) is BELOW the model:
the spec has no object lifetimes, so a sibling thread freeing the
polled object mid-sleep is inexpressible. The retain discipline is
prose + audit territory, not a green-run guarantee — the same
blindness class [[spec-sched-tickless]] has for #363.

## Binding

`specs/SPEC-TO-CODE.md::poll.tla`: Register ↔ the first scan +
`dev->poll`; CommitOrSleep ↔ the flag check + tsleep; MakeReady ↔
`poll_waiter_list_wake`; the timeout composes with
[[spec-tsleep]].
