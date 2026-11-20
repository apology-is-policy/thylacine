---
id: chg-2026-11-20-newidle-pull
type: chg
title: "An idle CPU steals before it parks: the newidle pull, the one path TI-4 left with no steal"
date: 2026-11-20
arc: arc-tickless-idle
commits: ["79357ca59", "81e11e87b", "4992e3758"]
touched:
  - sub-kernel-sched-smp
  - spec-sched-rebalance
established: []
closed: []
opened: []
mirrors-checked:
  - "no ABI: kernel/sched.c sched_idle_park only; no syscall, wire or on-disk change"
depth: skeletal
no-dossier-change: "the note changes no code; sub-kernel-sched-smp and spec-sched-rebalance were co-staged with the code in 79357ca59 and 4992e3758"
created: 2026-11-20
---
Found by measuring the slow present and echo tail under HVF (q-ps5, work/nov03-mm1/ps5/JUDGEMENT.txt): 4 of 43 slow wakes were a thread placed on an idle CPU the host had stalled for 5.5-6.2 ms, while another CPU went idle 7-820 us after the wake and parked over it until its backstop.

**The mechanism.** A CPU whose real thread blocks reaches its idle through `pick_next`: the pinned in-tree idle sits in `BAND_IDLE`, so `pick_next` returns it and `sched()` never calls `balance_pull`. When the idle last switched out inside `sched_idle_park` (one of the park's own `sched()` calls dispatched that thread), it resumes past that `sched()` and parks with no steal of its own. A thread the IPI's IRQ-return preemption dispatched leaves the idle outside the park, and the next park's top `sched()` steals as before. TI-4's push placement and busy-side kick did not cover the first path.

**The fix (79357ca59).** In the park's #363 loop, with the production flag on and a peer's work queued, one more `sched()` with the idle current finds nothing local and steals; an unchanged `nctxt` (pinned work, a contended lock) sends the CPU on to park. Linux's newidle balance; Plan 9's single run queue never needed it.

**The witness and its first failure (81e11e87b).** `scheduler.idle_park_steals_before_parking` passed with the pull sabotaged: its push-placed blocker was IRQ-return dispatched, so the idle resumed outside the park. The blocker now yields first. Sabotaged (7e9e51c65) it FAILS with the park message; fixed it PASSES, suite 2388 / 0.

**Audit r1** (Opus 5.5 max, same family, 0/1/0/3): the kernel change sound. F1 was the witness (fixed in 81e11e87b); F2 doc precision, F3 a `Pull` action in `sched_rebalance.tla` with `RunningImpliesNotParked` (the `~pending` guard is faithful but undiscriminated: one unit of surplus), F4 a test leak (all in 4992e3758). The prosecutor's observation, pre-existing: main.c turns notify on before secondary preemption (OPEN-BUGS 2026-11-20T03:47Z).

**Leases.** q-ni1 / q-ni1b: the witness both ways, 17 scheduler TLC cfgs. q-ni2 (4992e3758): q-ni2 (4992e3758, 03:50:50-04:53:38Z): the SMP subset gate, default-smp4 10/10 and ubsan-smp4 10/10 PASS, 0 corruption; the last boot shows the witness PASS, 2388 / 0. The UBSan image is 0x778000, 32 KiB under the 8 MiB window (default 0x713000, 436 KiB): OPEN-BUGS 2026-11-06 addendum.

**Not run:** the full SMP matrix (held since 2026-11-02), the interactive fleet.
