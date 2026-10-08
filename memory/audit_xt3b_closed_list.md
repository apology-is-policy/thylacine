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

## Round r2 (Fable 5.1, start == end, max effort, on 3638a075): 0 P0 / 0 P1 / 0 P2 / 3 P3

The code of the r1 fixes was judged sound. Fixed in the round-2 commit:

- r2-F1 P3: the restated I-32 premise was too broad. A last Thread out can
  sleep in its close window, or run unmasked: the userland_enter die-check,
  or the spawn thunk's exits("fail-exec"). And a kernel-mode thread of a user
  Proc (in-kernel tests only) retires from a preemptible stretch. The premise
  is now a property of RETIRING exits: SYS_THREAD_EXIT, the masked die-checks,
  proc_fault_terminate. The last-out and kernel-mode exceptions are stated in
  proc.h, ARCH I-32, row 66 (f) and sub-kernel-death.
- r2-F2 P3: the model header called the REAP_ROUND coarsening a superset, and
  it was not. Rounds are now MODELLED: RoundMax, `nround`, the "reaploop" state,
  ClaimStart / ClaimNext / LoopEnd. At 4 Threads with RoundMax 1, two reapers
  split one settled set; NoSplitClaim was checked to fire, so the state is
  reachable. ClaimTakesAllSettled is replaced by the action property
  ReapEndsOnShortRound, plus the liveness ReapLoopEnds; both were checked to fire
  on sabotaged copies. Counts 858 / 5994. WF(Settle) is stated as an obligation
  on the C. The reviewer's sub-claim that a preempted tail "never settles"
  is wrong: the preempting sched() completes a switch and the destination
  clears on_cpu, so the tail settles with its work cut short. The gate is not
  part of WF(Settle); the never-sleeps seam is.
- r2-F3 P3: thread_exit_self's last-out ran the srv, cap and weft teardown
  AFTER its EXITING commit; exits() runs the same three while RUNNING and
  ALIVE. They now run in the last-out window (after the territory release,
  lock dropped, peer re-check), so on both paths the EXITING tail is the
  clear-child-tid handoff plus sched(). The seam note and the gate comments
  are updated. No regression test: none of the three sleeps today, so
  nothing fails without the move; it removes a dependency.

## Withdrawn in r2

W1 wait_pid's claimed-Thread extinction EL0-reachable. W2 the drain's alone
extinction EL0-reachable. W3 the gate making an EXITING thread spin or starve
(every EXITING write is followed by its own sched()). W4 a remote write of
EXITING. W5 the gate test's transient EXITING observed. W6 the gate test
vacuous or hanging. W7 the gauges test vacuous. W8 the lock-free kstack scan
vs a reader. W9 the commit's head-mismatch extinction. W10 the gauges
dipping. W11 the weft release sleeping (the share admission excludes FILE;
moot after r2-F3). W12 the tail's store reaching the FILE-miss arm. W13 the
REAP_ROUND arrays on a deep frame. W14 an endless reap loop. W15 a stale
in_syscall after execve. W16 the "EL0 fault path runs masked" premise.
W17 an orphan reap that skips p->exited.
