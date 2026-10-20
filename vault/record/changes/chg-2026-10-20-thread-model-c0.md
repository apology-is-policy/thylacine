---
id: chg-2026-10-20-thread-model-c0
type: chg
title: "Thread-model C0: exec waits for exiting peers; an exit tail keeps its CPU; spawn in a dying Proc and spent id spaces refuse"
date: 2026-10-20
arc: arc-phase6-pouch
commits: ["*(pending)*"]
touched:
  - sub-kernel-thread
  - sub-kernel-proc
  - sub-kernel-sched
  - sub-kernel-syscall-dispatch
  - sub-kernel-death
  - inv-i32
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-20
---
The first chunk of the thread-model overhaul (`docs/thread-model-review.html`,
from the joint Opus/Fable review of 2026-10-20). Four of the review's findings,
none needing an operator decision. **F-1 (P1):** exec could swap the address
space while an EXITING peer was still on another CPU, whose tail then stored its
clear-child-tid word through the old translation (a freed page by a stale TLB
entry, or the new image by a fault); `proc_exec_replace` now waits until every
peer is off its CPU and frees them before the swap ([[sub-kernel-proc]]).
**F-6 (P3):** an IRQ return could switch out an exit tail on a kthread-shaped
path, losing a last thread's srv, cap and weft notifies; `preempt_check_irq` no
longer switches out a thread that is not RUNNING ([[sub-kernel-sched]]; the
preempt half of [[seam-exiting-tails-never-sleep]]). **F-8 (P3):** both spawn
arms refuse a dying Proc -EAGAIN ([[sub-kernel-syscall-dispatch]]). **F-2 (P2):**
the tid and pid counters saturate and refuse instead of extincting, and a spawn
on a spent tid space answers EAGAIN ([[sub-kernel-thread]], [[inv-i32]]). Each fix
has a kernel regression test that fails without it; F-1 has two, a fabricated
peer at any CPU count and a real one holding its exit tail on a second CPU.
Audit round 1 (Fable 5.1 on Opus 5.5): 0 P0 / 0 P1 / 0 P2 / 4 P3, all fixed.
