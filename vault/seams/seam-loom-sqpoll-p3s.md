---
id: seam-loom-sqpoll-p3s
type: seam
title: "the SQPOLL poll substrate's four P3s (yield-spin, re-arm jitter, spurious poll()==0, the untested KOBJ_LOOM poll arm)"
status: open
surface: [sub-kernel-loom, sub-kernel-poll]
opened-by: fnd-kt1-r1-a1
tracker: "memory bug_loom_poll_substrate_p3s.md; the KT-1 audit round 1 (A-F1..A-F4)"
created: 2026-09-05
updated: 2026-09-05
---
## Owed

A follow-up KERNEL chunk (its own SMP gate) for [[fnd-kt1-r1-a1]] (the SQPOLL kthread's `P9_PUMP_BUSY` yield-spin is the production steady state now that halcyond's session is an EL0 SQPOLL consumer -- measure, then park on BUSY), [[fnd-kt1-r1-a2]] (an SQE queued during the kthread's 10 ms boundary recv waits for that recv -- document the bound or shorten the idle), [[fnd-kt1-r1-a3]] (a `cq_waiters` poller can be woken with no CQE and `poll(2)` returns 0 under an infinite timeout -- re-sleep), and [[fnd-kt1-r1-a4]] (no deterministic kernel test drives `poll_scan_one`'s KOBJ_LOOM arm with the RW-2 2C-F1 keep_out loom-ref).

## What closes it

The chunk lands with a measurement of the yield-spin under a synchronous RPC, the three code changes, and the kernel test; the four findings flip to fixed via its chg.

## Risk while open

Efficiency and latency only (a spin, a 10 ms stall, one spurious empty wake); no soundness claim is open. halcyond's session loop tolerates a zero return by re-polling.
