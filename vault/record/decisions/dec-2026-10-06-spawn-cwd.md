---
id: dec-2026-10-06-spawn-cwd
type: dec
title: "A spawn names its child's cwd, resolved by the spawner, in a record tail"
date: 2026-10-06
status: standing
decided-by: user-vote
affects: [sub-kernel-syscall-abi, sub-kernel-syscall-dispatch, sub-kernel-stalk, sub-libthyla-rs]
created: 2026-10-06
---
## Fork

The Go fork's `os/exec` gives a child a working directory by moving its own:
it chdirs the whole process, spawns, and chdirs back, holding a lock so that
no other spawn interleaves (the held launch's audit, round 2 F11). Every other
goroutine of the parent sees the borrowed cwd for that window, and a relative
open in it lands in the wrong directory. `SYS_SPAWN_FULL_ARGV` had no way to
name the child's cwd. The operator was asked whether to add one; the
alternative was to document the window.

## Decision

The operator voted on 2026-10-06: **add the cwd field**. A spawn may name the
cwd its child is born with.

Decided under the operator's grant, as the field's own design:
- **A record tail announced by a flag, not a grown record.** The 104-byte
  record had no free slot. Its `_pad_envp` word, which had to be 0, becomes
  `ext_flags`. Bit 0, `SPAWN_EXT_CWD`, says a 16-byte tail follows the record:
  the path's address, its length, and a flags word that must be 0. The kernel
  reads the tail only when the bit is set. A later tail takes the next bit and
  follows the earlier ones in bit order.
- **The spawner resolves the cwd, as chdir resolves one.** It is joined to
  the spawner's cwd when relative, walked by `stalk_landed` from the root, and
  must be a directory the spawner can search. The child is born with the
  landed name. A bad cwd fails the spawn with chdir's errno, and no child is
  made.
- **A relative image name is found from the child's cwd.** The image is the
  one a child that changed directory and then exec'd would run: posix_spawn's
  `addchdir` action runs before the exec, and Go documents `Cmd.Dir` the same
  way. With no cwd tail the image is found from the spawner's cwd, as before.
- **chdir answers its errno.** `SYS_CHDIR` answered every failure a bare -1,
  which pouch and Go read as `EPERM`. It now shares the spawn's resolver:
  `ENOENT`, `ENOTDIR`, `EACCES`, `EINVAL`, or a failing stat's own errno
  (`EIO` when it carries none), and chdir itself adds `EFAULT` for a bad
  buffer and `ENOMEM` when the new name cannot be stored. A 9P walk carries no
  errno yet, so one that a caught note interrupts answers `ENOENT`, as `open`
  does.

## Rationale

Growing the record would have moved every copy of it in step, and one copy
lives in the Go fork that main and aux share. `check-spawn-args-mirrors.py`
holds the fork's copy to the kernel header of the tree being built, so a fork
grown to 120 bytes breaks every tree whose kernel is still 104 until that tree
merges, and the reverse holds too. A flag-announced tail leaves the 104-byte
record where it is: only the in-tree copies rename the word, and the fork,
held to offsets, needs no change until it uses the tail. A kernel that predates
the tail refuses the bit rather than dropping the cwd, and a caller that does
not set the bit is never read past its record. Linux versions `clone3` and
`openat2` by an explicit size for the same reason. The word had been reserved
for a per-child environment override (`seam-pouch-spawn-envp`), and a u32
could never hold that vector's address, so the override becomes a tail too.

Plan 9 chdirs in the rfork child, and POSIX's
`posix_spawn_file_actions_addchdir` runs in the child. Here the image path is
already resolved in the spawner, before any child exists, so a missing image
fails the spawn rather than killing a child the caller must reap. The cwd
follows the image: the caller learns of a bad directory from the spawn's own
return. Resolving it with the spawner's credentials grants the child nothing,
because Thylacine's cwd is a name, not a held directory. Every lookup through
it is resolved again under the child's own identity, so a child born with a
cwd it cannot search fails its first relative lookup.
