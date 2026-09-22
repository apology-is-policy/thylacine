---
id: spec-poll
type: spec
title: "poll.tla"
models: [sub-kernel-poll]
pins: [inv-i9]
cfgs:
  - "poll.cfg -- clean: every invariant, HAS_TIMEOUT (2194 states)"
  - "poll_notimeout.cfg -- poll(-1), the infinite wait; safety holds (968)"
  - "poll_liveness.cfg -- Spec_Live: PollTerminates + StableReadyReturns + DeathTerminates + StopHonoured + IrqLatencyBounded (2194)"
  - "poll_liveness_notimeout.cfg -- Spec_Live, poll(-1): StableReadyReturns + DeathTerminates + StopHonoured + IrqLatencyBounded (968)"
  - "poll_buggy_check_before_register.cfg -- sample-then-register: a readiness edge in the gap reaches no hook (NoMissedPoll counterexample)"
  - "poll_buggy_no_wake.cfg -- producer sets the flag but never signals the Rendez (NoMissedPoll counterexample)"
  - "poll_buggy_lazy_unregister.cfg -- poll returns still-listed (NoStaleHook counterexample)"
  - "poll_buggy_clear_after_sample.cfg -- the fresh hook's flag cleared AFTER its sample (NoMissedPoll counterexample)"
  - "poll_buggy_return_on_wake.cfg -- an empty re-sample returns 0 (NoSpuriousZero counterexample)"
  - "poll_buggy_no_loop_die_check.cfg -- death left to tsleep's die-check, which a set flag short-circuits (DeathTerminates counterexample; needs no bound disabled, since the point checks neither death nor stop)"
  - "poll_buggy_no_loop_stop_check.cfg -- a stop left to tsleep's detour (StopHonoured counterexample)"
  - "poll_buggy_no_point.cfg -- no preemption point: noise keeps poll(-1) awake, IRQ-masked, for ever (IrqLatencyBounded counterexample)"
  - "poll_cpu.cfg -- [[spec-poll-cpu]]: two pollers on ONE CPU, the point on; CpuServesIrqs + EachPollerSleeps hold (16 states)"
  - "poll_cpu_buggy_sleep_only.cfg -- round-6 S1: round 5's per-thread sleep bound granted in full, and the CPU still never unmasks (CpuServesIrqs counterexample, 12 states)"
  - "poll_cpu_sleep_bound_holds.cfg -- the positive control one variable away: EachPollerSleeps HOLDS in that same configuration, so the counterexample is not 'the pollers stopped sleeping' (12)"
  - "poll_cpu_one_poller.cfg -- the K=1 control: round 5's bound WAS sound with one poller (4)"
gate: "any change to the register/sample atomicity, the re-arm pass, the sweep, the loop's death/stop checks, the preemption point, or a producer wake site -- specs/check-poll.sh"
created: 2026-08-01
updated: 2026-09-22
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
- **IrqLatencyBounded** (round 5 F1, restated at round 7): the poller
  reaches a real sleep OR its preemption point (`atpoint`) again and
  again -- no producer can keep it from one, and between them the syscall
  is IRQ-masked. A timeout does not supply the bound -- a producer can
  hold a ten-second poll for all ten -- so it holds on poll(-1) too.
  `Point` is UNCONDITIONAL (`LoopCheck` routes every re-loop through it,
  before the rescan), which is what round 5's budget-gated sleep was not.
  The point checks NEITHER death nor stop, so it cannot mask a missing
  loop check: the two loop-check buggy cfgs now reproduce with
  `BUGGY_NO_POINT=FALSE`, where round 5's backoff had needed its own
  bound disabled. **Read the scope limit above**: this property is about
  one THREAD; the CPU-level composition is [[spec-poll-cpu]]'s.
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
re-registering scan; SpinLapse / BackoffCommit / BackoffTimeout ↔ the
loop's `nsleeps`-keyed budget and its `poll_never` tsleep; MakeReady ↔
`poll_waiter_list_wake`; the timeout
composes with [[spec-tsleep]]. `specs/check-poll.sh` asserts every
cfg's verdict (clean counts pinned; each buggy cfg's NAMED property).

## The CPU half is a SEPARATE module (round-7 F2)

This module has ONE poller, so `IrqLatencyBounded` is a claim about that
thread: it reaches a real sleep, or its preemption point, again and
again. That does NOT establish the CPU-level bound the point was built
for -- `[]<>(pc \in RealSleep)` implies the property by set inclusion, so
a behaviour in which the poller sleeps for ever and never reaches the
point satisfies it, and that is round-6 S1's exact shape. Measured:
`poll_buggy_no_point` discriminates a poller that NEVER SLEEPS, i.e.
round-5 F1 -- not S1.

The CPU obligation therefore lives in [[spec-poll-cpu]], which models K
pollers on one CPU and the handoff step that carries S1 (a blocking
poller dispatches a runnable peer without the CPU ever idling). Read the
two together: this one for what a poll call does, that one for what the
CPU gets.
