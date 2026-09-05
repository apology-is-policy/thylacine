---
id: fnd-torpor8-r1-f2
type: fnd
round: adt-torpor8-r1
severity: P2
status: documented
title: "torpor_lock held across wakeup()'s on_cpu spin — global futex serialization"
surface: [sub-kernel-torpor]
threatens: []
seam: seam-torpor-lock-wake-spin
regression: "none — a latency hazard, not a correctness violation; the SMP gate is the standing witness"
created: 2026-08-01
---
## Prosecution

Every wake walk calls `wakeup()` under `torpor_lock`. If the woken
thread is mid-context-switch on a peer CPU, `wake_rendez_waiter`
spins on `t->on_cpu` until the switch-out drains — with the ONE
global futex lock held. Under multi-Proc futex contention, every
WAIT and WAKE system-wide queues behind that spin.

## Disposition

Documented, deliberately: dormant at v1.0 (single-Proc-mostly), the
critical section is bounded by an independent-progress spin, and
both candidate fixes change lock granularity on an audited surface.
Three walks now share the hazard (per-VA, death, stop) — a fix must
land uniformly. Open as [[seam-torpor-lock-wake-spin]]; the go-arc's
multi-M futex traffic is the workload most likely to end the
dormancy.
