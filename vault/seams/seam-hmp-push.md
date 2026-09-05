---
id: seam-hmp-push
type: seam
title: "balance() is pull-only; misfit push is deferred to real heterogeneous hardware"
status: open
surface: [sub-kernel-sched-smp]
opened-by: chg-2026-06-05-864-hmp-foundation
tracker: "ARCH 8.4.4 -- the verification boundary"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

Capacity-aware **push** migration: moving a heavy task off a
low-capacity CPU onto a high-capacity one *even when the latter is not
idle*. That is the one HMP mechanism a pull-only stealer structurally
cannot express — an idle CPU pulls, but nothing makes a busy big core
take work from a busy little one.

Also owed with it: the empirical EAS tuning — PELT decay constants, an
energy model, schedutil/DVFS, real misfit thresholds. `SCHED_MISFIT_PCT`
is 80 and `SCHED_UTIL_SHIFT` is 3 because they are plausible, not
because they were measured.

## Why it was deferred

The **verification boundary** (ARCH §8.4.4): none of it is verifiable on
QEMU or HVF, where every CPU is identical. Tuning numbers against a
uniform emulator would produce confident, meaningless constants.

What was built instead is the *shape*: `balance()` wraps `try_steal`
today with the same signature a push path would use, and the push
primitives already exist — `ready_on` is cross-CPU enqueue,
`sched_notify_cpu` wakes the target. Adding push is a tick-time misfit
scan calling those two, which is additive, not a rewrite. And safety for
*any* placement is already proved: [[spec-sched-alpha]]'s `Place` picks
its target non-deterministically.

## Cost of leaving it

Zero on every current target: `g_sched_hetero` is false on QEMU virt and
RPi, so `select_target_cpu` short-circuits to `prev_cpu` before reading
a capacity, and the whole layer is inert.

The real cost is that the HMP path is **unreachable by the runtime
matrix**, which is why its audit findings ([[fnd-866-r1-f1]],
[[fnd-866-r1-f3]]) were found by reasoning rather than by tests, and why
the two load-bearing pure functions (`sched_capacity_normalize`,
`sched_place_by_capacity`) are unit-tested against a *synthetic*
asymmetric DTB — logic-verified without the hardware.
