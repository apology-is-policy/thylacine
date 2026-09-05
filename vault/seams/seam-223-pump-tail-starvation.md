---
id: seam-223-pump-tail-starvation
type: seam
title: "dev9p.poll pump: >16 distinct QTPOLL clients starve the tail"
status: open
surface: [sub-kernel-ninep-dev9p-poll]
opened-by: fnd-net6b-r2-f1
tracker: "task #223"
created: 2026-07-31
updated: 2026-07-31
---
**Owed**: a fair per-client work-queue (or a round-robin cursor) in
`dev9p_poll_collect_clients`. The collect is a head-anchored LIFO scan
bounded at `DEV9P_POLL_MAX_PUMP` (16): with MORE simultaneous QTPOLL
clients each holding a perpetually-parked op, the cap is NOT graceful —
the 16 nearest the head are pumped every cycle and a TAIL client is
STARVED outright (its reply never demuxed; its pollers hang), not merely
delayed.

**Why open is tolerable**: v1.0 has exactly ONE QTPOLL client (the single
netd `/net` mount); the per-user-netd v1.x config has a handful — far
below the cap.

**What closes it**: the v1.x per-client work-queue with a FAIR START (the
in-code note forbids re-using the head-anchored scan).

**Risk while open**: none reachable below 17 clients.
