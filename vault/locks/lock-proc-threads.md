---
id: lock-proc-threads
type: lock
title: "p->threads_lock — one Proc's threads"
kind: spin-irqsave
guards: "p->threads + links, thread_count (written under it; lockless atomic readers stay), live_threads, loom_sqpoll_count, every THREAD_EXITING commit, debug_focus_thread, the per-Proc walks (death wake, interrupt and caught-note walks, stop wake, debug resume, cancel steps), PROC_FLAG_EXIT_CLOSING at the last-out decision; with g_proc_table_lock, the group claim (group_exit_msg + code + signal + EXIT_CLOSE_FORCED)"
orders-before: []
created: 2026-10-22
updated: 2026-10-22
---
## Discipline

Each Proc's own lock for its threads (ARCH 7.3.1, thread-model C3), taken
through `proc_threads_lock` / `proc_threads_unlock`, which mask IRQs first. It
nests under [[lock-proc-table]] and above the wait/wake locks:

```
g_proc_table_lock -> p->threads_lock -> wait_lock -> g_timerwait.lock -> r->lock -> cs->lock
p->threads_lock -> torpor_lock
```

There is no edge to or from a note queue's `q->lock`: the kill re-enqueue drops
`q->lock` before it takes this lock to read the live count.

**One per CPU.** No path holds two Procs' thread locks: work that reaches
other Procs (the parent's wake, the orphan rule, a session hangup, a legate
teardown, the process-group fans, the kill walk) runs under
`g_proc_table_lock` and takes each target's thread lock in turn, after
releasing its own. Under KERNEL_TESTS a per-CPU slot names the held lock: a
second acquire, a release by a non-holder, and a `_locked` walker without its
lock (`ASSERT_TL_HELD`) each extinct; `proc.threads_lock_slot` pins the slot.

**A `Thread *` read from `p->threads` is valid only while it is held.** Every
unlink holds it and precedes the free, so a pointer matched under the lock is a
live struct. Exec's off-CPU wait re-resolves its peers every round instead of
keeping a pointer across the unlock. The one exception is `wait_pid_for`'s free
walk of a zombie that is already off the process tree.

**`live_threads`** counts linked threads that have not committed: +1 at the
link, -1 at `thread_commit_exiting_locked` (the only writer of
`THREAD_EXITING`), -1 at an unlink of a thread that never committed. The last
out reads 1. Under KERNEL_TESTS every commit cross-checks it against the walk.

**Never held across a sleep, the global lock or a waiting IPI.** The last
thread out decides under it, drops it to close the handles, and commits under
it again nested in `g_proc_table_lock`; the ZOMBIE publish runs outside it
because it reaches other Procs' thread locks.

Validated by `specs/thread_reap.tla` ([[spec-thread-reap]]).
