---
id: fnd-shed-r2-f1
type: fnd
title: "a union handle is a latent capability on the COVERED directory at its mount point: dissolve the union and '/' or '.' hands it over"
round: adt-shed-r2
severity: P2
status: fixed
surface: [sub-kernel-stalk, sub-kernel-territory]
threatens: [inv-i28]
fixed-by: chg-2026-09-21-mount-shed
regression: "stalk.union_dissolved_degrades (kernel; both handle classes, control legs while the union lives); usr/symlink-probe child stages union-a (chroot ONTO a union of /proc + /ctl over a Stratum directory, unmount('/') x2, open('/') must be member[0]) and union-b (hold the union dirfd, chroot into /proc, '.' must be member[0])"
created: 2026-09-21
---
## Prosecution

**File**: `kernel/stalk.c`, the zero-component arm: `zbase = (base->union_snap && base->union_snap->point) ? base->union_snap->point : base;`
**Invariant**: I-28
**Prosecution**:
1. VARIANT A (no shed needed; pre-existing): a Proc whose root is a union handle U (point P in the LAUNCHER's tree) calls `unmount("/")` until the union is empty -- `STALK_MOUNT`, zero components, key = P's identity; unmount is ungated.
2. `open("/")` -> `zbase = P`, no members, no cross -> an fd on the COVERED directory, outside the root's tree.
3. VARIANT B (new with the shed): a confined native child holding a union dirfd and any directory handle in an instance that cannot reach instance(P): `chroot(that handle)` sheds the union's own entries; `openat(ufd, ".")` -> the covered directory.
4. Blast radius: the raw subtree under P; DAC still applies. No in-tree union root or confined native consumer: reachable but undriven.
**Suggested fix**: use the point only while it still hosts a member, else the base -- a dissolved union degrades to member[0], never to the covered directory.

## Disposition

Fixed as suggested, as one sentence of scripture (ARCH 9.6.10). The base-set site probes `mount_member_at(point, 0)`; the zero-component site enforces the rule as a POST-condition of the cross, so a peer Thread's `unmount` opens no window between a check and the cross. `STALK_MOUNT` still keys the point. The first version of the fix was itself wrong for an OPENED union handle and was caught by its own kernel test before any gate: see [[fnd-b0self-r1-f1]].
