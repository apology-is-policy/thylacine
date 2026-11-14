---
id: dec-2026-11-14-exdev-and-names
type: dec
title: "A rename across devices answers EXDEV and mv copies; libthyla-rs names EROFS, EXDEV, EFBIG and ENAMETOOLONG"
date: 2026-11-14
status: standing
decided-by: user-vote
affects: []
created: 2026-11-14
---
## Fork

Moving a file onto or between shared folders (`docs/MAC-RUNNER.md` 4.4) fails two ways today.
Between two shared folders the app answers EXDEV (18) (`macrunner/app/src/interop/tree.rs`,
two host roots), a value libthyla-rs has no name for. Between a shared folder and the guest's own files
the kernel refuses the rename before any Dev op with EINVAL (`kernel/syscall.c`, the cross-Dev
check, whose comment names this seam #80: EXDEV is not in the registry, and adding it needs
signoff). A read-only folder's refusal of a mode change prints `kernel error (errno 30)`. `mv`
(`usr/coreutils/src/bin/mv.rs`) has no copy fallback, since nothing tells it a move crossed
devices.

## Research

- POSIX `rename(2)`: EXDEV when the two names are on different file systems; POSIX `mv` then
  copies and removes the source.
- Plan 9: `wstat` renames within a directory only, and `mv` copies across directories and file
  servers.
- `docs/ERRORS.md`: values follow Linux's numbering; a server errno with no `T_E_*` name crosses
  by value, and a new name appends to `errno.h` and the table with its first emitter.

## Options

1. The kernel answers EXDEV for a rename between two devices, `mv` falls back to copy and remove
   on EXDEV, and libthyla-rs names EROFS (30), EXDEV (18), EFBIG (27) and ENAMETOOLONG (36).
2. The names only.
3. Leave it as is.

## The call

The operator, 2026-11-14: option 1, as recommended (vote M15). The vote is the signoff for the
`docs/ERRORS.md` additions, which land with the kernel change.

## Rationale

EXDEV is the answer POSIX callers act on, and it makes `mv` work across the line in both
directions. The four names turn bare numbers into words a person reads. The kernel's rename is
an audit-trigger surface, so the change takes its audit round.
