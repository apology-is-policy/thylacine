---
id: fnd-kt1-r1-a4
type: fnd
title: "no deterministic test drives the NEW code -- `poll_scan_one`'s KOBJ_LOOM arm with the RW-2 2C-F1 keep_out loom-ref retention"
round: adt-kt1-r1
severity: P3
status: deferred
surface: [sub-kernel-loom, sub-kernel-poll]
threatens: []
regression: "seam-loom-sqpoll-p3s"
seam: seam-loom-sqpoll-p3s
created: 2026-09-05
---
## Prosecution

**File**: kernel/test/test_loom.c:726-758 (`test_loom_poll`), usr/loom-smoke/src/main.rs:97-129 (leg 5); kernel/poll.c:238-244, :265-269
**Invariant**: I-7 / I-9 witness coverage
**Prosecution**:
1. `test_loom_poll` calls `loom_poll` directly (white-box). It pins register -> post -> flag -> re-poll POLLIN and the POLLOUT non-registration -- the ordering the race depends on -- but never enters `sys_poll_for_proc`, so the only new lines in poll.c (the arm + the `held[]` retention of a `loom_ref` and its post-sweep `handle_put`) have no kernel unit witness. `kernel/test/test_poll.c` has no KOBJ_LOOM mention (grep).
2. loom-smoke leg 5 exercises the real syscall path end to end, with a true negative control (idle ring, 200 ms, rc 0). Its positive leg races the kthread: the CQE is either posted before the first scan (fast path) or after (the tsleep path) per run -- it cannot deterministically construct "completion between the sample and the park", and it cannot construct the sibling-close-during-poll case (single-threaded).
3. A regression that dropped the `*keep_out = hh` transfer for KOBJ_LOOM (freeing the Loom under a listed waiter when a sibling closes the fd) would pass both witnesses.
**Suggested fix**: a kernel test: `sys_loom_setup_for_proc(p, 8, LOOM_SETUP_SQPOLL)`, `sys_poll_for_proc(timeout 0)` -> 0; stage a NOP + ENTER-kick + the sched-yield loop until `hdr->cq_tail >= 1`; `sys_poll_for_proc(timeout 100)` -> 1 with POLLIN; assert `l->refcount` returns to its pre-poll value after each call (the retention is balanced). The sibling-close interleaving joins the deferred two-thread SMP harness (#907).

## Disposition

Deferred (kernel; no kernel change in this close): the four SQPOLL-substrate P3s ride one follow-up chunk with its own SMP gate -- park on BUSY after measuring the yield-spin (A-F1), shorten/document the 10 ms re-arm (A-F2), re-sleep on a CQE-less wake under timeout -1 (A-F3), and a kernel test that drives `sys_poll_for_proc` against the KOBJ_LOOM arm with the keep_out loom-ref (A-F4). Owed at [[seam-loom-sqpoll-p3s]]; memory `bug_loom_poll_substrate_p3s.md`.
