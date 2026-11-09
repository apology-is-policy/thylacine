---
id: chg-2026-11-04-c9-switch
type: chg
title: "Thread-model C9: the switch decides from TTBR0"
date: 2026-11-04
arc: arc-phase6-pouch
commits: [addcee94f, 370a5b5ea, 22aeb1aaa, f6b6b9d1e, 3ea459c7d]
touched:
  - sub-kernel-sched-smp
  - sub-kernel-thread
established: [dec-2026-11-04-switch-from-ttbr0]
closed: []
opened: []
depth: skeletal
created: 2026-11-04
---
E-4 and E-5, on measured figures. `cpu_switch_context` skips V state for a thread on ASID 0, skips the
TTBR0 write on an equal value, and has one ISB. Witnesses: `fp.round_trip_v_regs_fpsr_fpcr` moved onto
threads of a Proc, `fp.kernel_thread_holds_no_v`, `fp.user_v_survives_a_kernel_thread`,
`context.ttbr0_follows_next`; `context.switch_cost` reports by the threads a switch joins.
