---
id: fnd-kt1-r1-a2
type: fnd
title: "an SQE queued while the kthread is in its 10 ms boundary recv is not consumed until that recv returns -- up to `LOOM_SQPOLL_IDLE_NS` of re-arm latency per event read, invisible to the design docs"
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

**File**: kernel/loom.c:2056-2059 (SQPOLL ENTER = clear NEED_WAKEUP + `wakeup(&l->sqpoll_park)` only), :2166 (`loom_drain_sq` at loop top), :2195-2196 (the recv); usr/lib/libtapestry/src/lib.rs:1163-1178 (`pump`)
**Invariant**: none violated (latency characteristic); relevant to the HALCYON 14.11.7 latency claim
**Prosecution**:
1. After halcyond reaps a KEY, the console slot is unarmed; the next `poll_event` -> `pump(false)` -> `arm_all` -> `try_submit` (sq_tail++) -> `enter(n,0,0)`.
2. On an SQPOLL ring `loom_enter` only wakes `sqpoll_park` (loom.c:2058-2059). The kthread is NOT parked -- it is blocked in `srvconn_client_recv` (tsleep on the s2c rendez, srvconn.c:647-651) with the 10 ms deadline. Nothing wakes that rendez but a frame or the deadline.
3. The SQE sits in the SQ until the kthread's loop top runs `loom_drain_sq` (loom.c:2166) -- i.e. after the next frame on the session or after the 10 ms lapse. Until then tapestryd has no outstanding read for the surface and holds its events server-side (bounded by the 128-event cap + WEDGE retire; a 1 kHz pointer flood is 10 events per window, so no wedge).
4. Net: every event delivery on the ring carries up to 10 ms of re-arm jitter under SQPOLL, whereas the old blocking `wait` submitted inline (`loom_drain_sq` in `loom_enter`'s non-SQPOLL arm). The scripture's "byte-driven, not frame-driven" claim is true for the consdrain leg and only approximately true (10 ms-driven) for the ring leg.
**Suggested fix**: document the bound in 107-loom.md + HALCYON 14.11.7a; consider a shorter `LOOM_SQPOLL_IDLE_NS` for rings with a pending SQE, or an ENTER-side kick that wakes the transport's recv rendez at a frame boundary (a "submission pending" nudge the boundary recv honours as IDLE). An inline drain from ENTER is NOT the fix: R4-F1's park-cond premise (`sq_head` single-writer, loom.c:2128-2129) forbids it.

## Disposition

Deferred (kernel; no kernel change in this close): the four SQPOLL-substrate P3s ride one follow-up chunk with its own SMP gate -- park on BUSY after measuring the yield-spin (A-F1), shorten/document the 10 ms re-arm (A-F2), re-sleep on a CQE-less wake under timeout -1 (A-F3), and a kernel test that drives `sys_poll_for_proc` against the KOBJ_LOOM arm with the keep_out loom-ref (A-F4). Owed at [[seam-loom-sqpoll-p3s]]; memory `bug_loom_poll_substrate_p3s.md`.
