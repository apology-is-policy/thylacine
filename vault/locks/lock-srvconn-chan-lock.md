---
id: lock-srvconn-chan-lock
type: lock
title: "srvconn_chan.lock — the per-direction ring lock (c2s / s2c)"
kind: spin
orders-before: []
guards: "One direction's ring state: count/head/tail, eof, and the reading/writing role flags. Two instances per SrvConn (c2s.lock, s2c.lock)."
created: 2026-07-31
updated: 2026-07-31
---
## Discipline

- **Plain `spin_lock`** (never irqsave — every path is syscall/kthread
  context; nothing touches a chan from IRQ).
- **Single-instance almost everywhere**: `chan_produce`,
  `chan_consume_nonblock`, `chan_set_eof`, `chan_role_acquire/release`,
  and both blocking-op loops each take exactly ONE direction's lock.
- **The dual-lock pair** — exactly two sites take both, in the FIXED
  order **c2s → s2c**: `srvconn_poll` (atomic sample+register across both
  directions) and `srvconn_teardown`'s EOF latch (both `eof` flags flip
  inside one dual-lock critical section so a concurrent poller can never
  observe POLLHUP without POLLERR). No other path takes both, so the
  nested acquire cannot deadlock.
- **Nests INSIDE it**: the `poll_waiter_list` lock only — at the two
  register sites (`srvconn_poll` registers on `cn->poll_list` under both
  chan locks; `chan_role_acquire` registers on `ch->role_waiters` under
  the chan lock). Register-then-observe requires the atomicity.
- **Never nests inside it**: any `Rendez` lock. Every `wakeup()` and
  every `poll_waiter_list_wake()` runs AFTER the chan lock is released
  (the mutate-then-wake discipline mirrored from `kernel/pipe.c`); the
  happens-before for the lockless cond reads (`chan_cond_readable` /
  `chan_cond_writable` / `role_cond_*`) is the rendez-lock acquisition
  inside `tsleep`/`wakeup`, not this lock.
- **Never held across a sleep** — both blocking loops release before
  `tsleep`.
- The sibling `SrvConn.lock` (guards only the LIVE→TORN `state` flip) is
  released before the chan locks are taken in teardown; the two have no
  ordering relation.

See [[sub-kernel-srvconn]] Concurrency for the full wait/wake protocol.
