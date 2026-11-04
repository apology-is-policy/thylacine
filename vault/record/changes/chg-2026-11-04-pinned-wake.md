---
id: chg-2026-11-04-pinned-wake
type: chg
title: "A pinned thread's wake goes to its home CPU, not the waker's"
date: 2026-11-04
arc: arc-phase6-pouch
commits: []
touched:
  - sub-kernel-sched-smp
established: []
closed: []
opened: []
depth: skeletal
created: 2026-11-04
---
`select_target_cpu` answered `prev_cpu` for a `cpu_pinned` thread, and `ready()` passes the waker's
CPU as that, so the boot thread woken from a peer ran on the peer. Before
`smp_enable_secondary_preemption` a secondary has no tick, so the boot's tick wait then slept in WFI
with nothing to end it. Since #864, 2026-06-05. C8a's peer-placed torpor tests made it show, about two
boots in three of a narrowed suite. Sampled from QEMU's monitor at the hang: `boot_main` on CPU 1 in
`timer_busy_wait_ticks`, the other three in `sched_idle_park`, and the poke itself ended the WFI.

Witnesses: `scheduler.pinned_wake_goes_home`, the unit leg in
`scheduler.select_target_cpu_homogeneous_is_prev`. The test runner now fails a test that leaves the
boot thread off CPU 0, and `timer_busy_wait_ticks` extincts off cpu0 rather than hang.
