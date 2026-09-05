---
id: spec-sched-rebalance
type: spec
title: "sched_rebalance.tla"
models: [sub-kernel-sched-smp]
pins: [inv-i8, inv-i9]
cfgs:
  - "sched_rebalance.cfg -- clean: PendingImpliesNotRunning + the fairness-conditioned liveness"
  - "sched_rebalance_buggy_nokick.cfg -- the busy CPU never kicks: work strands"
  - "sched_rebalance_buggy_nolift.cfg -- the kick does not lift a registered park: the wake is lost"
gate: "any change to the busy-side overload kick or the surplus predicate it fires on"
created: 2026-08-01
updated: 2026-08-01
---
## Abstraction

The **push-on-overload** mechanism: a busy CPU that holds surplus queued
work and sees a parked peer kicks that peer to come steal.

Written model-first for TI-4, and it exists because of a regression whose
cause nobody had named. NO_HZ_IDLE stopped the 1 kHz tick — and that tick
had silently *been* the work-stealing re-poll. An idle CPU re-ran
`try_steal` every tick, so any work-arrival the best-effort single kick
missed was pulled within a millisecond anyway. Remove the tick and queued
work strands on a busy CPU until the backstop: a 2.4x boot slowdown.

The fix is the Linux NO_HZ_IDLE shape (confirmed against FreeBSD ULE and
Zircon as well): tickless stops only the *idle* tick, so a **busy** CPU is
still ticking and can drive rebalancing. This module models exactly that
one new mechanism.

## What it pins

- **`Overload`** — the kick action itself, and the composed claim that a
  busy producer with surplus plus a parked peer eventually parallelizes.
  That is [[inv-i8]]'s work-conservation clause.
- **The register-then-observe leg** — the kick must *lift* a parked
  peer's park, which holds only because the peer set `idle_in_wfi` before
  parking ([[spec-sched-tickless]]). This is [[inv-i9]] restated on the
  push path.

The two buggy cfgs separate the two ways to lose the work, which is the
useful part: `nokick` is "the mechanism is absent", `nolift` is "the
mechanism fires and the wake is still lost". Only the second is a
concurrency bug; the first is a design gap. Distinguishing them is what
the model is for.

## Impl correspondence

`sched_tick`'s tail: on a CPU whose current is not its own idle, with
`cpu_has_surplus_for_kick(cs)` true, call `sched_notify_idle_peer()`.
Gated on the production flag so the test phase stays quiescent.

The model's "one-shot kick == migrate" is operationally realized by the
*repeated* per-tick kick plus `try_steal` on the woken peer — which sits
inside [[spec-sched-alpha]]'s proven arbitrary-placement envelope, so no
additional safety argument is needed for where the work lands.

## Gate

All three cfgs on any change to the kick or to the surplus predicate. The
predicate is lock-free by design (relaxed reads of band heads, never a
Thread deref), so a change that makes it dereference is a change to its
safety argument, not just its logic.
