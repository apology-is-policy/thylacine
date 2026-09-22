---
id: fnd-shed-r1-f1
type: fnd
title: "chroot / pivot INTO A UNION DIRECTORY sheds the union's own live entries; every root-based resolution then fails"
round: adt-shed-r1
severity: P1
status: fixed
surface: [sub-kernel-territory, sub-kernel-spoor]
threatens: [inv-i28]
fixed-by: chg-2026-09-21-mount-shed
regression: "territory.shed_union_root_keeps_point_entries (with its control one variable away: the same root identity with no snap sheds everything) + territory.shed_full_table_boundary (2 seeds + 32 chained sources lands exactly on reach[]); specs/territory_shed_buggy_no_union_seed.cfg violates ShedLosesNothing"
created: 2026-09-21
---
## Prosecution

**File**: `kernel/territory.c` (the closure seed); consumers `kernel/stalk.c` (`union_base = base->union_snap->point`, `stalk_union_child`, the zero-component path)
**Invariant**: I-28 / ARCH 9.6.10 `ShedLosesNothing`; the row's own bar that the shed "must NEVER drop a reachable one"
**Prosecution**:
1. The closure is seeded from the root's own instance only: `shed_add(reach, &nr, root->dc, root->devno);`.
2. The resolver consults one more Spoor on every resolution from that root: `if (base->union_snap && base->union_snap->point) { union_base = base->union_snap->point; ... }`. `point` is the union's mount point, which lives in the tree the union was mounted in, not in member[0]'s.
3. An O_PATH open of a union yields exactly such a Spoor (identity member[0], the point in the snap), and `sys_lookup_spoor` hands `territory_chroot` that same object.
4. In the default image `/bin` is a union: member[0] the devramfs root, the point the Stratum `/bin` directory. `t_chroot(open("/bin", O_PATH))` gives `R = {('m',0)}`; no entry is keyed there, so the point's instance never enters `R` and both `/bin` entries are shed with every other host entry.
5. Every lookup then goes to `stalk_union_child` -> `mount_members_snapshot(point)` -> 0 members -> ENOENT; `open("/")` returns the covered Stratum directory, and a create or unlink at `/` lands there.
6. Before the shed the same chroot worked. No in-tree caller does it; a union-composed rootfs is the natural Plan 9 layering idiom. Damage stays in the caller's own Territory.
**Suggested fix**: seed `R` with the instance of `root->union_snap->point`; grow `reach[]` by one.

## Disposition

Fixed, scripture first: ARCH 9.6.10 names the point's instance as a second seed; `reach[]` is `PGRP_MAX_MOUNTS + 2` with an overflow extinction and a test landing exactly on the bound. The snap is set once before the Spoor is published and freed with it, so the read under `ns_lock` needs only the root ref the caller holds. `spoor.h`'s "ONLY `spoor_readdir_run` consults it" -- the stale comment that hid the dependency -- corrected, and [[sub-kernel-spoor]] gained the field its struct listing never had. "What does the resolver consult at the base without walking to it" is prosecution item (8) of the audit row.
