---
id: chg-2026-10-12-group-status-first-claim
type: chg
title: "A dying group keeps its status; a page-in its death abandoned is that death"
date: 2026-10-12
arc: arc-boosty
commits: ["b0bd631a0", "c855c2dd3", "59381c434", "d82c0d14c", "479d1218b", "f4d6b1152", "8337ea211"]
touched:
  - sub-kernel-death
  - sub-kernel-fault
  - sub-kernel-exception
  - sub-kernel-notes
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-10-12
---
Taken by main from B-2c's `ls-jsc` (OPEN-BUGS 2026-10-08 17:15Z), which paused
behind it. After jsc printed its result and exited, the console showed
`9p: op abandoned (tag 0, death, flush sent)` and a `user fault:
reason="snare:bus"` at an instruction fetch in jsc's demand-paged text.

**What** ([[sub-kernel-death]], [[sub-kernel-fault]], [[sub-kernel-exception]],
[[sub-kernel-notes]]). jsc's exit claimed the group's death; a peer thread's 9P
text read was abandoned by that death; the pager answered `FAULT_USER_BUS`;
`proc_fault_terminate` called `exits("snare:bus")`; and `exits_code`'s
last-thread path, when that thread was the last out, recorded `snare:bus`/1
over the claimed status (I-24). Two
halves fix it. `exits_code` finds `group_exit_msg` under `g_proc_table_lock`, in
the hold that marks `PROC_FLAG_EXIT_CLOSING`, and leaves through
`thread_exit_self`, so the first claim's status is the one reaped (Linux's
`do_group_exit` precedent). `userland_demand_page` answers the new
`FAULT_USER_DYING` for an unresolved user fault while the faulting thread's
death is pending, sampled before and after the page-in. The EL0 arm returns to
the vector tail for a group death and acts at the fault for a terminate latch
(`notes_fault_dying_latch`): exit with the note the tail would pick, or clear
latches no queued note still backs and re-execute.

**Why the latch is acted on at the fault.** The tail defers every note but
`kill` while a handler runs (N-3) and while `SP_EL0` is unusable; returning
there re-executed the faulting instruction into the same fault forever (audit
r1 F1). A latch whose note is live but masked by the faulting thread stays
armed for its peers; the faulting thread's own die-pending reads false for a
masked family, so its re-executed fault is reported as the fault it is.

**Found on the way, owned elsewhere.** ci-smp-gate's default-smp1 row failed L285
(a CLONE_THREAD poller's `ppoll` returned -EINTR) 1 boot in 40. Measured
deterministic, and identical on main a7353c36f: ARCH 8.8.3's caught-note claim
gives a child's SIGCHLD to whichever sleeper claims first, where Linux and
FreeBSD give it to the running main thread. The probe now re-issues an
interrupted `ppoll` (479d1218b); the kernel half is an operator vote (OPEN-BUGS
2026-10-12 07:24Z). A phenotype `rt_sigaction` handler install still leaves an
armed latch on the sleep side (OPEN-BUGS 2026-10-12 06:43Z).

**Verification.** Audits by Fable 5.1 (start == end each round): r1 0/1/0/4 (F1 the latch livelock), r2 0/0/0/3 (scripture fold, the live test reads the exit message, a closed-list record). On the stacked tip 512383b31 (groupstatus + SQ-1): suite 1975/1975; test-fault 8/8; ci-smp-gate N=10 50/50 (default smp1/4/8, UBSan smp4/8, no corruption); the production shape (KERNEL_TESTS off) builds, boots and logs in. groupstatus alone at f4d6b1152: suite 1972/1972. REDs: removing the fault-arm call -> L329/L330 "hung"; breaking the helper -> the three new kernel tests fail (1969/1972); breaking only the stale clear -> the stale test alone (1971/1972); exiting with snare:segv -> only the live test's message assertion (1971/1972). L285: the forced construction failed every boot before the probe fix (r=-4, identical on main a7353c36f) and passes 2/2 after it, with a leg proving the retry ran. death_wake.tla clean (1422 distinct, EventuallyReaps) and its buggy cfg violates NoLostDeathWake; the model covers the cascade and the reap, not which status the ZOMBIE records, which rests on the two kernel witnesses.
