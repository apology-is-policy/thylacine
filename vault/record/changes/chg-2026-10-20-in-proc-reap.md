---
id: chg-2026-10-20-in-proc-reap
type: chg
title: "An exited peer thread is freed at its Proc's next thread create (the in-Proc reap)"
date: 2026-10-20
arc: arc-phase6-pouch
commits: ["*(pending)*"]
touched:
  - sub-kernel-thread
  - sub-kernel-proc
  - sub-kernel-death
  - sub-kernel-syscall-dispatch
  - sub-pouch-thread
  - inv-i32
established: []
closed: []
opened:
  - seam-exited-threads-wait-for-a-create
mirrors-checked: []
depth: skeletal
created: 2026-10-20
---
A thread that exits while its peers live used to stay on its Proc's list,
counted against `PROC_THREAD_MAX`, until `wait_pid` reaped the whole Proc, so a
long-lived program that starts and joins workers was refused every thread
create at 256: OpenRCT2 froze in the operator's 2026-10-19 demo rehearsal, and
libxml2's runtest failed `pthread_create`. `proc_reap_exited_threads`
([[sub-kernel-thread]]) now frees every exited peer that has left its CPU, at
the Proc's next thread create or Loom SQPOLL setup. It claims each one by the
unlink under [[lock-proc-table]], folds its CPU time and kstack watermark into
the Proc, and clears a debug focus that names it. Audit round 1 (Fable 5.1 on
Opus 5.5): 0 P0, 0 P1, 1 P2 (the SQPOLL charge, fixed), 3 P3. What it leaves
is [[seam-exited-threads-wait-for-a-create]].
