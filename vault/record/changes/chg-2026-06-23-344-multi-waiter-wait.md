---
id: chg-2026-06-23-344-multi-waiter-wait
type: chg
title: "#344: multi-waiter wait_pid_for — retire the guard that refused the second waiter"
date: 2026-06-23
arc: arc-go-build
commits: ["86f085f9"]
touched: [sub-kernel-proc]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
no-dossier-change: "retro backfill -- sub-kernel-proc is established in this same sweep commit"
---
## What

Any number of a Proc's Threads may sit in `wait_pid_for` concurrently. Each
registers its OWN stack `poll_waiter` on the parent's `child_waiters` list
and parks on its OWN private rendez; a child entering ZOMBIE wakes them all;
exactly one reaps each zombie and the losers re-scan. The single-waiter
`Rendez child_done` and the `wait_active` guard are both retired.

## Why

The old design could not survive a second concurrent same-Proc waiter — a
second sleeper trips `sleep`'s single-waiter assert, an unprivileged
EXTINCTION. So the RW-2 guard had to **refuse** the second caller with `-1`,
and that refusal is what broke multi-threaded Go's parallel `go build`
(#342): a second goroutine-thread's waitpid simply got `-1`.

The lift also dissolves a lock hazard rather than managing it. The old
`wait_pid_cond` read the children list under `r->lock` WITHOUT the table
lock — the one `rendez → proc_table_lock` inversion candidate — leaning on a
"single-writer children list" premise the multi-thread lift had already
falsified. The new predicate reads ONE flag (`pw->ready`) and touches no
lineage state, so it adds nothing to the lock order. The authoritative
"is a matching zombie reapable" decision moves to a re-scan under the table
lock, making `ready` a pure wake relay.

`struct poll_waiter_list` is byte-identical in layout to the `Rendez` it
replaced, so the field swap kept every following `struct Proc` offset stable
— the old field survives as `_reserved0` for exactly that reason.

## Verification

Retro record from `git log` + the unusually complete rationale block above
the lock declaration in `kernel/proc.c`. Regression:
`proc.wait_pid_concurrent_waiters_both_reap`.
