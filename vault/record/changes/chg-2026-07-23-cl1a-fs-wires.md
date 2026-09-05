---
id: chg-2026-07-23-cl1a-fs-wires
type: chg
title: "Clade CL-1a: the FS/process syscall wires"
date: 2026-07-23
arc: arc-clade
commits: ["6d10943e"]
touched:
  - sub-pouch-fs
established: []
closed: []
opened: ["seam-pouch-dirfd"]
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
Twelve POSIX calls the toolchain demands per-compile, each wired onto an
EXISTING kernel syscall -- zero new kernel surface. `getpid`, `chdir`,
`getcwd`, `ftruncate`, `fchmod`, `fchmodat`, `faccessat`, `renameat`,
`unlinkat`, `mkdirat`, `readdir`, and `openat`'s `O_CREAT` arm.

The shape of the work is the shared split: Thylacine's mutation
primitives are parent-fd + leaf, not path-based, so `__pouch_open_parent`
opens the parent `O_PATH` and hands `(parent_fd, leaf)` to the kernel.
`faccessat` also dropped musl's privilege-drop clone dance as dead code
-- Thylacine has no setuid, so `euid == uid` always. Relative paths
became resolvable here, because `chdir`/`getcwd` landed in the same
patch.
