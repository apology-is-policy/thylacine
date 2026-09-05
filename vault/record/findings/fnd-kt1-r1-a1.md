---
id: fnd-kt1-r1-a1
type: fnd
title: "the SQPOLL kthread's `P9_PUMP_BUSY` yield-spin is now the production steady state (the SA-2 premise 'no EL0 SQPOLL consumer' no longer holds)"
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

**File**: kernel/loom.c:2192-2206; kernel/9p_client.c:1205-1215 (`P9_PUMP_BUSY`), :1247 (`client_handoff_reader_locked(c, NULL)`), :519-560 (client_wait self-election)
**Invariant**: I-32 (resource floor -- CPU), not a correctness invariant
**Prosecution**:
1. halcyond's ring is SQPOLL (`EventRing::connect_sqpoll`, lib.rs:991-1004) and always has >= 1 armed event read, so the kthread never parks: it loops `p9_client_reader_pump_once_deadline(cl, now + 10 ms)` (loom.c:2195-2196).
2. The main thread makes SYNCHRONOUS RPCs on the SAME session every frame (`Surface::present` -> `t_write(self.present_fd)` lib.rs:787; ctl writes; pane-tree reads). In `client_wait` it self-elects whenever `!c->reader_active` (9p_client.c:551-556) -- the window between two kthread pumps -- AND it is HANDED the role whenever the kthread's boundary recv lapses at the 10 ms deadline while the sync rpc is in flight (`client_handoff_reader_locked(c, NULL)` at 9p_client.c:1247 picks the first sync inflight rpc, sets `be_reader`, wakes it). A present whose Rwrite takes T ms hits that lapse with probability ~T/10 ms.
3. Once the main thread holds the role, every kthread iteration returns `P9_PUMP_BUSY` (9p_client.c:1209: `if (c->reader_active) { ...; return P9_PUMP_BUSY; }`) and the loop does `if (rc == P9_PUMP_BUSY) sched();` (loom.c:2205) -> `loom_reap_terminal` -> loop top -> `loom_drain_sq` + `loom_rearm_pending` + `loom_admit_chain` + a `spoor_ref`/`spoor_clunk` pair + one `c->lock` acquire -> BUSY -> `sched()` ... a yield-spin contending `l->lock` and `c->lock` for the whole remaining RPC duration. On SMP it occupies a core (sched() returns at once with nothing else runnable); on a slow compose (tens of ms) it burns most of a core per frame exactly when tapestryd needs CPU.
4. Correctness is untouched (the reply lands, the role is released, the kthread resumes), so this is not a soundness violation -- but the Loom-4d disposition that classified it P3 rested on "v1.0 has no userspace Loom consumer" (107-loom.md caveats; loom.c:1712), and 15796866/a85c94e4 make the console renderer that consumer.
**Suggested fix**: on BUSY, park instead of yielding: register a poll_waiter on the client's existing reader-progress list (`c->send_waiters_list`, already woken on reader departure by `client_send_progress_signal`, 9p_client.c:259-262 and at every role release) with a bounded deadline, so the kthread sleeps until the sync reader departs. Measure kthread CPU time in the SMP gate with a present-heavy scenario before deciding.

## Disposition

Deferred (kernel; no kernel change in this close): the four SQPOLL-substrate P3s ride one follow-up chunk with its own SMP gate -- park on BUSY after measuring the yield-spin (A-F1), shorten/document the 10 ms re-arm (A-F2), re-sleep on a CQE-less wake under timeout -1 (A-F3), and a kernel test that drives `sys_poll_for_proc` against the KOBJ_LOOM arm with the keep_out loom-ref (A-F4). Owed at [[seam-loom-sqpoll-p3s]]; memory `bug_loom_poll_substrate_p3s.md`.
