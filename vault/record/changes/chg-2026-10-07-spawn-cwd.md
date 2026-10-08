---
id: chg-2026-10-07-spawn-cwd
type: chg
title: "A spawn names its child's cwd in a record tail, and chdir answers its errno"
date: 2026-10-07
arc: arc-go-ide
commits: *(pending)*
touched:
  - sub-kernel-syscall-abi
  - sub-kernel-syscall-dispatch
  - sub-kernel-stalk
  - sub-libthyla-rs
  - sub-pouch-process
  - sub-substrate-build
  - sub-stratum-boot
  - sub-stratum-session
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-07
---
The Go fork gave a child a working directory by borrowing its own: chdir, spawn,
chdir back, under a lock, while every other goroutine saw the borrowed cwd (the
held launch's audit, round 2 F11). `SYS_SPAWN_FULL_ARGV` could not name the
child's cwd. The operator voted to add the field
([[dec-2026-10-06-spawn-cwd]]).

The record does not grow. Its `_pad_envp` word becomes `ext_flags`, and bit 0,
`SPAWN_EXT_CWD`, announces a 16-byte tail after the 104-byte record: the path's
address, its length, and a flags word that must be 0. The kernel reads the tail
only when the bit is set, so a caller that does not set it is never read past
its record, and an older kernel refuses the bit instead of dropping the cwd
([[sub-kernel-syscall-abi]]). The shared Go fork's copy is held to offsets
only, so it needs no change until it uses the tail; libt and libthyla-rs mirror
the tail, and libthyla-rs's `Command::current_dir` sends it
([[sub-libthyla-rs]]). `check-spawn-args-mirrors.py` holds the tail's mirrors
to the kernel's, and `check-flag-words.py` owns the new word
([[sub-substrate-build]]).

The spawner resolves the cwd before any child exists, by the resolver
`SYS_CHDIR` now shares (`sys_dir_landed_name`): joined to the spawner's cwd,
walked by `stalk_landed`, a directory the spawner can search. A bad cwd fails
the spawn with chdir's errno and makes no child; the child's thunk installs the
landed name before its first instruction; a relative image name is found from
the child's cwd ([[sub-kernel-syscall-dispatch]], [[sub-kernel-stalk]]).
`SYS_CHDIR` answered every failure a bare -1, which pouch and Go read as
`EPERM`; it answers the resolver's errno now, a failing stat's or re-walk's own
among them. A Dev's walk carries no errno yet, so a 9P walk that fails for any
reason -- a caught note interrupting it, a dead session -- answers `ENOENT`,
as it does for `open`.
