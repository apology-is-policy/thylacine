---
id: spec-sched-oncpu
type: spec
title: "sched_oncpu.tla (the #860 diagnostic)"
models: [sub-kernel-sched-smp]
pins: [inv-i21]
cfgs:
  - "sched_oncpu_prefix860.cfg / _3cpu -- reproduces #860: NoGuardFire violated"
  - "sched_oncpu_optionA.cfg / _3cpu -- option A (guard the dispatch)"
  - "sched_oncpu_optionB.cfg / _3cpu -- option B (the in-tree idle)"
  - "sched_oncpu_intree_guard.cfg -- the in-tree idle with the guard retained"
gate: "counterexample-only -- not a pre-commit gate; kept as the reproduction of the bug class"
created: 2026-08-01
updated: 2026-08-01
---
## Abstraction

The **diagnostic** model. It re-introduces exactly what
[[spec-scheduler]] abstracts away:

- `on_cpu[t]` — the cross-CPU "this context is claimed / in use" flag.
- A per-CPU `locked[c]` held **across** the multi-step switch (the impl's
  run-queue lock, held from `sched()` entry until the resuming thread's
  `finish_task_switch`) — which is what makes a peer's `spin_trylock`
  skip a mid-switch CPU.
- The boot CPU's second dispatch route: `BootIdle` reached through a
  global pointer, independent of any runqueue.

With those three restored, `#860` reproduces as a modelled trace.

## What it pins

Nothing, in the pre-commit sense — this module is a **counterexample
generator**, not a gate. Its value is that it made the bug class visible
at the model level and let the two candidate fixes be compared before
either was written:

- **Option A**: keep the boot-CPU deadlock dispatch and guard it.
- **Option B**: give every CPU a pinned, *in-tree* idle and delete the
  special case entirely.

The cfgs check both. Option B is what shipped, and
[[spec-sched-alpha]] is the model of it.

## Why it is kept

Because the shape of the failure is worth being able to re-run. The
lesson `sched_oncpu` encodes is methodological: a model that is green
while the system it describes has a live SMP bug is not evidence of
soundness — it is evidence that the model does not contain the
mechanism. Keeping the diagnostic alongside the gating model makes that
concrete rather than a maxim.

Its `NoGuardFire` invariant is the one that breaks under the `prefix860`
cfg; `Safety` and `Availability` are the same shape as
[[spec-sched-alpha]]'s.

## Gate

Not run as a pre-commit gate (`prefix860` is *expected* to fail — that is
its job). Re-run by hand if the dispatch protocol is restructured, to
confirm the new shape still cannot reach the traces this module names.
