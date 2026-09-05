---
id: seam-torpor-lock-wake-spin
type: seam
title: "torpor_lock held across wakeup()'s on_cpu spin"
status: open
surface: [sub-kernel-torpor]
opened-by: chg-2026-05-23-torpor
tracker: "torpor-8 audit F2 (P2, documented)"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

All three wake walks (per-VA, death, stop) hold the ONE global
`torpor_lock` across `wakeup()`, which can spin on the woken thread's
`on_cpu` while a peer CPU switches it out. Under heavy multi-Proc
futex contention every torpor operation system-wide serializes behind
that spin.

Dormant so far — but the workload that ends the dormancy is exactly
the one the tree now runs (multi-M Go programs hammering futexes).

## The lift

Either (a) per-bucket locks — which must preserve the property that
the death walk and a registering sleeper still serialize on a common
lock per bucket ([[inv-i24]]'s closure) — or (b) two-pass wake:
mark + collect under the lock, dispatch the wakeups after the drop
(each waiter is pinned by its bucket link until its own unlink, so
the collected list stays valid).
