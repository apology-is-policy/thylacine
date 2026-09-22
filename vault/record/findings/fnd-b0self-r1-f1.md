---
id: fnd-b0self-r1-f1
type: fnd
title: "the dissolved-union degrade cloned the OPENED handle, which a 9P server refuses to walk: '.' of a dissolved union fd was an I/O error on exactly the Devs that matter"
round: adt-b0self-r1
severity: P1
status: fixed
surface: [sub-kernel-stalk]
threatens: [inv-i28]
fixed-by: chg-2026-09-21-mount-shed
regression: "stalk.union_dissolved_degrades (the fixture refuses to walk an opened Spoor because Stratum's h_walk does); sabotage `walkable` fails it at 'dissolved: . still resolves'"
created: 2026-09-21
---
## Prosecution

**File**: `kernel/stalk.c`, the `zero_from_point` degrade arm (`quarry = clone_walk_zero(base)`) and the depth-0 `parent = base`
**Invariant**: I-28 (the rule must hold on every Dev, not only the ones the probe used)
**Prosecution**:
1. A `STALK_OPEN` union handle is member[0] OPENED (`COPEN`).
2. 9P forbids a `Twalk` from an opened fid, the zero-element clone included (Stratum `h_walk`: `is_open` -> EINVAL); the kernel-test fixture refuses what Stratum refuses.
3. While the union lives this never shows: every resolution leaves through the point.
4. Dissolved, `"."` and every name off the handle walked the opened Spoor -> NULL. Measured: 1594/1595 on the unsabotaged kernel at 5807bd9f.
5. The on-device probe passed, because its members are `/proc` and `/ctl` -- kernel Devs that walk an opened Spoor.

## Disposition

Fixed @237ba793: both dissolved sites resolve from `stalk_union_handle_walkable(base)`, the UNOPENED clone of that member which the full snap already retains for the readdir dedup probe, matched by identity and never by index (the snap skips a member it could not open). An `O_PATH` handle is itself unopened and is used as is. `STALK_MOUNT` still keys the point. No new shed seed: the matched walkable carries the handle's own `(dc, devno)`.
