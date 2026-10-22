---
id: seam-exiting-tails-never-sleep
type: seam
title: "EXITING tails must never SLEEP — a property held by accident, not enforcement"
status: open
surface: [sub-kernel-death]
opened-by: chg-2026-07-14-68-last-thread-out-close
tracker: "unfiled"
created: 2026-08-01
updated: 2026-10-22
---
## Owed

The #68 close window's soundness rests on a stated property: an EXITING
peer's residual execution never touches the handle table and never SLEEPS.
The second half is what is owed, because it is currently true by
COINCIDENCE rather than by construction.

An EXITING thread's remaining work is the clear-child-tid handoff plus
`sched()`. The handoff does `uaccess_store_u32` into a user VA — which
COULD demand-page. Sleeping while EXITING trips `sched()`'s "current is not
RUNNING" assertion, i.e. extincts.

It does not happen today because every writable VMA is eager or lazy-anon
(the lazy arm resolves fully under `vma_lock`, without blocking) and FILE
Burrows are never writable — the REVENANT dispatch gate keeps `PF_W`
segments eager.

## What closes it

Either an explicit non-sleeping guarantee on the exit-tail uaccess path, or
an enforcement that catches the violation instead of relying on the mapping
taxonomy staying as it is.

The v1.x anon-COW / pageout work is the trigger: the moment a writable
mapping can fault into a BLOCKING arm, this property must be
re-established deliberately, before that work lands.

## The preempt half (enforced 2026-10-20)

The same property has a second half this seam did not record: a tail must not
be switched out before its own `sched()`. On a kthread-shaped path
(`userland_enter`'s die-check, an exec failure's exit) the tail runs with
interrupts on and neither the preempt count nor the in-syscall marker set, so an
interrupt return could switch it out, and an EXITING thread is never requeued:
a last thread out lost its srv, cap and weft notifies. `preempt_check_irq` now
returns without switching for a thread that is not RUNNING ([[sub-kernel-sched]];
regression `proc.exit_tail_runs_to_its_end`). The sleep half stays owed.

Two dependents since 2026-10-22: exec waits until every peer has been freed
before it swaps the address space, and `wait_pid` waits until the zombie's last
thread has been freed before it frees the Proc ([[sub-kernel-proc]]). Each
thread is freed by the resume path of its last switch-out ([[inv-i48]]), which
frees only a thread it switched away from EXITING. A tail that slept would
leave its CPU SLEEPING and never be freed: exec would wait forever, and
`wait_pid`'s check that every remaining thread is EXITING would extinct.

## The sleep half (refused at the one sleeping arm, 2026-10-21)

Thread-model C3 kept the clear-child-tid store after the `THREAD_EXITING`
commit (exec's sole-thread check relies on a joiner that reads the zeroed word
finding the worker EXITING) and enforced the sleep half where it can break
today: `demand_page_locked`'s FILE miss, the only fault arm that sleeps,
refuses a current thread that is not RUNNING before it pins anything
([[sub-kernel-fault]]; regression `demand_page.file_miss_refuses_exiting_thread`).
The store then fails as on an unmapped page.

What stays owed: a refused store skips the joiner's wake, so the program hangs
where the kernel used to extinct. No store can reach the arm today (FILE
mappings are never writable). The first writable private FILE mapping, or any
new blocking arm, must therefore either fault the clear-child-tid page in at
`SYS_SET_TID_ADDRESS` (and keep it resident), or move the store before the
commit together with exec's check; and a new blocking arm must add the same
refusal.

## Risk while open

None today. The failure mode when it breaks is an extinction on the exit
path — loud, but on the death lineage, which is where loud failures have
historically been hardest to attribute.
