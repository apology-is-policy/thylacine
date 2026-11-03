---
id: dec-2026-10-14-b3-sqlite-9p-locks
type: dec
title: "File locks are 9P byte-range locks (Tlock), and SQLite runs in EXCLUSIVE locking mode"
date: 2026-10-14
status: standing
decided-by: user-vote
affects: [sub-kernel-ninep-client, sub-kernel-syscall-abi, sub-pouch-fs]
created: 2026-10-14
---
## Fork

SQLite needs file locks, and WebKit turns on WAL for every database it
writes (`SQLiteDatabase.cpp:190-191, :270`). Thylacine has no lock call:
Pouch's `__NR_flock` is the ENOSYS sentinel and `fcntl` answers ENOSYS.
WAL shares its index through a `MAP_SHARED` mapping of the `-shm` file,
which Pouch also lacks.

## Research

- Plan 9 has no byte-range locks; its idiom is the exclusive-use file
  (`DMEXCL`, one open at a time). 9P2000.L, which Thylacine speaks, has no
  `DMEXCL`; it carries Linux's byte-range locks as `Tlock`/`Tgetlock`.
- Stratum already serves both (`src/9p/server.c` `h_lock`, `h_getlock`), with
  the lock owned per fid -- Linux's open-file-description (OFD) semantics.
  Only the kernel's 9P client lacks the messages (`9p_wire.h` lists them as
  deferred).
- In EXCLUSIVE locking mode SQLite keeps the WAL index in process memory and
  never calls its shared-memory methods. In WebKit only the network process
  opens databases (cookies, IndexedDB, LocalStorage, service workers, ITP,
  private click measurement); no database is opened by two processes by
  design (read in the subtrees fetched, not tree-wide).

## Options

- **9P locks + EXCLUSIVE**: a native lock call forwarded to the file server as
  `Tlock`/`Tgetlock` (a syscall addition); Pouch maps `fcntl` locks and
  `flock` onto it; SQLite defaults to EXCLUSIVE, so WAL needs no shared file
  mapping. A second opener reads `SQLITE_BUSY` and never corrupts.
- **9P locks + shared file mappings**: the same, plus coherent `MAP_SHARED`
  file mappings in the kernel so WAL runs in normal mode across processes.
  The largest; it touches the paging invariants.
- **No locks**: SQLite's no-lock mode plus EXCLUSIVE; correct only while one
  process opens each database, silently corrupting otherwise.
- **Exclusive-use files**: `DMEXCL` as a 9P extension in the kernel and in
  Stratum.

## The call

9P locks + EXCLUSIVE (operator vote, 2026-10-14).

## Rationale

The file server already holds the lock state, so the kernel adds a client
message and a call, not a lock manager; the lock lives where the file lives.
EXCLUSIVE mode removes the one use of a shared file mapping B-3 would have
had, and the failure it leaves -- a second opener -- is a refusal, not
corruption.
