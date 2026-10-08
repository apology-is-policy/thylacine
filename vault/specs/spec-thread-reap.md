---
id: spec-thread-reap
type: spec
title: "thread_reap.tla"
models: [sub-kernel-death, sub-kernel-thread, sub-kernel-proc]
pins: [inv-i32, inv-i24]
cfgs:
  - "thread_reap.cfg -- clean, 3 Threads, RoundMax 1: Safety + EventuallyFreed / ExecCompletes / ClaimsDischarged / ReapLoopEnds + the action property ReapEndsOnShortRound (858 distinct states)"
  - "thread_reap_4.cfg -- clean, 4 Threads, RoundMax 1 (two reapers split one settled set): the same (5994 distinct states)"
  - "thread_reap_buggy_no_oncpu.cfg -- BUGGY_REAP_IGNORES_ONCPU: NoFreeInFlight violated"
  - "thread_reap_buggy_unlocked_claim.cfg -- BUGGY_CLAIM_UNLOCKED: OneFreerPerThread violated"
  - "thread_reap_buggy_unlink_at_claim.cfg -- BUGGY_UNLINK_AT_CLAIM (audit F1): EveryThreadCounted violated"
  - "thread_reap_buggy_exec_no_drain.cfg -- BUGGY_EXEC_NO_DRAIN (task #19): TailsOnLiveSpace violated"
  - "thread_reap_buggy_waitpid_skips_retired.cfg -- BUGGY_WAITPID_SKIPS_RETIRED: TailsOnLiveSpace violated"
  - "thread_reap_buggy_tid_after_ready.cfg -- BUGGY_TID_AFTER_READY: NoTidReadAfterFree violated"
gate: "specs/check-thread-reap.sh ALL CFGS AS CLAIMED, on any change to the retire, a reap point, the claim or the commit, proc_drain_retired, wait_pid's free of the zombie's lists, or a spawn handler's use of the new Thread after ready()"
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
free is legal only for a claimer, and only of a settled Thread. A claim marks a
Thread and leaves it on the retired list; the step that frees it also unlinks it
and records it in `folded`, the Proc's totals, so `EveryThreadCounted` can say no
reading of those totals loses a Thread while the Proc can be read.

**Rounds** are modelled, not abstracted (audit round 2 F2): a round claims
`Min(RoundMax, |reapable|)` of the reapable Threads, a free choice standing for
the list order, and the reaper loops (`reaploop`) until a round comes back
short, as `proc_reap_retired`'s `while (n == REAP_ROUND)` does. At four Threads
and `RoundMax = 1` two reapers split one settled set, a state checked to be
reachable, so every safety invariant holds over split claims.
`ReapEndsOnShortRound` is the loop's exit condition, `ReapLoopEnds` its
liveness.

**Coarsenings**, each a superset of the C's behaviours or argued equal, and
stated in the module header: an exit need not reap first (the C always does);
exec claims every retired Thread at once and frees each once settled (the C
claims settled ones round by round, indistinguishable while the execer is
alone); one step folds, unlinks and frees (the C folds and unlinks a round in
one hold and frees after it, when no other actor can reach the Thread).

**What it cannot state:** the bound on retired-but-allocated Threads, about twice
the CPU count per Proc. It rests on a per-CPU stretch from a Thread's reap to its
settle that is never switched out ([[sub-kernel-death]]), and the model has no
CPUs; it lets every Thread sit in its tail at once. `ReapEndsOnShortRound` is the
half it can see: a reap ends only after a round that took every reapable
Thread. The churn test's `retired_max` and `/thread-torture` witness the bound
at runtime.

**An obligation, not a result:** `WF(Settle)`, every tail reaching a switch
away. It holds because a tail never sleeps ([[seam-exiting-tails-never-sleep]])
and spins only on bounded waits. A tail preempted early settles too, its work
cut short; that lost wake (task #20, closed by `preempt_check_irq`'s EXITING
refusal) is below this model, which has no joiners.

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
- the values of the per-Proc totals (`reaped_run_ns`, the kstack peak): only
  WHETHER a Thread is counted is modelled (`folded`), witnessed by
  `proc.thread_reap_gauges` and `proc.thread_reap_churn`.

## Action-site map

| Action | Site |
|---|---|
| `Exit(t)` | `thread_exit_self` -- `t->state = THREAD_EXITING` and, with a live peer, `proc_retire_locked` in the same `g_proc_table_lock` hold |
| `Settle(t)` | the outgoing Thread's `on_cpu` RELEASE clear in `sched()`'s resume path / `sched_finish_task_switch` |
| `ClaimStart(r)` / `ClaimNext(r)` / `ReapFree` / `ReapDone` / `LoopEnd(r)` | `proc_reap_retired`'s round loop -> `proc_claim_settled_retired` (the `reap_claimed` mark under the lock, at most `REAP_ROUND`; the Thread stays listed) + `proc_commit_reaped` (the fold and the unlink in one hold, the free lock-dropped) |
| `Exec(e)` / `ExecFree` / `ExecSwap` | `proc_exec_replace` -> `proc_drain_retired`, then the swap and `addrspace_unref(old)` |
| `WaitPid` / `WaitFree` / `ProcFree` | `wait_pid_for` -- `zombie->exited` taken in the `proc_unlink_child` hold; `thread_free_retired` spins each; `proc_free` |
| `Spawn(s, n)` / `ReadTid(s)` | `sys_thread_spawn_handler` and the vivarium clone thread arm -- the tid read before `ready(nt)` |

## The bugs it names

Each buggy cfg is built to trip exactly one named invariant, so the checker can
judge it by name:

- **`BUGGY_REAP_IGNORES_ONCPU`** -> `NoFreeInFlight`: a reaper that detaches a
  retired Thread still switching away frees it under its own tail.
- **`BUGGY_CLAIM_UNLOCKED`** -> `OneFreerPerThread`: chosen under the lock but
  marked after it, one Thread is claimed by two reapers.
- **`BUGGY_UNLINK_AT_CLAIM`** -> `EveryThreadCounted`: audit F1, the first shape
  of the C. The claim unlinked the Thread and its stack depth was folded in a
  later hold, so a `/ctl/kstack` reading between the two lost it.
- **`BUGGY_EXEC_NO_DRAIN`** -> `TailsOnLiveSpace`: task #19, the pre-XT-3b exec,
  which freed the old address space with a retired tail still storing into it.
- **`BUGGY_WAITPID_SKIPS_RETIRED`** -> `TailsOnLiveSpace`: `wait_pid` frees the
  live list only, and the Proc's space goes under a tail no peer reached.
- **`BUGGY_TID_AFTER_READY`** -> `NoTidReadAfterFree`: the spawner's `return
  nt->tid` after `ready()`, which a peer's reap can now make a use-after-free.
