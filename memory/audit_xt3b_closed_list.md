# XT-3b closed list (per-thread reaping + the EXITING preempt gate)

Surface: docs/AUDIT-TRIGGERS.md "thread_spawn / thread_exit / multi-thread
exit" (XT-3b addendum), plus the Scheduler row's XT-3b addendum for the
preempt gate. Round r1: cross-family reviewer (Fable 5.1) at max effort, on
`dec9cd55`, 2026-10-08. 0 P0 / 0 P1 / 0 P2 / 7 P3. Fixes in `3638a075`.
Do not re-report these.

## Fixed (r1, in 3638a075)

- F1 P3: proc_kstack_peak dipped between the detach hold and the later
  fold hold. The reap is now claim-then-commit: proc_claim_settled_retired
  marks Thread.reap_claimed under the lock and leaves the Thread on
  p->exited; the depth is measured lock-free; proc_commit_reaped folds
  run_ns + depth and unlinks in ONE hold; the free follows. Rounds of
  REAP_ROUND (16) until one comes back short. Test proc.thread_reap_gauges;
  model BUGGY_UNLINK_AT_CLAIM -> EveryThreadCounted.
- F2 P3: the tail was called non-preemptible, and it was not. The reviewer's
  premise that the EL0 fault path runs with IRQs on was wrong; that path is
  masked. The real hole is the userland_enter die-check, which runs with IRQs
  on and outside a syscall: an EXITING thread switched out there is lost,
  taking its clear_child_tid wake and the last-out /srv, /cap and weft
  cleanup with it (task #20, pre-existing). preempt_check_irq now refuses an
  EXITING thread and leaves need_resched pending. The bound and its premise
  are restated in proc.h, ARCH I-32 and the dossiers. Test
  scheduler.preempt_gate_defers_while_exiting.
- F3 P3: the drain's alone check uses proc_count_live_peers_locked, the same
  predicate as proc_exec_alone and the swap re-check.
- F4 P3: model gaps. The two-phase claim is modelled: Reapable needs
  claims = {}, and ReapFree folds, unlinks and frees. New in the model:
  EveryThreadCounted, ClaimsHeldByLive and the action property
  ClaimTakesAllSettled; the coarsenings and the per-CPU bound the model
  cannot state are named in its header. The seam
  seam-exiting-tails-never-sleep now names its four sites.
- F5 P3: test_sys_spawn_with_fds.c's #68 F2 rationale is reworded.
- F6 P3: syscall.h SYS_THREAD_EXIT lists the reap as step 1.
- F7 P3: proc_reap_retired and proc_drain_retired extinct on a bad Proc.

## Observations recorded, no code change

- The reap on the IRQ-masked die-check tails runs the kstack scan + frees
  under the mask. It is bounded per call by the retired set (about 2x
  ncpus Threads) and per round by REAP_ROUND; recorded, not a finding.
- The drain's yield loop has no counter; it has the same shape as the #788
  on_cpu spin, which also has no timeout.
- A Proc whose remaining Threads never spawn or exit keeps about one kstack
  per CPU pinned until one does; stated in ARCH I-32 and proc.h.

## Withdrawn in r1 (the guard that withdrew each)

W1 spawn handlers touch nt after ready() (tid captured before). W2 lock-free
walkers of p->threads / p->exited (none in production). W3 a tail sleeps
(no writable FILE VMA; spin-only arms). W4 reaper vs wait_pid (a live reaper
keeps the Proc ALIVE). W5 stale on_cpu false (RELAXED true-store before the
retire's release; destination RELEASE false; ACQUIRE load under the lock).
W6 EXITING re-dispatch (ready_on / wakeup / sched / thread_switch all
extinct or skip). W7 I-39 focus UAF (CAS at retire, readers under the lock).
W8 thread_free on a retired Thread (now extincts). W9 deadlock /
lock-across-sleep. W10 exec drain at -smp 1. W11 I-24 last-out uniqueness.
W12 I-9 death-wake (a retired Thread never sleeps). W13 proc_cpu_ns dips
(folded in the hold). W14 probe soundness. W15 vacuous tests.
