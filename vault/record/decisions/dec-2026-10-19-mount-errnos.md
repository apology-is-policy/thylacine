---
id: dec-2026-10-19-mount-errnos
type: dec
title: "SYS_MOUNT and SYS_UNMOUNT answer an errno for each refusal"
date: 2026-10-19
status: standing
decided-by: user-vote
affects: [sub-kernel-syscall-dispatch, sub-kernel-syscall-abi, sub-kernel-territory, sub-utopia-eval, sub-libthyla-rs]
created: 2026-10-19
---
## Fork

`SYS_MOUNT` answers `-ENOTDIR` for a source whose type differs from the
point's ([[dec-2026-09-25-mrepl-only-at-a-file]]) and a bare -1 for
every other refusal: a point that does not resolve, a source handle that
is not live or lacks read, flags outside the set or naming two
placements, a mount that would close a cycle, and a full table.
`SYS_UNMOUNT` answers -1 for everything. When ls-libs' seventh overlay
failed on 2026-10-19, `bind` printed `cannot bind at <point>` with no
cause, and telling a full table from an unresolvable point took a device
run with a discriminator written for the purpose.

## Research

- **Plan 9 (heritage).** `mount(2)` and `bind(2)` return -1 and set an
  error string naming the cause (`Emount` for an inconsistent mount, for
  one), which the shell prints. There are no errnos.
- **Linux.** `mount(2)` answers the path's resolution errno (`ENOENT`,
  `EACCES`, `ENOTDIR`, `ELOOP`), `EINVAL` for flags it does not accept,
  `ELOOP` for a move into the source's own subtree, and `ENOSPC` past
  `fs.mount-max`; the descriptor-based `move_mount(2)` answers `EBADF`
  for a bad descriptor. `umount2(2)` answers `EINVAL` for a target that
  is not a mount point.
- **The tree.** ERRORS.md keeps a bare -1 for a truly unclassifiable
  failure, and ER-3 replaces the local -1 sites as each handler is
  touched. The path checks of the errno-aware path handlers
  (`SYS_OPEN_CREATE`: an empty or over-long path `EINVAL`, a bad user
  buffer `EFAULT`, an embedded NUL `EINVAL`) and `stalk_err`'s
  resolution errno are there to reuse. `ut`'s `bind` and `mount` already
  print an errno whenever the kernel answers something other than -1;
  `unmount` prints none. libthyla-rs maps `SYS_MOUNT`'s -1 to
  `InvalidArgument` and every `SYS_UNMOUNT` failure to `NotFound`.
  `ENOSPC` has no `T_E_*` name: a server's crosses by value.

## Options

1. **An errno per refusal**, Linux's answer for each cause.
2. **Keep the bare -1.** A discriminator stays the only way to tell the
   causes apart.

## The call

Option 1 (operator, 2026-10-19). `SYS_MOUNT` answers the point's
resolution errno as `stalk_err` reports it, after the path checks that
`SYS_OPEN_CREATE` makes; `EINVAL` for flags outside the set or naming
two placements; `EBADF` for a source handle that is not live or lacks
read; `ENOTDIR` as before; `ELOOP` for a cycle (I-3); `ENOSPC` for a
table at its cap ([[dec-2026-10-19-mount-table-dynamic]]); and `ENOMEM`
once entries are allocated. `SYS_UNMOUNT` answers the point's resolution
errno, and `EINVAL` when nothing is mounted there. `T_E_NOSPC` (28)
joins the registry. `ut` and libthyla-rs report each code. ERRORS.md
states the table.

## Rationale

The causes need different fixes (free an entry, fix the path, open the
source for read), and a caller can act on none of them from -1. Each
answer is the code Linux gives the same cause, which Pouch and Go
already decode, and native callers test `rc < 0`, so none regresses.
