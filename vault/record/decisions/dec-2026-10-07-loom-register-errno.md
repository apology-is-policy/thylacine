---
id: dec-2026-10-07-loom-register-errno
type: dec
title: "SYS_LOOM_REGISTER returns the negative errno, not a bare -1"
date: 2026-10-07
status: standing
decided-by: user-vote
affects: [sub-kernel-loom, sub-kernel-syscall-abi, sub-libthyla-rs]
created: 2026-10-07
---
## Fork

Since the Loom write-behind fold ([[dec-2026-10-07-exit-close]]'s land), a
Loom registration flushes dev9p's staged run first and fails on a latched
flush error. `loom_register_handles` returns 0 or -1, so a latched ENOSPC, the
server's EIO or a death's EINTR reaches userspace as the generic -1, which
libthyla-rs reads as `InvalidArgument` (Loom write-behind audit r1 F5).

## Research

- The ABI is per-family: older calls return a bare -1, newer ones a negative
  errno ([[sub-kernel-syscall-abi]]). All three Loom syscalls (`SETUP`,
  `REGISTER`, `ENTER`) return a bare -1 today; this decision changes
  `REGISTER` only, the one whose failure now carries a cause from another
  layer.
- The latch itself is not lost: a latched file keeps reporting the errno
  through sync write, fsync and close (witness
  `dev9p.wb_loom_register_keeps_the_latch`).

## Options

1. **Return -errno.** `loom_register_handles` and the syscall return the
   negative errno; libthyla-rs maps it to the matching error kind.
2. **Keep -1.** The cause stays visible only through the file's own write,
   fsync and close.

## The call

Option 1 (operator, 2026-10-07, AskUserQuestion). A syscall-interface change;
it rides with B-2's land.

## Rationale

A registration that fails because a write could not be flushed should say why,
in the ABI's newer -errno convention; a caller that retries on EINTR or
reports ENOSPC needs the value, and -1 tells it only that something failed.
