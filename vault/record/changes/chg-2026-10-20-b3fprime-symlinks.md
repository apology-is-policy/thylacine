---
id: chg-2026-10-20-b3fprime-symlinks
type: chg
title: "Native lstat and readlink, and Pouch's symlink truth: lstat, readlink, realpath, O_NOFOLLOW, O_PATH, O_DIRECTORY, lchmod (B-3f')"
date: 2026-10-20
arc: arc-boosty
commits: ["8ff107f18", "3d1562eec", "3711c0750", "a91a87505", "f8256ffce", "e4e72d6f2", "105e26cfc", "e47fccce8"]
touched:
  - sub-kernel-syscall-abi
  - sub-kernel-syscall-dispatch
  - sub-kernel-vivarium
  - sub-kernel-stalk
  - abi-t-stat
  - sub-pouch-fs
  - sub-pouch-seam
  - sub-stratum-boot
  - sub-substrate-build
established: []
closed: []
opened: []
mirrors-checked:
  - usr/lib/libt/include/thyla/syscall.h
  - usr/lib/libthyla-rs/src/fs/metadata.rs
  - usr/lib/pouch/patches/0010-pouch-fstat-lseek.patch
  - usr/lib/pouch/patches/0019-pouch-stat.patch
  - usr/lib/pouch/patches/0021-pouch-pty.patch
  - usr/lib/pouch/patches/0024-pouch-fs-process-wires.patch
  - go-thylacine src/syscall/syscall_thylacine.go
depth: skeletal
created: 2026-10-20
---
B-3f' of Boosty ([[dec-2026-10-20-native-lstat-readlink]], operator vote
10): two native calls, `SYS_READLINK` 135 and `SYS_LSTAT` 136, thin entries
over the phenotype's audited cores ([[sub-kernel-syscall-dispatch]],
[[sub-kernel-syscall-abi]], [[sub-kernel-vivarium]]), and patch 0059, which
makes Pouch's lstat, readlink, realpath, `O_NOFOLLOW`, `O_PATH`,
`O_DIRECTORY` and lchmod tell the truth about links ([[sub-pouch-fs]]).
`symlink()` stays ENOSYS. The holotype round (Fable 5.1, 0/0/0/4) closed in
`105e26cfc`. Its F3 is that the kernel suite cannot see the handler glue:
kernel tests run in kproc, whose TTBR0 has no user half. So the witness is
`/bin/pouch-hello-links`, ten legs over the real links the bake puts at
`/lib/pouch-links` ([[sub-stratum-boot]], [[sub-substrate-build]]), and the
bar asks it to go red on a sabotaged kernel as well as on the old libc.
On the CI image of `16419a1ce` the suite passed 2046/2046, both new
kernel tests among them, and `pouch-hello-links` 10/10. On a sabotaged kernel
(lstat's flag 0, readlink's min off by one) exactly `lstat` and
`readlink-trunc` failed while the suite stayed 2046/2046; without 0059 all
ten failed; the CI image of `8f1871877`, after raw-open's round 1, was green
again (2046/2046, 10/10). The round's
F2 guard, no second close after an fstat `EBADF`, turned out unreachable on
the device (raw-open round 1, [[chg-2026-10-20-raw-open]]): the kernel's
fstat answers a flat -1 for a dead descriptor, which Pouch reads as `EIO`.
Two checkers broke before the first bake and were fixed
first (`f8256ffce`, `e4e72d6f2`): a patch that edits a file another host
checker compiles breaks that checker.
