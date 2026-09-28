---
id: fnd-b1d-v-r1-f3
type: fnd
title: "no invariant pinned the model's Emount guard, so territory_file_point.cfg checked nothing territory.cfg did not"
round: adt-b1d-v-r1
severity: P3
status: fixed
surface: [spec-territory]
threatens: []
fixed-by: chg-2026-09-25-b1d-v-emount
regression: "specs/territory_buggy_emount.cfg (NoMemberAtFile violated)"
created: 2026-09-25
---
## Prosecution

WIP 1 guarded `MountBefore` / `MountAfter` / `MountRepl` with `DirPoint`, so
no mount reached the file point `b`, and `NoCoveredFile` held vacuously: a
covered member at `b` needs a mount at `b`. No invariant said a file point
takes no mount, no buggy configuration removed the guard, and the one that
should have shown the covered-member conjunct failing (`BUGGY_COVER_FILE`)
could no longer reach a file point at all.

## Disposition

Fixed: the guard is split. `EmountOK` (the syscall's check) guards the three
mount actions, and `CovGuard` (`starts_union`'s QTDIR conjunct) guards the
covered member. `KERNEL_MOUNTS` models `mount()`'s kernel callers, which the
syscall check does not cover. The new invariant `NoMemberAtFile` fails under
`territory_buggy_emount.cfg`; `territory_buggy_cover_file.cfg` sets
`KERNEL_MOUNTS` and fails `NoCoveredFile` again; `territory_file_point_kernel`
is clean.
