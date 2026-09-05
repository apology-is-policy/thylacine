---
id: lock-srv-registry-lock
type: lock
title: "SrvRegistry.lock — the /srv service-registry lock"
kind: spin-irqsave
orders-before: []
guards: "All 16 SrvService entries of one registry: state machine (FREE/RESERVING/LIVE/TOMBSTONED), name, poster identity, mode + ring_msize, and every accept-backlog ring (backlog[]/head/tail/count)."
created: 2026-07-31
updated: 2026-07-31
---
## Discipline

- **Near-leaf, irqsave** (`spin_lock_irqsave` at every site). Nothing
  nests inside it except a `poll_waiter_list` register
  (`svc_listener_poll` registers on `svc->poll_list` under the registry
  lock — the atomic sample+register step).
- **Heavy work runs OUTSIDE**: the tombstone/drain paths
  (`srv_proc_exit_notify_in`, `srv_registry_drain`) collect SrvConn
  pointers and Rendez/poll-list addresses under the lock, release, and
  only then run `srvconn_teardown`/`srvconn_unref` (which take the
  SrvConn's own locks) and the wakes. `srvconn_*` never re-enters the
  registry lock, so the chains are acyclic.
- **Wakes after release**: `wakeup(&svc->accept_rendez)` and
  `poll_waiter_list_wake(&svc->poll_list)` always follow the unlock (the
  `chan_produce` discipline).
- **Reached two ways**: through the registry pointer (a devsrv root
  Spoor's `aux`) or through the permanent `svc->reg` back-pointer
  (stamped once at `srv_registry_create`, never cleared — the svc-taking
  API's route).
- The lockless cond read in `accept_cond_is_ready` (backlog_count/state
  without this lock) is sound by the rendez-lock happens-before: every
  producer mutates under this lock then wakes.
- **Drain stack cost**: `srv_registry_drain` carries
  `SRV_MAX_SERVICES × SRV_ACCEPT_BACKLOG` pointers on the kernel stack
  (~2 KiB at 16×16) — the recorded brake on raising `SRV_MAX_SERVICES`.

See [[sub-kernel-devsrv]] Concurrency.
