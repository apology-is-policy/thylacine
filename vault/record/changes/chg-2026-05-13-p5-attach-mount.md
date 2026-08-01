---
id: chg-2026-05-13-p5-attach-mount
type: chg
title: "P5-attach-mount: the Territory mount table + the spec extension"
date: 2026-05-13
arc: arc-phase5-namespace
commits: ["c21d0023"]
touched:
  - sub-kernel-territory
established: []
closed: []
opened: [seam-union-mount-walk, seam-rfnameg-shared-territory]
mirrors-checked: []
depth: skeletal
---
The mount table is born: `PgrpMount` (source Spoor + abstract
`path_id_t` target + Plan 9 flags), `mount`/`unmount` with the
one-refcount-per-entry discipline, `territory_clone` deep-copying it,
and `territory_unref`'s final release dropping every entry — the
realization of the user-signed-off scripture "every filesystem entity is
a Spoor, and mount grafts one at a path".

The substantive half is the SPEC extension, which is what makes the
refcount discipline checkable rather than asserted. `territory.tla`
grows `mounts` (per-Proc set) and — crucially — `refcount` as a
SEPARATE variable, so "forgot to bump" and "forgot to drop" surface as a
desync from cardinality instead of being true by construction.
`MountRefcountConsistency` plus three new buggy cfgs
(`BUGGY_MOUNT_NO_REFBUMP`, `BUGGY_UNMOUNT_NO_REFDROP`,
`BUGGY_DESTROY_LEAK`) are the miss-one-site regression net that still
gates this surface. `ForkClone` extends to bump per cloned entry.

The user-visible `SYS_MOUNT` is DEFERRED here for a hard reason: v1.0
had no open/close/read/write/dup syscalls, so userspace had no way to
hold a `KOBJ_SPOOR` to mount. Tests and the future boot path call the C
API with a Spoor pointer directly. Both the abstract `path_id_t` target
and the union-mount flags land unimplemented — the first superseded by
stalk-2's Spoor-identity re-key, the second still owed
([[seam-union-mount-walk]]).
