---
id: chg-2026-05-23-torpor
type: chg
title: "P6 sub-chunk 8: torpor — the futex, prose-validated"
date: 2026-05-23
arc: arc-pouch-boot
commits: ["e2455471"]
touched: [sub-kernel-torpor]
established: [sub-kernel-torpor]
closed: []
opened: [seam-torpor-lock-wake-spin, seam-torpor-cross-proc]
mirrors-checked: []
depth: rich
---
## What

Wait-on-address over a global-lock bucket table: compare under
`torpor_lock`, register, tsleep on a private stack rendez; wake
walks the bucket under the same lock. The chunk that BROADENED the
spec-to-code suspension: no `futex.tla` was written — the
[[inv-i9]] specialization is validated by the lock acquire/release
prose proof in `torpor.h`, with the embedded audit
([[adt-torpor8-r1]], 0/0/2/10) as the rigor floor.

## The two P2s

[[fnd-torpor8-r1-f1]] — WAKE's count said "matched and marked", not
"actually woken"; a waiter whose deadline had already fired was
counted. Fixed: count `wakeup()`'s truth.

[[fnd-torpor8-r1-f2]] — `torpor_lock` held across `wakeup()`'s
`on_cpu` spin. Documented, not fixed; still open as
[[seam-torpor-lock-wake-spin]].

Of the ten P3s, the ones that shaped the surface: F4 (the
`p == current->proc` extinction assert — the TTBR0 argument), F7
(`WAKE(addr, 0)` is no barrier), F10 (the alignment check is
load-bearing for the uaccess fault class).
