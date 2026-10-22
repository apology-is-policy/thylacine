---
id: inv-i48
type: inv
title: "I-48 — thread reclamation"
number: I-48
guards: [sub-kernel-thread, sub-kernel-sched, sub-kernel-proc, sub-kernel-death, sub-kernel-exec]
validated-by: [spec-thread-reap, gate-smp]
strength: spec
created: 2026-10-22
updated: 2026-10-22
---
## Statement

A thread's context and kernel stack are freed exactly once, only after its
last switch-out has completed, and only by its reaper. A user thread's reaper
is the resume path of that switch, a kernel thread's is its joiner, and a
thread that never ran is freed by its creator. No address space is swapped
(exec) or freed (the Proc's free) while a thread of its Proc is on a CPU or not
yet freed. An exited thread not yet freed is always either on its CPU or in the
hand of the resume path that switched it out. So a Proc's `thread_count`
exceeds its `live_threads` by at most the CPU count, and no exited thread
waits for a spawn, an exec or a wait.

Adopted spec-first by operator vote on 2026-10-21 (thread-model decision 1),
specified in C1a and built in C1b. ARCH 7.3.2 describes the mechanism. It
replaces the 2026-10-19 interim fix, in which exited threads waited for the
next spawn (`seam-exited-threads-wait-for-a-create`).

## Enforcement

- Both `sched()` resume tails: `sched()` after `cpu_switch_context`, and
  `sched_finish_task_switch` for the trampolines. Each decides (EXITING, not
  `g_kproc`) BEFORE the RELEASE clear of `on_cpu`, then calls
  `thread_reap_dead` after the run-queue lock is released, with interrupts
  still masked.
- `thread_reap_dead`: in one `threads_lock` hold it folds the run time and
  stack depth, clears the debug focus and unlinks the thread; it unlocks, then
  frees the stack and the slot.
- `wait_pid_for` reads the zombie's `thread_count` under its `threads_lock`
  before `proc_free`. `proc_exec_replace` waits for `thread_count == 1` and
  re-checks it at the swap. `proc_free` keeps its drain checks.
- `thread_free`'s magic, state and `on_cpu` gates guard the joiner and creator
  paths.

## Validation

`spec-thread-reap` (C1 actions): `NoFreeOnCpu`, `ExactlyOnce`,
`NoSwapWhileOnCpu`, `ProcFreedLast` and `NoParkedDead` hold, as does the
liveness property `ExitedThreadsFreed`. Five buggy cfgs each fail as named:
a free on EXITING alone, a second reaper, exec once the caller is the only live
thread, `wait_pid` reading the count without the lock, and the interim fix's
spawn-time sweep. Kernel tests and the SMP gate land with C1b.
**blind-to:** the model has one Proc and no CPUs. That the reaper runs before
its CPU runs anything else, and that interrupts stay masked across it, is the
resume path's code, not a modelled action. Kernel threads and their joiners
are outside the model. Any holder of a thread pointer that is not under the
Proc's `threads_lock` and not in a state the thread cannot leave (C1's holder
census) is a use-after-free the model cannot see.
