---
id: chg-2026-05-21-p5-chroot
type: chg
title: "P5-stratumd-stub-bringup-e2: SYS_CHROOT + Territory.root_spoor + walk-from-root"
date: 2026-05-21
arc: arc-phase5-namespace
commits: ["d1e012c7", "bcd6feaa", "1ae21e5c"]
touched:
  - sub-kernel-territory
  - sub-kernel-ninep-attach
established: []
closed: [fnd-stube2-r1-f5]
opened: []
mirrors-checked: []
depth: skeletal
---
`Territory.root_spoor` + `territory_chroot` + `SYS_CHROOT = 35`, and the
`spoor_fd == -1` FROM_ROOT sentinel that makes "walk from my root" a
resolvable thing. The refcount discipline mirrors MREPL exactly —
`spoor_ref` BEFORE the swap (so a corrupted source, which extincts in
`spoor_ref`, leaves `root_spoor` untouched), `spoor_clunk` the displaced
root AFTER (so its Dev close hook runs if this Territory was the last
holder), idempotent on same-pointer. `territory.tla::Chroot` models it
with a two-key EXCEPT, and `MountRefcountConsistency` extends to include
the `root_spoor` contribution.

The round ([[adt-stube2-r1]]) returned 1 P0 / 1 P1 / 2 P2 / 3 P3 across
the combined e1+e2 surface. The P0 and P1 were on the walk and 9P-attach
side; the territory-facing one is [[fnd-stube2-r1-f5]] — only
`sizeof(struct Territory)` was asserted, so a field reorder preserving
the total would have silently broken the FROM_ROOT path and the mount
iteration. The per-offset assert set landed there and is the reason both
later additions (LS-4's cwd fields, RW-4's `ns_lock`) were APPENDED at
the tail.

joey deliberately does NOT chroot during bringup at this point: it never
exits during boot, so an in-flight chroot would hold the attach's
transport Spoors alive past its `t_close`, the stratumd stub would never
see EOF, and the reap would deadlock. The chroot path is proven by a
child probe whose exit releases it, plus six kernel tests. That
constraint is what `SYS_PIVOT_ROOT` later exists to lift.
