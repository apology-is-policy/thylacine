---
id: dec-2026-09-25-mrepl-only-at-a-file
type: dec
title: "SYS_MOUNT accepts only MREPL at a point that is not a directory"
date: 2026-09-25
status: standing
decided-by: user-vote
affects: [sub-kernel-territory, sub-kernel-syscall-abi, spec-territory]
supersedes: dec-2026-09-25-sys-mount-emount
created: 2026-09-25
---
## Fork

[[dec-2026-09-25-sys-mount-emount]] took Plan 9's second `Emount` case to be
"an `MBEFORE` or `MAFTER` mount at a point that is not a directory". `cmount`
tests `order != MREPL`, and Plan 9's flag 0 is `MREPL`, so in Plan 9 the two
readings agree. In Thylacine they do not: a mount with no placement flag
appends after the point's members (`mount()` in `kernel/territory.c`). A
flagless mount of a second file at a file point that already hosts one makes a
group of two members at a file. The resolver treats two or more members as a
union, which it searches as a directory and whose listing skips members that
are not directories, so the new mount returns 0 and never shows. The B-1d-v
audit (round 1) and its self-audit both found the case.

## Research

- **Plan 9.** `cmount` (`port/chan.c`) raises `Emount` for
  `(old->qid.type&QTDIR)==0 && order != MREPL`, where `MREPL` is 0, `MBEFORE`
  1, `MAFTER` 2 and `MORDER` 3. A mount with no order bits is a replacement.
- **Thylacine as found.** `MREPL` is 0x1. With no placement bit, `mount()`
  appends like `MAFTER` but never makes the covered directory a member, and
  a repeat of a mount already at the point keeps its place. The
  flagless `SYS_MOUNT` callers in the tree (joey's cross-mount probe,
  stub-driver, attach-probe) all mount at directories. `ut`'s `mount` defaults
  to `MREPL`, as Plan 9's does, and viv mounts its files with
  `MREPL|MNOEXEC`.
- **Linux.** No union mounts. A bind mount over a file replaces what the name
  shows.

## Options

1. **Only `MREPL` at a file.** Refuse every other mount at a point that is not
   a directory with `ENOTDIR`: `MBEFORE`, `MAFTER` and a flagless mount, alone
   or with `MCREATE`, `MNOEXEC` or `MPHENO_LINUX`. This is `cmount`'s test
   taken literally, and `sys_mount_for_proc` decides it from the flags alone.
2. **Refuse a flagless mount only when a member is already there.** A lone
   flagless mount at a file stays legal, but the check needs the mount table,
   so it moves into `mount()` under the Territory lock.
3. **Treat a flagless mount at a file as `MREPL`.** Plan 9's meaning, but the
   same flag word would then mean two things depending on the point's type.
4. **Leave it, and document the case.**

## The call

Option 1 (operator, 2026-09-25). `SYS_MOUNT` answers `ENOTDIR` when the
source's type (directory or not) differs from the mount point's, whatever the
flags, and when the flags lack `MREPL` at a point that is not a directory. This
note restates the whole call of [[dec-2026-09-25-sys-mount-emount]] and
replaces it. The implementing change adds a kernel test over every refused flag
set (the three placements short of `MREPL`, each with every subset of
`MCREATE`, `MNOEXEC` and `MPHENO_LINUX`) and over a flagless mount beside a
mounted file, and a device leg in alloc-smoke for the file point.
`territory.tla` is unchanged apart from its header, since every source it
models is a directory.

## Rationale

A mount at a file can only replace what the name shows: there is no directory
to search, so `MREPL` is the one placement that leaves the name a file.
Refusing the others at the syscall means `SYS_MOUNT` never gives a file point
a second member, which is what the resolver assumes of a file. The check reads
no table state, so there is nothing to race. It is made at install, on the
point's own Spoor, so the resolver keeps its own type checks: a 9P server can
answer a later walk to the point with the other type. No caller in the tree
mounts at a file without `MREPL`.
