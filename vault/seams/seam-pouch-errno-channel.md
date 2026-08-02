---
id: seam-pouch-errno-channel
type: seam
title: "The flat-`-1` error convention gives every kernel failure `EIO`"
status: open
surface: [sub-pouch-seam, sub-pouch-fs, sub-pouch-net]
opened-by: chg-2026-05-22-p6-syscall-seam
tracker: "POUCH-DESIGN.md 5.1"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

Thylacine's syscall convention is a flat `-1` with no errno channel, so
`__syscall_ret` maps every failure of a 1:1 seam call to the generic
`EIO`. Code that branches on a specific errno after `read` / `write` /
`close` never gets it. The same collapse forces every lower-half wrapper
to GUESS: `bind` answers `EACCES` for post-gate / name-taken /
registry-full / table-full / bad-name alike; `connect` answers
`ECONNREFUSED` for all of its causes; `fstat` answers `EBADF` for any
non-zero return; `posix_spawn` answers `ENOENT`; `kill` loses `ESRCH`
vs `EPERM`.

`EIO` is a documented POSIX errno, so P-3 holds — this is imprecision,
not silent wrongness.

## The lift

A richer kernel error channel (`-errno` in `[-4095,-2]`, which the decode
ALREADY passes through — the stalk-resolved calls use it since ER-1).
Every call migrated to real `-errno` retires one guess. The decode needs
no change; the kernel-side handlers do.
