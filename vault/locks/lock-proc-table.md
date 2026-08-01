---
id: lock-proc-table
type: lock
title: "g_proc_table_lock — the Proc-lineage lock"
kind: spin-irqsave
guards: "children lists + sibling chains + parent pointers, ALIVE->ZOMBIE transitions, exit_status/exit_msg, the companion Thread's THREAD_EXITING commit, p->threads link/unlink + thread_count, sid/pgid, the PTY-1e report latches, the stop flags' set/clear walks, g_init_proc, and the three console-role pointers"
orders-before: []
created: 2026-08-01
updated: 2026-08-01
---
## Discipline

The one global lock of the execution area. File-static in `kernel/proc.c`
and exposed to `kernel/thread.c` only as `proc_table_lock_acquire` /
`proc_table_lock_release`, so its storage stays private.

`spin_lock_irqsave` **uniformly**. No IRQ handler mutates lineage state
today; the discipline is deliberate future-proofing against notes/signals
surfacing from IRQ context, and it is what lets the cascade run from the
console kthread.

**Order.** It sits at the "lineage" level — acquired before any wait/wake
lock, after none. The full chain:

```
g_proc_table_lock -> wait_lock -> g_timerwait.lock -> r->lock -> cs->lock
g_proc_table_lock -> torpor_lock -> g_timerwait.lock -> r->lock
g_proc_table_lock -> allowance->lock          (the I-34 revoke)
g_proc_table_lock -> q->lock                  (a note post)
g_proc_table_lock -> poll_waiter_list -> rendez
```

`torpor_lock` and `wait_lock` never nest (torpor drops its own before
`tsleep`). `smp_resched_others` and the IPI handler take no locks. The
reverse edge `rendez->lock -> g_proc_table_lock` is **forbidden** and since
#344 no path takes it: the old single-waiter `wait_pid_cond` read the
children list under `r->lock` without the table lock — the one inversion
candidate — leaning on a "single-writer children list" premise the
multi-thread-Proc lift falsified. That is precisely why the `wait_active`
guard had to *refuse* a second concurrent waiter. #344 dissolved it by
making the wait predicate read one flag and nothing else.

**Held across a wake, deliberately.** `proc_become_zombie_locked` wakes the
parent's `child_waiters` and posts the `child_exit` note **inside** the
critical section. Releasing first would admit the R5-H F75 race: between the
release and the wake, the parent can be reaped and freed by the
grandparent's `wait_pid`, and the wake touches freed memory. Holding it
pins the parent alive through the wake.

**Never held across a sleep.** The two places that need to sleep under an
otherwise-locked sequence drop it explicitly and re-take: the #68/#926 close
window (`unlock → proc_close_handles_at_exit → relock → recount-assert`) and
the clear-child-tid handoff, which runs *after* the release because
`uaccess_store_u32` may demand-page (`vma_lock` + buddy) and `torpor_wake`
takes `torpor_lock` — neither of which composes with this lock.

**Callback contract.** `proc_for_each` holds it across the whole DFS, so a
callback must not re-enter `proc_find_by_pid` / `rfork` / `exits` /
`wait_pid` / `proc_for_each`. Code already inside the lock uses the
`_walk` variants (`proc_for_each_walk`, `proc_find_by_pid_walk`) — calling
the locking form from a locked context deadlocks, which is why
`proc_legate_teardown_if_root` documents its precondition explicitly and
`el0_return_die_check`'s lockless tail correctly uses the *locking* form.
