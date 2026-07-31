---
id: lock-dev9p-poll-glock
type: lock
title: "g_dev9p_poll_lock — the readiness-op registry lock"
kind: spin
orders-before: [lock-9p-client-c-lock]
guards: "the dev9p_poll op registry chain + count, ps->op, ps->wanted_mask (dev9p_poll_state's non-atomic fields)"
created: 2026-07-31
updated: 2026-07-31
---
Acquisition contexts: `dev9p_poll` (register/submit/widen), the kthread's
phase-1 sweep + collect, `dev9p_poll_priv_release` (the close grab).
Order edges: **g_lock → c->lock** (submit/abandon under it is forbidden —
submit takes c->lock inside `p9_client_submit_async` while g_lock is
held, which IS the sanctioned edge; the reverse never occurs) and
**g_lock → poll_list lock** (the GC empty-check nests for atomicity with
the unlink vs a concurrent reuse). NEVER held across a wakeup, a blocking
pump, a spoor/session unref, or the abandon's Tflush — all Phase-2 work
runs after the drop. Sleep-illegal under it (spinlock). The registry
count is atomic so the kthread's park cond reads it lock-free
(register-then-observe via the rendez lock).
