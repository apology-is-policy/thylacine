---
id: chg-2026-06-05-864-hmp-foundation
type: chg
title: "#864 + the #866 audit close: the placement seam, capacity, and util"
date: 2026-06-05
arc: arc-deep-smp-review
commits: ["df7c794d", "206680a6"]
touched: [sub-kernel-sched, sub-kernel-sched-smp]
established: []
closed: []
opened: [seam-hmp-push]
mirrors-checked: []
depth: rich
---
## What

Separated placement **policy** from enqueue **mechanism**, and gave the
policy something to decide with.

- `ready()` becomes `ready_on(select_target_cpu(t, self), t)`.
- `ready_on(cpu, t)` enqueues onto an arbitrary CPU's tree under that
  CPU's lock — the cross-CPU primitive a push path needs.
- Per-CPU `capacity`, parsed and normalized from the DTB's
  `capacity-dmips-mhz`; per-task `util`, an EWMA accruing while running
  and decaying while blocked.
- `balance_pull()` wraps `try_steal` with the signature a push path
  would share.

On every v1.0 target this is **inert**: `g_sched_hetero` is false, so
`select_target_cpu` returns `prev_cpu` and `ready()` is byte-identical to
its predecessor. The audit verified that step by step.

## The verification boundary

The empirical EAS work — PELT decay constants, an energy model,
schedutil/DVFS, real misfit thresholds — is deferred to real
heterogeneous hardware, because tuning it against a uniform emulator
produces confident meaningless numbers. What *is* verified is the logic:
the two load-bearing decisions are extracted as **pure functions**
(`sched_capacity_normalize`, `sched_place_by_capacity`) and unit-tested
against a synthetic asymmetric DTB. [[seam-hmp-push]].

And the safety of *any* placement is already proved: [[spec-sched-alpha]]'s
`Place` picks its target non-deterministically, so the heuristic sits
inside a proven envelope rather than needing its own argument.

## The audit ([[adt-866-r1]])

One Opus prosecutor plus an independent self-audit, on the redesign as a
whole. **0 P0 / 1 P1 / 0 P2 / 5 P3 — clean, not dirty.**

Every finding lived on the cross-CPU or declared-heterogeneous path,
which is structurally unreachable on the uniform targets — *which is
exactly why the runtime matrix could not reach them and the audit had to
prosecute them by reasoning*. [[fnd-866-r1-f1]] (the placement
`need_resched`), [[fnd-866-r1-f2]] (the head-only steal scan),
[[fnd-866-r1-f3]] (the capacity publish), [[fnd-866-r1-f4]] (the
restored self-index guard).

The core was confirmed sound by both passes: byte-identical homogeneous
behavior, a deadlock-free single-lock `ready_on`, single-writer `util`
by [[inv-i21]], and #860 closed by construction.
