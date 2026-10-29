---
id: dec-2026-10-29-b3h-file-locks
type: dec
title: "File locks: one SYS_FILE_LOCK call naming its owner (open file or Proc), forwarded as 9P Tlock/Tgetlock"
date: 2026-10-29
status: standing
decided-by: user-vote
affects: [sub-kernel-syscall-abi, sub-kernel-syscall-dispatch, sub-kernel-ninep-wire, sub-kernel-ninep-client, sub-kernel-ninep-dev9p, sub-kernel-larder, sub-kernel-handle, sub-libthyla-rs, sub-pouch-fs]
created: 2026-10-29
---
## Fork

[[dec-2026-10-14-b3-sqlite-9p-locks]] chose 9P byte-range locks for
B-3h: a new kernel call forwarded to the file server as `Tlock` and
`Tgetlock`. It left two things open, and both shape a new syscall: whose
lock it is (the owner), and the call's arguments.

## Research

- Plan 9 has no byte-range locks (its idiom is the exclusive-use file,
  `DMEXCL`). 9P2000.L carries Linux's: `Tlock fid type flags start length
  proc_id client_id` answers SUCCESS, BLOCKED, ERROR or GRACE, and "the
  client retries the lock request in an interruptible loop until status is
  no longer BLOCKED" (diod `protocol.md`).
- Linux's v9fs takes the local VFS lock first, then sends `Tlock`; on
  BLOCKED with `F_SETLKW` it sleeps interruptibly and retries; ERROR and
  GRACE answer ENOLCK; a remote failure reverts the local lock; a set-lock
  flushes and invalidates the page cache (`fs/9p/vfs_file.c`).
- Linux has two owners. A POSIX (`F_SETLK`) lock belongs to the descriptor
  table: threads share it, fork does not copy it, exec keeps it, and closing
  any descriptor of the file drops it. An OFD lock (`F_OFD_SETLK`) and a
  `flock` lock belong to the open file description.
- Stratum keys a lock to the fid (`lock_owner_base | fid`, `server.c`
  `h_lock`), drops a fid's locks when it is clunked, and ignores `proc_id`
  and `client_id`: OFD semantics. It never waits; a conflict answers
  BLOCKED with or without the BLOCK flag.
- Stratum's lock table (`src/locks/locks.c`) stacks a same-owner overlap
  instead of replacing it, releases only an exact range, has no record cap,
  and `Rgetlock` reports a WRLCK over the request's range rather than the
  conflicting lock. SQLite's EXCLUSIVE-to-SHARED downgrade and its one
  two-byte unlock of PENDING and RESERVED both fail on it, so the table
  takes POSIX range semantics as part of B-3h.

## Options

Owner:
- **Both owners**: the call names its owner. The open file (the Spoor's own
  fid) serves `flock`, `F_OFD_*` and native programs; the Proc's
  descriptor table serves POSIX `F_SETLK`, built by the kernel from one
  extra fid per Proc and file. Stratum's owner model is unchanged.
- **Open file only**: `F_SETLK` takes OFD semantics. A parent that opens a
  lock file and forks workers that lock the inherited descriptor gets no
  exclusion among them, silently; two opens in one Proc conflict.
- **Proc owner in Stratum**: Stratum keys locks by session and `proc_id`
  too, and the kernel sends an unlock-all when any descriptor of the file
  closes. More cross-tree work in Stratum's server and lock spec.

Shape:
- **One call, register arguments**: `SYS_FILE_LOCK(fd, op, start, len, out)`
  with the type and WAIT, TEST and PROC bits in `op`.
- **Two calls**: a set call and a test call.
- **Linux-shaped**: `(fd, cmd, struct flock *)`.

## The call

Both owners, one call with register arguments (operator votes 34 and 35,
2026-10-29).

## Rationale

The lock state stays where the file lives, as the 10-14 decision wants:
the Proc owner is a second fid on the same server table, not a kernel lock
manager. POSIX `F_SETLK` keeps its process semantics, so the fork-then-lock
idiom keeps its exclusion, and OFD and `flock` keep theirs. A register
call needs no user memory on the set path; only TEST writes its answer.
