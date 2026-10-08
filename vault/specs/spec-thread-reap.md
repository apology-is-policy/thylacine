---
id: spec-thread-reap
type: spec
title: "thread_reap.tla"
models: [sub-kernel-death, sub-kernel-thread, sub-kernel-proc]
pins: [inv-i32, inv-i24]
cfgs:
  - "thread_reap.cfg -- clean, 3 Threads: Safety + EventuallyFreed / ExecCompletes / ClaimsDischarged (668 distinct states)"
  - "thread_reap_4.cfg -- clean, 4 Threads: the same (4532 distinct states)"
  - "thread_reap_buggy_no_oncpu.cfg -- BUGGY_REAP_IGNORES_ONCPU: NoFreeInFlight violated"
  - "thread_reap_buggy_unlocked_claim.cfg -- BUGGY_CLAIM_UNLOCKED: OneFreerPerThread violated"
  - "thread_reap_buggy_exec_no_drain.cfg -- BUGGY_EXEC_NO_DRAIN (task #19): TailsOnLiveSpace violated"
  - "thread_reap_buggy_waitpid_skips_retired.cfg -- BUGGY_WAITPID_SKIPS_RETIRED: TailsOnLiveSpace violated"
  - "thread_reap_buggy_tid_after_ready.cfg -- BUGGY_TID_AFTER_READY: NoTidReadAfterFree violated"
gate: "specs/check-thread-reap.sh ALL CFGS AS CLAIMED, on any change to the retire, a reap point, proc_drain_retired, wait_pid's free of the zombie's lists, or a spawn handler's use of the new Thread after ready()"
created: 2026-10-08
updated: 2026-10-08
---
## Abstraction

Written **model-first** for XT-3b (per-thread reaping,
`docs/X86-TRANSLATION-DESIGN.md` 5.8 XT-K9), before the C, because freeing an
exited Thread while its Proc lives has four actors that can each race the free:
the dying Thread's own tail, live peers reaping at their spawn and exit, exec's
drain, and the parent's `wait_pid`. Before XT-3b only `wait_pid` freed a user
Proc's Threads, after the whole Proc was gone, which is why nothing modelled this.

One Proc, a set of Threads (one alive at the start, the rest spawnable once each),
and its address-space generations (an exec frees one and moves to the next). Each
Thread walks `unborn` -> `live` -> `tail` -> `settled` -> `freed`, with the live
sub-states a reaper, a spawner or an execer occupies mid-operation. Which of the
Proc's two lists holds a Thread (`live` / `retired` / `none`) and who has claimed
it (`claims`) are explicit, because "who may free this Thread" IS the theorem: a
free is legal only for a claimer, and only of a settled Thread.

**Deliberately beneath the model:**

- the memory itself: a Thread's struct and kstack are one `freed` state, so the
  model proves who frees and when, not that the C frees the right bytes (the
  kernel tests and `/thread-torture` are that witness);
- the walkers of the live list (the death-wake cascade, devproc, `proc_cpu_ns`):
  each runs entirely under `g_proc_table_lock`, and a Thread is freed only after a
  lock hold has detached it, so a walker's whole use of a Thread is one atomic
  step that sees it allocated; a walker that keeps the pointer across the lock
  drop is exactly the `BUGGY_TID_AFTER_READY` shape;
- the wake protocol of a group termination ([[spec-death-wake]]) and the on_cpu
  handoff inside the scheduler ([[spec-sched-alpha]]): `Settle` is the single
  step that publishes the destination CPU's RELEASE clear;
- the per-Proc totals (`reaped_run_ns`, the kstack peak): bookkeeping, argued in
  [[sub-kernel-death]], witnessed by `proc.thread_reap_churn`.

## Action-site map

| Action | Site |
|---|---|
| `Exit(t)` | `thread_exit_self` -- `t->state = THREAD_EXITING` and, with a live peer, `proc_retire_locked` in the same `g_proc_table_lock` hold |
| `Settle(t)` | the outgoing Thread's `on_cpu` RELEASE clear in `sched()`'s resume path / `sched_finish_task_switch` |
| `Reap(r)` / `ReapFree` / `ReapDone` | `proc_reap_retired` -> `proc_detach_settled_retired` (detach under the lock) + `proc_free_retired_chain` (free lock-dropped) |
| `Exec(e)` / `ExecFree` / `ExecSwap` | `proc_exec_replace` -> `proc_drain_retired`, then the swap and `addrspace_unref(old)` |
| `WaitPid` / `WaitFree` / `ProcFree` | `wait_pid_for` -- `zombie->exited` taken in the `proc_unlink_child` hold; `thread_free_retired` spins each; `proc_free` |
| `Spawn(s, n)` / `ReadTid(s)` | `sys_thread_spawn_handler` and the vivarium clone thread arm -- the tid read before `ready(nt)` |

## The bugs it names

Each buggy cfg is built to trip exactly one named invariant, so the checker can
judge it by name:

- **`BUGGY_REAP_IGNORES_ONCPU`** -> `NoFreeInFlight`: a reaper that detaches a
  retired Thread still switching away frees it under its own tail.
- **`BUGGY_CLAIM_UNLOCKED`** -> `OneFreerPerThread`: chosen under the lock but
  detached after it, one Thread is claimed by two reapers.
- **`BUGGY_EXEC_NO_DRAIN`** -> `TailsOnLiveSpace`: task #19, the pre-XT-3b exec,
  which freed the old address space with a retired tail still storing into it.
- **`BUGGY_WAITPID_SKIPS_RETIRED`** -> `TailsOnLiveSpace`: `wait_pid` frees the
  live list only, and the Proc's space goes under a tail no peer reached.
- **`BUGGY_TID_AFTER_READY`** -> `NoTidReadAfterFree`: the spawner's `return
  nt->tid` after `ready()`, which a peer's reap can now make a use-after-free.
