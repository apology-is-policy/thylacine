---
id: dec-2026-09-25-sys-mount-emount
type: dec
title: "SYS_MOUNT refuses what Plan 9's cmount refuses"
date: 2026-09-25
status: standing
decided-by: user-vote
affects: [sub-kernel-territory, sub-kernel-syscall-abi, spec-territory]
created: 2026-09-25
---
## Fork

`SYS_MOUNT` accepts two mounts Plan 9 refuses: a source whose type, directory
or file, differs from the mount point's, under any flag; and an `MBEFORE` or
`MAFTER` mount at a point that is not a directory. B-1d-u
([[dec-2026-09-24-union-covered-directory]]) kept the second from adding a
covered member and left the refusal itself to the operator, because refusing
narrows the syscall.

## Research

- **Plan 9.** `cmount` (`port/chan.c`) raises `Emount` when exactly one of the
  two files is a directory, whatever the flags, and when an ordered mount
  (`MBEFORE` or `MAFTER`) names an old file that is not a directory. bind(2)
  says of an ordered mount: "Both the old and new files must be directories."
- **Linux.** A bind or move mount whose source and mount point differ in
  directory-ness fails with `ENOTDIR`. Linux has no union flags.
- **The tree as found.** `sys_mount_for_proc` checks the flag mask and the
  source's rights, and `mount()` installs the entry whatever the two types are.
  A file mounted over a directory makes that name resolve as a file, so no name
  beneath it resolves. The one in-tree caller that does this is alloc-smoke's
  U-2f plumbing leg, which mounts `/bin/system.key` (a file) over the synthetic
  `/srv` directory with `MREPL`, `MBEFORE` and `MAFTER`
  (`usr/alloc-smoke/src/main.rs`).
- **The model.** `territory.tla` keeps `FilePaths`, points that are not
  directories, where an ordered mount stays plain (`territory_file_point.cfg`,
  `NoCoveredFile`).

## Options

1. **Refuse both, as Plan 9 does**, each with `ENOTDIR`.
2. **Refuse only the ordered mount on a file**, and keep type-mismatched `MREPL`
   mounts.
3. **Keep accepting both**, and record the deviation.

## The call

Option 1 (operator, 2026-09-25). `SYS_MOUNT` answers `ENOTDIR` when the
source's type (directory or not) differs from the mount point's, whatever the
flags, and when `MBEFORE` or `MAFTER` names a mount point that is not a
directory. `MREPL` of a file over a file stays legal. The implementing change
turns alloc-smoke's leg into refusal checks and keeps its success path with a
directory source, adds kernel tests for both refusals, and re-runs
`territory.tla` with the ordered mount at a file point refused. It is subject
to audit under the territory row of `docs/AUDIT-TRIGGERS.md`.

## Rationale

ARCH 9.6.1 says the mount flags mirror Plan 9, and after the union vote they
did in every respect but this one. A mount whose type differs from its point
leaves a name that was walked as one kind of file and resolves as the other, and
an ordered mount on a file has no directory to search. Refusing both at the
syscall is Plan 9's behaviour, agrees with Linux where Linux has the same case,
and means no program can reach the file-point case the mount table otherwise
has to handle.
