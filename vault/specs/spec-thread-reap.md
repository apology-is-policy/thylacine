---
id: spec-thread-reap
type: spec
title: "thread_reap.tla"
models: [sub-kernel-thread, sub-kernel-proc, sub-kernel-death]
pins: [inv-i24, inv-i32, inv-i48]
cfgs:
  - "thread_reap.cfg -- clean: Safety (TypeOk + LiveMatches + NoAdmitAfterClaim + CapExact + OneLastOut + NoOrphanedProc + CloseStable + ZombieImpliesAllExiting + NoFreeOnCpu + ExactlyOnce + NoSwapWhileOnCpu + ProcFreedLast + NoParkedDead) + EventuallyReaped + ExitedThreadsFreed; four threads, cap 3; 3528 distinct states"
  - "thread_reap_buggy_admit_claim.cfg -- the claim check and the link in two holds: NoAdmitAfterClaim violated"
  - "thread_reap_buggy_admit_cap.cfg -- the cap check and the link in two holds: CapExact violated"
  - "thread_reap_buggy_count.cfg -- the live-count read and the commit in two holds: NoOrphanedProc violated"
  - "thread_reap_buggy_spawner.cfg -- a thread admitted with no RUNNING spawner joins the last out's close: CloseStable violated"
  - "thread_reap_buggy_zombie.cfg -- ZOMBIE published before the last commit: ZombieImpliesAllExiting violated"
  - "thread_reap_buggy_free_on_cpu.cfg -- a reaper frees on THREAD_EXITING alone, before the final switch-out (the #788 shape): NoFreeOnCpu violated"
  - "thread_reap_buggy_two_reapers.cfg -- an old reaper (the spawn sweep, exec's reap, wait_pid's walk) left beside the resume path: ExactlyOnce violated"
  - "thread_reap_buggy_exec_early.cfg -- exec swaps once it is the only LIVE thread while a committed peer still runs its tail (F-1): NoSwapWhileOnCpu violated"
  - "thread_reap_buggy_waitpid_unlocked.cfg -- wait_pid reads the thread count with a bare load and frees the Proc under the last reaper's unlock: ProcFreedLast violated"
  - "thread_reap_buggy_reap_at_spawn.cfg -- the 10-19 interim fix (only a later spawn frees the dead): the temporal property ExitedThreadsFreed violated"
gate: "any change to thread admission, the EXITING commit, the last-out close, the group claim, the final switch-out and its reaper, exec's wait for its dead peers, or wait_pid_for's zombie wait (specs/check-thread-reap.sh)"
created: 2026-10-21
updated: 2026-10-22
---
## Abstraction

Written **model-first** (thread-model C3a, 2026-10-21): spec-first is
re-enabled for the thread lifecycle by the operator's vote on the overhaul's
decision 1, which named C3's per-Proc thread lock as covered. The spec lands
before the code; C3b is written to it. C1a (2026-10-22) extends the same module,
again before its code, with the final switch-out, the reaper and the free,
exec's wait for its dead peers and `wait_pid` freeing only the Proc, and the
section-28 row I-48 (`inv-i48`) lands with it.

One Proc. Threads walk `unborn -> run -> exiting -> dead -> unlock -> gone`,
the last out through `closing`. `exiting` is committed and still on its CPU
(`oncpu`); `dead` is switched out and held by the resume path that did it
(`inhand`); `unlock` is the reaper between its unlink and its unlock, the only
span in the model where a lock (`tlheld`) is held across two steps, because
that unlock is the reaper's last touch of the Proc and `wait_pid` must not free
the Proc before it. Every action is one critical section: admission and the non-last
commit hold the Proc's `threads_lock`; the claim holds `g_proc_table_lock` and
`threads_lock`; the close holds `g_proc_table_lock` with `threads_lock` nested.
The locks are not variables, because no correct critical section spans two
steps; each buggy cfg splits one section into two steps with the lock free
between them, which is exactly the shape of the bug it documents. The
`closing` pc is the last out's window with every lock dropped (handle close,
which sleeps); `CloseStable` is the premise that makes the drop safe.

Beneath the model: two Procs and the one-thread-lock-per-CPU rule (a
KERNEL_TESTS assert in the kernel), the death-wake walk (`spec-death-wake`),
CPUs (the reaper running before its CPU runs anything else, interrupts masked,
is the resume path's code), kernel threads and their joiners, and the
clear-child-tid store,
which follows the commit and must not sleep (`demand_page_locked` refuses the
file page-in to a thread that is not RUNNING; a kernel test pins it).

## Action-site map

The table is in `specs/SPEC-TO-CODE.md` ("thread_reap.tla"). In short:
`Admit` is `proc_thread_admit` (both user spawn arms, after an unlinked
`thread_alloc_*`); `Claim` is `group_terminate`'s CAS inside the Proc's thread
lock; `ExitDecide` is `thread_exit_self` / `exits_term` through
`thread_commit_exiting_locked`, the only writer of `THREAD_EXITING`;
`CloseDone` is their last-out tail (`proc_release_territory_at_exit`, the
commit, `proc_become_zombie_locked` in one `g_proc_table_lock` hold);
`WaitPid` is `wait_pid_for`, which since C1 frees only the Proc;
`SwitchOut` / `ReapUnlink` / `ReapUnlock` are the two `sched()` resume tails and
`thread_reap_dead`; `Exec` is `proc_exec_replace`'s wait for `thread_count == 1`.
