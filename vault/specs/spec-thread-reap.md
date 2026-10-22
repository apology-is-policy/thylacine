---
id: spec-thread-reap
type: spec
title: "thread_reap.tla"
models: [sub-kernel-thread, sub-kernel-proc, sub-kernel-death]
pins: [inv-i24, inv-i32]
cfgs:
  - "thread_reap.cfg -- clean: Safety (TypeOk + LiveMatches + NoAdmitAfterClaim + CapExact + OneLastOut + NoOrphanedProc + CloseStable + ZombieImpliesAllExiting) + EventuallyReaped; four threads, cap 3"
  - "thread_reap_buggy_admit_claim.cfg -- the claim check and the link in two holds: NoAdmitAfterClaim violated"
  - "thread_reap_buggy_admit_cap.cfg -- the cap check and the link in two holds: CapExact violated"
  - "thread_reap_buggy_count.cfg -- the live-count read and the commit in two holds: NoOrphanedProc violated"
  - "thread_reap_buggy_spawner.cfg -- a thread admitted with no RUNNING spawner joins the last out's close: CloseStable violated"
  - "thread_reap_buggy_zombie.cfg -- ZOMBIE published before the last commit: ZombieImpliesAllExiting violated"
gate: "any change to thread admission, the EXITING commit, the last-out close, the group claim, or wait_pid_for's zombie check (specs/check-thread-reap.sh)"
created: 2026-10-21
updated: 2026-10-21
---
## Abstraction

Written **model-first** (thread-model C3a, 2026-10-21): spec-first is
re-enabled for the thread lifecycle by the operator's vote on the overhaul's
decision 1, which named C3's per-Proc thread lock as covered. The spec lands
before the code; C3b is written to it. C1 extends the same module with the
final switch-out, the reaper and the free, and the section-28 Thread
reclamation row lands with C1.

One Proc. Threads walk `unborn -> run -> exiting`, the last out through
`closing`. Every action is one critical section: admission and the non-last
commit hold the Proc's `threads_lock`; the claim holds `g_proc_table_lock` and
`threads_lock`; the close holds `g_proc_table_lock` with `threads_lock` nested.
The locks are not variables, because no correct critical section spans two
steps; each buggy cfg splits one section into two steps with the lock free
between them, which is exactly the shape of the bug it documents. The
`closing` pc is the last out's window with every lock dropped (handle close,
which sleeps); `CloseStable` is the premise that makes the drop safe.

Beneath the model: two Procs and the one-thread-lock-per-CPU rule (a
KERNEL_TESTS assert in the kernel), the death-wake walk (`spec-death-wake`),
the reap of exited peers and the free (C1), and the position of the
clear-child-tid store before the commit (a kernel test pins it).

## Action-site map

The table is in `specs/SPEC-TO-CODE.md` ("thread_reap.tla"). In short:
`Admit` is `proc_thread_admit` (both user spawn arms, after an unlinked
`thread_alloc_*`); `Claim` is `group_terminate`'s CAS inside the Proc's thread
lock; `ExitDecide` is `thread_exit_self` / `exits_term` through
`thread_commit_exiting_locked`, the only writer of `THREAD_EXITING`;
`CloseDone` is their last-out tail (`proc_release_territory_at_exit`, the
commit, `proc_become_zombie_locked` in one `g_proc_table_lock` hold);
`WaitPid` is `wait_pid_for`.
