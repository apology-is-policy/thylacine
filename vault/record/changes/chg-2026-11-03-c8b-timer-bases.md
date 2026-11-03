---
id: chg-2026-11-03-c8b-timer-bases
type: chg
title: "Per-CPU timer bases: a wake takes no timer lock"
date: 2026-11-03
arc: arc-phase6-pouch
commits: ["0905ba39d"]
touched:
  - sub-kernel-rendez
  - sub-kernel-sched-smp
  - sub-kernel-sched
  - sub-kernel-death
  - sub-kernel-poll
  - sub-kernel-ninep-dev9p-poll
established: [dec-2026-11-03-c8b-timer-bases]
closed: [seam-timerwait-sharding]
opened: []
depth: skeletal
created: 2026-11-03
---
`g_timerwait` is gone. Every thread in a deadlined `tsleep` is linked on one CPU's `struct TimerBase`, sorted by deadline; that CPU's tick expires the head. A wake takes `r->lock` alone and leaves the entry; the sleeper unlinks on its way out, before it clears `rendez_blocked_on`. The tick unlinks under `r->lock`, because an owner that reads its mark clear skips the base's lock and takes `r->lock` next. A secondary with no armed tick links on cpu0's base and kicks cpu0. `tsleep.tla` restated: the tick split into three steps, `TimerEntryHasRendez` and `RendezOutlivesTick` new, `lazy_unlink` retired, three new buggy cfgs. NOT LANDED: compiled, model-checked, not booted.
