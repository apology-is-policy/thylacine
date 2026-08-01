---
id: lock-territory-ns-lock
type: lock
title: "Territory.ns_lock — the namespace-table guard"
kind: spin
orders-before: []
guards: "one Territory's mounts[]/nmounts, binds[]/nbinds, and root_spoor"
created: 2026-08-01
updated: 2026-08-01
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
`mount`'s MREPL arm, `unmount`, `territory_chroot`,
`territory_pivot_root`. `path_unref` is the deliberate exception: it is
refcount plus `kfree` with no close hook, non-sleeping, so it runs in
place (the `ns_lock -> slub c->lock` edge has no reverse).

`spoor_ref` and `path_ref` ARE legal inside — both are atomic and
non-sleeping — which is what lets the lookup-and-ref be one step.

Two inbound edges, both acyclic: `g_proc_table_lock -> ns_lock` (devproc
renders `/proc/<pid>/ns` inside a `proc_for_each` callback, which is
also what keeps the Territory alive — `territory_format_ns` takes the
lock for table STABILITY and relies on its caller for LIFETIME), and
`ns_lock -> slub c->lock`. Nothing taken under `ns_lock` ever reaches
back for either. Process-context only.

Deliberately separate from [[lock-territory-dot-lock]]: the cwd is
touched on a different, much hotter path and shares no field.
