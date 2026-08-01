---
id: chg-2026-05-31-809-exit-group
type: chg
title: "#809: SYS_EXIT_GROUP — the cross-thread shootdown (I-24)"
date: 2026-05-31
arc: arc-holotype-rw
commits: ["89456e9c"]
touched: [sub-kernel-death]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
no-dossier-change: "retro backfill -- sub-kernel-death is established in this same sweep commit"
---
## What

The flag-and-self-terminate group termination: a single set-once
`group_exit_msg` on the Proc, `torpor_wake_all_for_proc`, a broadcast
`smp_resched_others`, and the EL0-return die-check that turns the flag into
each Thread's own `thread_exit_self`. Realizes [[inv-i24]].

## Why

Before this, `exits()` with live peer Threads was a kernel EXTINCTION and
`kill` on a multi-thread Proc returned `-EIO`. Both were placeholders for a
mechanism that did not exist: there was no way to make a peer Thread stop.

Three lineages converge on the same answer — Plan 9, Linux and Zircon all
flag-and-self-terminate; seL4's synchronous stall was rejected. No Thread is
torn down from outside; the IPI is a latency accelerant, not a stop.

The design is deliberately ONE per-Proc pointer rather than the sketched
per-Thread `die_requested` + `group_exiting` + `group_exit_status` +
targeted IPIs: a NULL-sentinel CAS is simultaneously the die flag, the
last-out status source, and the idempotency guard.

## Verification

Retro record from `git log` and ARCH §7.9.1. Its own audit's F1 — that an
indefinite sleeper is never woken, so the Proc never reaps — was the
non-reaping HANG that [[chg-2026-06-01-811-death-interruptible]] closed;
F3 and F4 landed with it.
