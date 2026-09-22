---
id: spec-poll-cpu
type: spec
title: "poll_cpu.tla"
models: [sub-kernel-poll, sub-kernel-sched]
pins: [inv-i27]
cfgs:
  - "poll_cpu.cfg -- two pollers on one CPU, the preemption point ON: CpuServesIrqs + EachPollerSleeps hold (16 states)"
  - "poll_cpu_buggy_sleep_only.cfg -- ROUND-6 S1: round 5's per-thread sleep bound granted in FULL as fairness, and the CPU still never unmasks (CpuServesIrqs counterexample, 12 states)"
  - "poll_cpu_sleep_bound_holds.cfg -- the positive control ONE VARIABLE AWAY: EachPollerSleeps HOLDS in that same sleep-only configuration, so the counterexample is not 'the pollers stopped sleeping' (12 states)"
  - "poll_cpu_one_poller.cfg -- the K=1 control: round 5's bound WAS sound with a single poller, so the counterexample is about COMPOSITION, not about sleeping being useless (4 states)"
gate: "any change to the preemption point, to what unmasks a CPU (the idle loop, EL0 return), or to the claim that poll's bound composes across pollers -- specs/check-poll.sh"
created: 2026-09-22
updated: 2026-09-22
---
## Abstraction

ONE CPU, K pollers, and nothing else. Readiness, hooks, deadlines,
death and stop all belong to [[spec-poll]] and are deliberately absent:
this module asks one question, about one CPU. A "poller" here is any
thread looping in an IRQ-masked syscall that an unprivileged producer
can keep waking; poll is the instance that has one.

The CPU is unmasked in exactly two places: idle (the idle loop restores
the caller's mask around its WFI) and at a preemption point. Everywhere
else a poller is inside a syscall body, which runs masked end to end
(ARCH 8.11; 8.1 records that this was never the design). Reaching EL0
would unmask too, but a `poll(-1)` under noise never returns, so the
model gives the adversary the stronger world where it does not.

## Why it exists, separately from [[spec-poll]]

Round-7 F2. `poll.tla` has ONE poller, so its `IrqLatencyBounded` is a
claim about that thread. `[]<>(pc \in RealSleep)` IMPLIES that property
by set inclusion, so a behaviour in which the poller sleeps for ever and
never reaches the point satisfies it -- and that is round-6 S1's exact
shape. Measured: `poll_buggy_no_point` discriminates a poller that NEVER
SLEEPS (round-5 F1), not S1. A single-poller model has no CPU to be
masked and structurally cannot carry the composition claim.

## The step that carries S1

`SleepHandoff(p, q)`: the running poller blocks and `sched()` dispatches
a RUNNABLE peer without the CPU ever idling, because the switch happens
inside the masked syscall. That is why "the thread really slept" does
not imply "the CPU took an interrupt". `SleepIdle(p)` is the other
sleep -- no runnable peer, so the CPU idles and unmasks -- and it is the
ONLY one available at K=1, which is why round 5's bound was sound there.

The loop's ORDER is modelled, not abstracted: a pass reaches its point
BEFORE it can sleep again, because the point sits at a fixed place in
the loop body and the tsleep is below it. A first cut let a poller sleep
straight out of `run`, which let the adversary hand the CPU off before
the point ever fired and made the CLEAN cfg fail. The code gives the
adversary no such choice.

## Action-site map

| TLA action | code |
|---|---|
| `Point` / `PointDone` | `kernel/sched.c::sched_preempt_point` (unmask + `isb` + re-mask, `preempt_count` held) |
| `PassNoPoint` | round 5's loop: a pass that reaches its tsleep without ever unmasking |
| `SleepHandoff` | `sched()` picking a runnable peer inside the masked syscall (`kernel/sched.c`) |
| `SleepIdle` | `sched_idle_park` -- the CPU reaches its idle loop and restores the mask |
| `Wake` | a producer's `poll_waiter_list_wake` on another CPU |
| `Open` | the CPU is idle, or at a point: the two places it is not masked |

## Properties

- **CpuServesIrqs** `[]<>Open` -- THE property. This CPU services
  interrupts again and again, whatever the producers do. [[spec-poll]]'s
  `IrqLatencyBounded` is the same sentence about one THREAD; this one is
  about the CPU, so a sleep that hands the CPU to another masked poller
  does not satisfy it.
- **EachPollerSleeps** -- round 5's bound, stated as a property so the
  counterexample is self-documenting: it HOLDS in the buggy cfg while
  `CpuServesIrqs` fails. Every poller really sleeps infinitely often,
  and the CPU still never takes an interrupt.

## What it does NOT establish

The length of one masked span. Round-7 F1: nothing caps hooks per
`poll_waiter_list`, so a producer's `poll_waiter_list_wake` walk and the
poller's per-pass unregister walks are O(attacker-scaled) and masked,
and the producer's walk crosses no point at all. This module counts
masked spans; it does not measure them.
