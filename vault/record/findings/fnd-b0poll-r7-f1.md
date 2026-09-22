---
id: fnd-b0poll-r7-f1
type: fnd
title: "The point's 'one pass' bound is attacker-inflatable: nothing caps hooks per poll_waiter_list, and the producer's wake walk crosses no point"
round: adt-b0poll-r7
severity: P2
status: deferred
surface: [sub-kernel-poll]
threatens: [inv-i9, inv-i27, inv-i32]
regression: "seam-poll-hooks-per-list"
created: 2026-09-22
---

## Prosecution

The point bounds the NUMBER of masked spans a poller takes, not the LENGTH of
one. Nothing caps how many hooks a single `poll_waiter_list` can hold -- 64 per
call times `PROC_THREAD_MAX` times the number of Procs -- so both the
producer's `poll_waiter_list_wake` walk and the poller's per-pass unregister
walks are O(attacker-scaled) and run IRQ-masked. The producer's walk crosses no
preemption point at all, because it runs in the waker's context. An I-27
latency claim (the SAK is served promptly) now rests on those walks.

Re-classified from the round-4 F8 / round-5 F6 perf items: the same code, but
what rests on it changed.

## Disposition

TRACKED, with ARCH 23.3's claim qualified honestly rather than left standing.
Recorded in `docs/browser-status.md` with three candidate fixes: the
per-endpoint/event-keyed lists of round-4 F8, a per-walk wake cap with the
remainder deferred, and an I-32 axis capping hooks per list. Its own chunk.
