---
id: fnd-stube2-r1-f5
type: fnd
title: "Territory asserted its size but not its field offsets"
round: adt-stube2-r1
severity: P3
status: fixed
surface: [sub-kernel-territory]
threatens: []
fixed-by: chg-2026-05-21-p5-chroot
regression: "the _Static_assert set itself (compile-time)"
created: 2026-08-01
---
## Prosecution

Only `sizeof(struct Territory)` was pinned. A future field reorder that
preserved the total — swapping two same-width members, or moving
`root_spoor` past `binds[]` while adding compensating padding — would
compile clean and silently break every consumer that depends on WHERE a
field is: `sys_walk_open_handler`'s FROM_ROOT path, `territory_chroot`'s
ref discipline, and the mount-table iteration.

A size assert catches growth. It cannot catch rearrangement, which is
the change a reorder actually is.

## Disposition

Fixed: per-offset asserts on `root_spoor` (24), `binds` (32), and
`mounts` (after `binds`). The set has since grown with every field
added, and it is why LS-4's cwd pair and RW-4's `ns_lock` were both
APPENDED at the struct tail rather than placed where they read most
naturally — the tail-append discipline keeps every pinned offset stable,
and the asserts are what make a violation of it a build failure rather
than a runtime mystery.
