---
id: chg-2026-06-13-107-sched-toctou
type: chg
title: "#104/#107: the per-CPU pointer read before the IRQ mask"
date: 2026-06-13
arc: arc-holotype-rw
commits: ["f14ea712"]
touched: [sub-kernel-sched]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
---
## What

`sched()` masks IRQs **before** reading `this_cpu_sched()`, and
`ready_on` masks before reading `smp_cpu_idx_self()`. Plus a loud-fail
assert that the locked slot belongs to the running CPU, and the restored
syscall-return preempt point at the vector level.

## The bug

`sched()` read `cs = &g_cpu_sched[smp_cpu_idx_self()]` with IRQs
**enabled**. For a `sched()` entered enabled — a kthread's voluntary
yield — a timer IRQ in the read..lock-acquire window switched the thread
out, a peer work-stole it, and it resumed **on another CPU still inside
the same `sched()` call**, with `cs` still naming the *origin* CPU.

It then acquired the origin CPU's run-queue lock while running elsewhere,
which broke the `pending_release_lock` handoff and **leaked** that lock.
A later `sched()` on the origin CPU spun on the orphaned lock forever.
The classic "read a per-CPU pointer with preemption enabled".

## What made it hard

The prior mitigation had the wrong theory. #104 was hypothesized as a
"mid-handler steal" and closed by removing the syscall-return preempt —
which merely cranked migration churn back down. The bug was latent under
tick-only preemption, so a week ran green and the fix looked confirmed.

It was root-caused red-handed by in-kernel instrumentation — an assertion
that `sched()` holds a foreign CPU's lock, printing
`cs_idx=0 running_cpu=1` — which was then reverted before the real fix.
That assert survives in production form as the durable regression: a
timing-only SMP race has no deterministic test, so a permanent invariant
check is the only thing that catches a reintroduction.

Masking only `I` (not `A`/`F`) is deliberate and sufficient — the only
migration vector in the window is a taken IRQ.

## The audit ([[adt-107-r1]])

Converged **0 P0 / 0 P1 / 1 P2 / 2 P3**, all fixed. The P2
([[fnd-107-r1-f1]]) is the one that matters: `ready_on` carried the
**identical TOCTOU class**, and it was fixed by masking first rather than
by documenting a caller precondition — closing the class, not the
instance.

Verified with the previously-buggy preempt placement deliberately
re-enabled: 14/14 clean boots on TCG-smp4, against a ~30–50% deadlock
rate pre-fix. The root fix, not the placement, was what mattered.
