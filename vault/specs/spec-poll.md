---
id: spec-poll
type: spec
title: "poll.tla"
models: [sub-kernel-poll]
pins: [inv-i9]
cfgs:
  - "poll.cfg -- clean: every invariant, HAS_TIMEOUT (2146 states)"
  - "poll_notimeout.cfg -- poll(-1), the infinite wait; safety holds (944)"
  - "poll_liveness.cfg -- Spec_Live: PollTerminates + StableReadyReturns + DeathTerminates + StopHonoured (2146)"
  - "poll_liveness_notimeout.cfg -- Spec_Live, poll(-1): StableReadyReturns + DeathTerminates + StopHonoured (944)"
  - "poll_buggy_check_before_register.cfg -- sample-then-register: a readiness edge in the gap reaches no hook (NoMissedPoll counterexample)"
  - "poll_buggy_no_wake.cfg -- producer sets the flag but never signals the Rendez (NoMissedPoll counterexample)"
  - "poll_buggy_lazy_unregister.cfg -- poll returns still-listed (NoStaleHook counterexample)"
  - "poll_buggy_clear_after_sample.cfg -- the fresh hook's flag cleared AFTER its sample (NoMissedPoll counterexample)"
  - "poll_buggy_return_on_wake.cfg -- an empty re-sample returns 0 (NoSpuriousZero counterexample)"
  - "poll_buggy_no_loop_die_check.cfg -- death left to tsleep's die-check, which a set flag short-circuits (DeathTerminates counterexample)"
  - "poll_buggy_no_loop_stop_check.cfg -- a stop left to tsleep's detour (StopHonoured counterexample)"
gate: "any change to the register/sample atomicity, the re-arm pass, the sweep, the loop's death/stop checks, or a producer wake site -- specs/check-poll.sh"
created: 2026-08-01
updated: 2026-09-21
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
- **NoSpuriousZero** + **PollTerminates** + **StableReadyReturns**
  (2026-09-21, the re-arm): readiness is a LEVEL (`MakeReady` /
  `Retract`), a flag is a HINT (`OtherEvent` walks a list for an event
  the poller did not ask about), `seen` separates the sample from the
  flag. poll returns 0 only at its deadline; the loop's own deadline
  test bounds a producer that never stops walking; an fd that stays
  ready ends any poll.
- **DeathTerminates** + **StopHonoured** (audit round 4): `TSleepCommit`
  keeps tsleep's real order — cond, deadline, stop detour, die-check —
  because that order is the bug: a set flag short-circuits both checks,
  so the pass (`Rearm` → `LoopCheck` → `Resample`) makes them itself,
  unhooked (`ParkedLoopHoldsNoHook`). One stop per behavior: unbounded
  stop/continue can hold even a dying thread in tsleep's detour, a race
  [[spec-debug-stop]] owns.
- The pass RE-REGISTERS (`Rearm` takes every hook off; `Resample` is the
  first scan's install-and-sample again). Why that matters is not
  visible here — one list per fd — and is pinned by [[spec-cons-poll]],
  where the Dev chooses its list by state.

## What it cannot see

The hook-lifetime UAF class ([[fnd-rw2-2cf1]]) is BELOW the model:
the spec has no object lifetimes, so a sibling thread freeing the
polled object mid-sleep is inexpressible. The retain discipline is
prose + audit territory, not a green-run guarantee — the same
blindness class [[spec-sched-tickless]] has for #363.

## Binding

`specs/SPEC-TO-CODE.md::poll.tla`: Register ↔ the first scan +
`dev->poll`; TSleepCommit ↔ the flag check + tsleep; Rearm ↔
`poll_unhook_all`; LoopCheck / ParkDeath / StopResume ↔ the loop's
`thread_die_pending` + `proc_stop_sleeper_park`; Resample ↔ the
re-registering scan; MakeReady ↔ `poll_waiter_list_wake`; the timeout
composes with [[spec-tsleep]]. `specs/check-poll.sh` asserts every
cfg's verdict (clean counts pinned; each buggy cfg's NAMED property).
