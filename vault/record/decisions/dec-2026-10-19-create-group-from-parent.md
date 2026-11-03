---
id: dec-2026-10-19-create-group-from-parent
type: dec
title: "A created file's group is its directory's group"
date: 2026-10-19
status: standing
decided-by: user-vote
affects: [sub-kernel-syscall-dispatch, sub-kernel-loom, sub-kernel-ninep-dev9p, sub-kernel-perm]
created: 2026-10-19
---
## Fork

IDENTITY-DESIGN 3.5 F-5 resolved in 2026-05 that a created file's group
is its parent directory's group, as in Plan 9 and BSD. No path does it.
The sync create sends the caller's `primary_gid`
(`spoor_create_install`), and Loom's `MKDIR`, `MKNOD` and `SYMLINK` send
the ring creator's `primary_gid` for an SQE gid of 0. Holotype round 1
on the M7 and M8 work found the deviation (F4).

## Research

- **Plan 9 (heritage).** open(5): "the group of the file is the same as
  dir". The server stamps it.
- **BSD.** A new file always takes its directory's group
  (`ufs_makeinode` copies the parent inode's `i_gid`).
- **Linux / POSIX.** Linux uses the creator's fsgid, unless the
  directory is setgid, when it uses the directory's group (and a new
  directory inherits the bit). POSIX allows either.
- **The tree.** 9P2000.L's `Tlcreate`, `Tmkdir`, `Tmknod` and
  `Tsymlink` carry a gid, which Stratum stamps as given, so the kernel
  chooses the group. `spoor_create_install` stats the parent only where
  the Dev enforces rwx (for M8's bound). A caped session's creates
  already carry `P9_NOGID`, and Loom refuses a named gid on one. An
  explicit Loom gid must be one the creator may chgrp a file it owns to
  (`perm_wstat_check`).

## Options

1. **The parent directory's group**, as F-5 states.
2. **The caller's primary group**, and amend F-5.

## The call

Option 1 (operator, 2026-10-19). The sync create and Loom's gid-0
default send the parent directory's group, which the create learns from
the parent's stat, so the stat is no longer taken only where the Dev
enforces rwx. An explicit Loom gid keeps the chgrp rule. A caped
session keeps `P9_NOGID`. IDENTITY-DESIGN 3.5 F-5 states the rule.

## Rationale

F-5 chose inheritance for the reason Plan 9 and BSD did: a group
directory makes every file created in it the group's, which is what
shared work needs, and a user in several groups need not change their
primary group to work in each. The scripture was right and the code
diverged, and building the scripture needs no ABI change.
