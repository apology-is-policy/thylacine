---
id: fnd-kt1-r1-a3
type: fnd
title: "a poller on `cq_waiters` can be woken with no CQE and `poll(2)` then returns 0 under an infinite timeout"
round: adt-kt1-r1
severity: P3
status: deferred
surface: [sub-kernel-loom, sub-kernel-poll]
threatens: [inv-i9]
regression: "seam-loom-sqpoll-p3s"
seam: seam-loom-sqpoll-p3s
created: 2026-09-05
---
## Prosecution

**File**: kernel/loom.c:1896-1902 (the flood-budget wake), :374 (the vacuous loom_free wake); kernel/poll.c:348-357 (post-wake re-sample), :382
**Invariant**: I-9 is NOT violated (no wake is lost); this is the converse -- a wake with nothing ready
**Prosecution**:
1. Thread A polls the loom fd with timeout -1 (halcyond) and is registered on `l->cq_waiters`.
2. A sibling ENTER caller on a NON-SQPOLL ring (or any ENTER on this ring in the non-SQPOLL arm) hits the flood budget: `poll_waiter_list_wake(&l->cq_waiters); break;` (loom.c:1901-1902) -- a Byzantine server flooding ownerless frames. A's flag is set with cq_ready still 0.
3. `tsleep` returns TSLEEP_AWOKEN; the re-scan yields `ready_count == 0`; `sys_poll_for_proc` returns 0 (poll.c:354-357, 382) -- "timeout" on a call that asked for no timeout. halcyond tolerates it (`< 0` is the only check, main.rs:835) and re-polls; a POSIX-shaped consumer treating 0 as "timed out" would misbehave.
4. Reachable only through a hostile/buggy 9P server on a shared client or through the defensive loom_free wake (vacuous while the held ref holds), so P3.
**Suggested fix**: in `sys_poll_for_proc`, on TSLEEP_AWOKEN with `ready_count == 0` and `timeout_ms < 0`, re-arm the hooks and sleep again (or document the spurious-return contract in poll.h and 72-poll.md).

## Disposition

Deferred (kernel; no kernel change in this close): the four SQPOLL-substrate P3s ride one follow-up chunk with its own SMP gate -- park on BUSY after measuring the yield-spin (A-F1), shorten/document the 10 ms re-arm (A-F2), re-sleep on a CQE-less wake under timeout -1 (A-F3), and a kernel test that drives `sys_poll_for_proc` against the KOBJ_LOOM arm with the keep_out loom-ref (A-F4). Owed at [[seam-loom-sqpoll-p3s]]; memory `bug_loom_poll_substrate_p3s.md`.
