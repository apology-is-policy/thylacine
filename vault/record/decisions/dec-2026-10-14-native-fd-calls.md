---
id: dec-2026-10-14-native-fd-calls
type: dec
title: "Native programs reach POSIX dup and a descriptor's status through two new syscalls"
date: 2026-10-14
status: standing
decided-by: user-vote
affects: [sub-kernel-handle, sub-kernel-syscall-abi, sub-pouch-process, sub-pouch-net]
created: 2026-10-14
---
## Fork

Pouch's `fcntl` (`F_GETFL`, `F_SETFL`, `F_DUPFD`), `dup`, `dup2` and `dup3`
answer ENOSYS. The kernel has POSIX dup -- to the lowest free number at or
above N (`handle_dup_posix`) and onto an exact number (`handle_dup_to`) --
and a read of a descriptor's access mode and nonblock flag
(`handle_get_status_flags`), but only the Linux phenotype's `fcntl`/`dup3`
arms reach them. Native `SYS_DUP` (12) takes a rights mask and installs at
the lowest free slot, and Pouch cannot learn a descriptor's rights to pass.

## Research

- Plan 9's `dup(oldfd, newfd)` duplicates onto a chosen number, or the lowest
  free one when `newfd` is -1.
- curl and OpenSSL set nonblocking with `fcntl(F_GETFL)` then `F_SETFL`.
- Small-integer socket numbers need dup onto a number: WebKit's WebSocket
  scheduler calls `select()` with `FD_SET` on a raw socket
  (`CurlStreamScheduler.cpp:182`, `CurlStream.cpp:104-108`), and `FD_SET`
  aborts on a tagged Pouch socket number (0039).
- Pouch has no `execve`, so close-on-exec has no effect there today; the
  kernel keeps the bit for the native `SYS_EXECVE` and the phenotype.

## Options

- **Two native syscalls**: a Plan 9-shaped dup(old, new, flags) -- exact
  number, lowest free, or lowest at or above a floor, and close-on-exec -- and
  a descriptor-status read (access mode, nonblock). Thin wrappers over the
  existing primitives.
- **One fcntl-shaped multiplexed call** mirroring the phenotype's arm.
- **No new syscalls**: libc bookkeeping; wrong for inherited descriptors, and
  dup2 stays ENOSYS.

## The call

Two native syscalls (operator vote, 2026-10-14).

## Rationale

The primitives exist and are audited; native code was only missing a door.
The dup takes Plan 9's shape, and the status read is the one piece of
`fcntl` that needs the kernel; everything else in `fcntl` builds on the pair
plus `SYS_SET_NONBLOCK`. The pair is also what lets a socket's number be a
real kernel descriptor.
