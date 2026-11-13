---
id: chg-2026-11-10-tick-fold-r16
type: chg
title: "Audit round 16 on the tick fold and the full-pool probe: a test with sound bounds, a freed thread's time kept, the probe's two false reds"
date: 2026-11-10
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-kernel-sched
  - sub-kernel-thread
  - sub-kernel-devproc
  - sub-stratum-boot
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-10
---
Round 16 (Opus 5.5 on Fable's work, 0 P0 + 0 P1 + 3 P2 + 5 P3, and one defect outside the round's commits) found the tick fold itself sound and its witnesses not.

`proc.cpu_ns_running_thread` compared a guest-clock reading with constants: a window in which the virtual CPU was not run would overshoot its upper bound, and one with late ticks would undershoot the lower, and a failed kernel test ends the boot. It now counts its window in ticks charged to the thread (its slice counts them down) and asserts relations between values read in one masked section: something was charged, what was charged is exactly how far the stamp moved, and three ticks charged at least a millisecond.

A kernel thread freed by its joiner took its `run_ns` out of its Proc's total (`thread_unlink_from_proc` did not fold it; only the reap did), so the kernel Proc's `cpu_ns` fell at each such free. The free now folds it in the hold that unlinks the thread (`proc.cpu_ns_survives_thread_free`). This predates the tick fold.

The probe had two ends that blamed the server for the holder's own timing: a holder that stalled between connects read as a wait, and then failed its next connect at a pool nobody would free (`REARM`); and a waiting connect served a moment before the probe had counted out its quiet read as the holder ending early. Both are now retaken, told apart by what the holder wrote after the quiet. The probe also requires the waiting connect to return within one quiet of its own free, and refuses a `/ctl/procs` table that ends within a row of the kernel's 4096-byte bound, because the kernel drops the rows that do not fit without saying so.

`test_caught_settle` (the caught-wait helper) read a moved switch-in stamp as "the thread ran again"; the tick now moves that stamp too, so it compares the dispatch count. The ARCH 25.4 scheduler row and its AUDIT-TRIGGERS copy named the switch boundary as the only writer of `run_ns`; both now name the tick as the second write site and say what to prosecute.

Not changed: `/ctl/procs` still shows only the rows that fit one 4096-byte render, silently; that is its own defect and is queued.
