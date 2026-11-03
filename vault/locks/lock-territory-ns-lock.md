---
id: lock-territory-ns-lock
type: lock
title: "Territory.ns_lock — the namespace-table guard"
kind: spin
orders-before: []
guards: "one Territory's mount table (points, entries, counts, visit marks), binds[]/nbinds, and root_spoor"
created: 2026-08-01
updated: 2026-10-19
---
## Discipline

A per-Territory NEAR-LEAF spinlock, appended at RW-4 SA-F1
([[fnd-rw4-sa-f1]]). Held for the table read-modify-write and NOTHING
else. Two hard rules define it:

**Never across `stalk`.** The resolver blocks on 9P; holding a spinlock
across it is a system-wide stall. This is why `mount_lookup` returns an
OWNED ref rather than a borrow — the lookup and its `spoor_ref` are
atomic under the lock, and the lock is released long before the caller
crosses. `territory_root_ref` is the identical pattern for
`root_spoor`, and is the only sound way to take a FROM_ROOT walk base
in a multi-thread Proc.

**Never across `spoor_clunk`.** A displaced or removed source's Dev
close hook may sleep. Every mutation site therefore CAPTURES the
outgoing Spoor under the lock and clunks it after release —
`mount`'s MREPL and reposition arms, `unmount`, `territory_chroot`,
`territory_pivot_root` (the old root and the shed). `path_unref` would be
safe in place (refcount plus `kfree`, no close hook), and was once run
there; since the dynamic table it is released with the entry.

**Never across an allocation or a free** (the dynamic table, ARCH 9.1;
[[sub-kernel-territory]]). `mount` allocates its two entries and its point
before taking the lock and frees what it did not use after it; every
edit chains what it unlinks on a `struct mount_dead`, which
`mount_dead_release` drops and frees after the unlock; `territory_clone`
allocates the child's nodes outside the parent's lock. So no slab call
runs under it.

`spoor_ref` and `path_ref` ARE legal inside — both are atomic and
non-sleeping — which is what lets the lookup-and-ref be one step.

**Never waited on with IRQs masked.** A shed holds this lock for
(members of the reached points) × (points) steps, about a million at
`PGRP_MOUNT_CAP`, and `pivot_root` lets any Proc repeat one on its own
Territory. So `territory_format_ns` asserts that IRQs are enabled. The
`g_proc_table_lock -> ns_lock` edge is gone (2026-10-19, audit round 1
F1): devproc rendered `/proc/<pid>/ns` inside a `proc_for_each` callback,
and every CPU queued on the IRQ-masked table lock waited out the shed with
it. devproc now takes a Territory ref under the table lock and renders
after dropping it, so the ref keeps the Territory alive (LIFETIME) and
this lock only keeps the table still (STABILITY). The
`ns_lock -> slub c->lock` edge that `path_unref` under the lock once made
went with the dynamic table, and nothing taken under `ns_lock` ever
reaches back for it. Process-context only.

Deliberately separate from [[lock-territory-dot-lock]]: the cwd is
touched on a different, much hotter path and shares no field.
