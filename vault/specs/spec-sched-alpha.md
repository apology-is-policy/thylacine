---
id: spec-sched-alpha
type: spec
title: "sched_alpha.tla (the SMP-redesign gating model)"
models: [sub-kernel-sched-smp, sub-kernel-sched]
pins: [inv-i21, inv-i8]
cfgs:
  - "sched_alpha.cfg -- clean: Safety (2 workers x 2 CPUs)"
  - "sched_alpha_3cpu.cfg -- clean: Safety (3 workers x 3 CPUs)"
gate: "any change to the switch protocol, the steal path, placement, or the idle's dispatch route"
created: 2026-08-01
updated: 2026-08-01
---
## Abstraction

The **target architecture**, modelled before it was implemented. Written
model-first at the deep SMP review, and the module every subsequent
scheduler change is checked against.

What it models that [[spec-scheduler]] does not: `on_cpu`, the multi-step
switch under a per-CPU lock, and placement as a *non-deterministic*
choice — `Place` picks the target CPU arbitrarily. That last point is
load-bearing: it means the safety result holds for **any** placement
policy, so the HMP capacity heuristic, the idle-preferring push, and any
future affinity mask are all inside an already-proven envelope rather
than each needing their own argument.

What it deliberately does **not** model: any boot-CPU special case. There
is none in the design.

## What it pins

`Safety` is the conjunction:

- `NoSimultaneousRun` / `OwnerUnique` — one CPU per thread.
- `OnCpuMeansOwned` / `RunningImpliesOnCpu` — the claim flag and the
  running state agree.
- `RunqRunnable` / `RunqOnCpuSafe` — a runqueue holds only RUNNABLE,
  unclaimed threads. This is the pair the impl asserts directly in
  `pick_next` and `try_steal`.
- `NoDoubleEnqueue` — a thread is in at most one tree.
- `IdleStaysHome` — a pinned idle never migrates.
- `IdleAvailable` — a CPU always has *something* dispatchable, which is
  what makes the old deadlock path structurally unreachable rather than
  merely unused.

Together they are [[inv-i21]]; `AlwaysRunning` carries the
[[inv-i8]] shape.

## The two model-checked design decisions

1. **The idle is in-tree and pinned.** Pinning gives `IdleStaysHome`
   (`try_steal` skips it); in-tree gives `IdleAvailable` (ordinary
   `pick_next` finds it). Together they retire the off-tree dispatch
   pointer that was #860's root cause — the guard-it alternative is
   modelled in [[spec-sched-oncpu]] and was not taken.
2. **Placement is arbitrary.** Because `Place` is non-deterministic, the
   safety proof does not depend on the placement policy being any
   particular function — which is exactly the property the HMP
   foundation needed, since its capacity logic is verified separately as
   two pure functions against a synthetic asymmetric DTB.

## Gate

Both cfgs, clean, on any change to: the switch protocol (claim, handoff,
resume), the steal path, the placement policy, or how an idle is
dispatched. A new placement heuristic does *not* need a new proof — it
needs to stay inside `Place`'s non-determinism, which means it must not
introduce a case where a thread is enqueued on two trees or placed while
claimed.
