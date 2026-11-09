---
id: fnd-noexec-r1-f2
type: fnd
round: adt-noexec-r1
severity: P1
status: fixed
title: "The Pouch witness could not pass where joey runs it"
surface: [sub-pouch-process]
threatens: []
fixed-by: chg-2026-11-04-noexec-r1
regression: "pouch-hello-fs posix_spawn(noimg)"
created: 2026-11-04
---
## Prosecution

joey runs pouch-hello-spawn pre-pivot, on read-only devramfs with no /tmp, so the create failed and the boot gate would report FAILED. Independently Pouch has no execve (`__NR_execve 0xFFFF`), so `execv` reads ENOSYS, never ENOEXEC.

## Disposition

The legs moved to pouch-hello-fs, which runs post-pivot on the writable pool, spawn half only. L369 and L370 now take bundle-supplied fixtures, so a refused create cannot read as anything.
