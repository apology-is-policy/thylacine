---
id: sub-kernel-filelock
type: sub
title: "File locks -- SYS_FILE_LOCK, its two owners, and the client half of 9P2000.L locking"
parent: moc-kernel-namespace
code: ["kernel/filelock.c", "kernel/include/thylacine/filelock.h", "kernel/test/test_filelock.c"]
audit: hard
guarded-by: [inv-i32, inv-i38]
validated-by: [prose]
locks: []
hazards: []
abis: []
design: ["docs/ARCHITECTURE.md section 9.6.11"]
created: 2026-10-29
updated: 2026-11-02
---
## Purpose

`SYS_FILE_LOCK` (137) is the kernel's half of byte-range file locks
(ARCHITECTURE.md 9.6.11, operator votes 34-35,
[[dec-2026-10-29-b3h-file-locks]]). The lock lives where the file lives: the
file's server holds every lock record and never waits. 9P2000.L's Tlock
answers a conflict with status BLOCKED, and the retry belongs to the client.
`kernel/filelock.c` is that client half: it validates the op word, checks the
descriptor's rights, chooses which owner the lock belongs to, and runs the
retry loop. The kernel keeps no lock table of its own. Its only lock state is
the per-Proc owner table described in [[sub-kernel-handle]]: one minted fid
per (Proc, file), and which file each names.

## Contract

`file_lock(fd, op, start, len, out)`: `op` is a type (`SYS_FILE_LOCK_RD` 0,
`_WR` 1, `_UN` 2) plus flags (`_WAIT`, `_TEST`, `_PROC`).

- **Owners.** Without `_PROC` the open file owns the lock. That is Linux's OFD
  and `flock` semantics: the Spoor's own fid is the owner, so dup'd and
  inherited descriptors share it. With `_PROC` the Proc owns it, which is
  POSIX `F_SETLK`. The owner is an entry in the Proc's table: a zero-name
  clone of the descriptor's fid, minted on the Proc's first take on that
  file. Threads share the entry. An rfork child starts with an empty table.
  exec keeps the table, and close-on-exec closes drop entries as any other
  close does. **Any close of the file by the Proc** drops all the Proc's
  locks on it, except that a navigation-only descriptor is no open of the file
  and its close drops nothing (Linux's `filp_flush` skips `FMODE_PATH`).
- **Refusals.**
  - `-T_E_INVAL`: an unknown op bit, type 3, `_TEST|_UN`, `_TEST|_WAIT`, or a
    range whose end would reach 2^64 (`len` 0 means "to EOF"). A server's table
    holds the end in a u64, and Stratum refuses more.
  - `-T_E_BADF`: not a Spoor; a navigation-only (`T_OPATH`) descriptor; a
    read lock without read rights; a write lock without write rights.
  - `-T_E_NOLCK`: the Dev serves no locks; the server answers ERROR, GRACE or
    a status 9P does not define; or the Proc's table is full
    (`PROC_LOCK_OWNER_MAX` 128, I-32).
  - `-T_E_STALE`: the descriptor's file has died on its server.
  - `-T_E_FAULT`: a `_TEST` answer outside the user half, refused before any
    wire op because only a user-half fault recovers (`uaccess_user_range_ok`,
    [[sub-kernel-uaccess]]), or one the call cannot copy out.
- **Conflict.** `-T_E_AGAIN`. With `_WAIT` the call polls instead, backing
  off from 1 ms to 64 ms, doubling each time. The server cannot report when a
  conflicting lock goes, so the cap bounds both the latency after a release
  and the RPC rate of a long wait.
- **Waits.** A wait for a take is note-interruptible (ARCH 8.8.3: F_SETLKW is
  on signal(7)'s list). It ends with `-T_E_INTR` for a caught note where the
  caller's waits are interruptible: Linux phenotype today, natives once item
  11c lands. A kill ends any wait. An unlock or a test never waits.
- **Tests.** `_TEST` changes nothing. It writes a `struct t_file_lock`
  (32 bytes): the conflicting lock's type, start and length, or `_UN` with the
  asked range. `pid` is always -1, because an owner here is a fid, not a
  process. A Proc test with no entry asks as a fresh temporary owner, never as
  the open file's fid, because an open-file lock conflicts with a Proc lock
  even within one Proc.
- **Post-grant fd re-check (Linux `fcntl_setlk`).** A Proc take re-checks,
  once granted, that its descriptor still names the same Spoor. If it does
  not, the call unlocks the range it took and answers `-T_E_BADF`. Without
  this, a lock whose descriptor closed mid-call would outlive the close meant
  to drop it.
- **No deadlock detection.** Stratum has none, and Linux has none for OFD
  locks. `_WAIT` never answers EDEADLK.

## Mechanism

`sys_file_lock_handler` validates the call. It takes the handle, refuses
`CWALKONLY`, and refuses a Dev with no `.lock`. It sets `note_interruptible`
for a waiting take, then loops on `file_lock_once`, with
`file_lock_backoff` (a `tsleep_noteintr` on a private Rendez that is never
woken) between attempts.

`file_lock_once` for the FILE owner is one `c->dev->lock(c, ...)`. For the
PROC owner it:
1. Gets the entry with `handle_lock_owner_get(p, c, mint = take)`. That makes
   `c` learn its file's identity first (`Dev.lock_ident`) and adopts or mints
   the entry.
2. If there is no entry, treats an unlock as done, and runs a test through a
   temporary owner from `Dev.lock_owner`, clunked after the call.
3. For any set-lock (an unlock, a take, a conversion), first lands every staged
   write of the file that the Proc can reach
   (`handle_lock_sync_file(..., DEV_LOCK_SYNC_RELEASE)`). A conversion gives up its
   write lock and admits readers. A test lands nothing.
4. Locks through the owner.
5. If the owner's fid answers `-T_E_STALE` on the first try, retires the
   entry and asks again once. The entry's file has died: Stratum kills an
   unlinked file's fids and gives the lowest freed inode number to the next
   create. So an entry found by number may belong to a dead file, and its
   locks died with it.
6. Puts the entry back, marking it granted when a take succeeded.

The Dev slots it drives are in [[sub-kernel-dev]]: `.lock`, `.lock_owner`,
`.lock_same_file`, `.lock_sync`, `.lock_ident`. The first four are
NULL-permitted together, and `.lock_ident` alone. Only dev9p implements them
([[sub-kernel-ninep-dev9p]]).

## Data structures

None of its own. `struct t_file_lock` (syscall.h, 32 bytes,
`_Static_assert`ed) is the `_TEST` answer. The Proc owner table
(`struct lock_owner_ent`, 16 bytes, `PROC_LOCK_OWNER_MAX` 128 per
HandleTable) lives in handle.h and is documented in [[sub-kernel-handle]].

## Concurrency

The handler holds the descriptor's handle reference for the whole call, so
`c` stays live through every attempt. Entries are reference-held: a caller
holds one across its RPC, and an entry is removed only when its users drop
to 0 (contract at `struct lock_owner_ent`). The server orders a close's
whole-file unlock against a concurrent take: either order is linearizable,
and the post-grant re-check catches the take that wins. No sleeping lock is
held across an RPC.

## Invariants enforced

- I-32: the owner table is bounded, and a full table answers ENOLCK without
  harm. A refused take spends no slot: an entry that never held a lock is
  removed when its last user puts it.
- I-38 (lock-to-unlock consistency): a grant drops the file's Larder attrs
  and pages, and every set-lock lands the staged writes first (dev9p half).
- I-5/I-6: none. A lock is not a handle, and a minted owner is never
  published to a handle slot.

## Error paths

Every exit puts the handle. Every entry got is put on every path, the
ESTALE retry included. A temporary test owner is clunked after its one call.
A dying thread that cannot flush or send hands both to the session's closer
(see [[sub-kernel-ninep-attach]], the job alone).

## Performance

A take is one Tlock round trip, plus on the Proc's first take on a file a
zero-name Twalk and one Tgetattr for the generation (learned once per
Spoor). A set-lock with staged writes adds the write-behind flush.

## Prosecution

Mutants run in the B-3h2 lease (each must turn its named leg red):
- F1, the close hook returns early: `filelock.close_drops`.
- F2, the close removes an entry still in use: `close_mid_call`.
- F3, no post-grant fd re-check: `close_mid_call`.
- F4a, the dev9p unlock does not flush: `dev9p.lock_unlock_flushes_first`.
- F4b, the Proc unlock does not sync: `filelock.proc_owner`.
- F5, a never-granted entry is kept: `failed_take_spends_no_slot`.
- F6, the bound is off by one: `bound`.
- F7, no retire on ESTALE: `stale_entry_retired`.
- F8, a dying thread hands nothing over:
  `p9_closer.dying_lock_release_flushes_shared_run`.
- F9, `same_file` ignores the generation: `dev9p.lock_ident_generation`.
- F10, the close hook does not learn the identity: `filelock.reused_number`.
- F11, an owner does not inherit the generation:
  `dev9p.lock_ident_generation`.
- F12, a get does not learn the identity: `filelock.reused_number`.
- F13, the closer sends a Tclunk for a job alone: the p9_closer leg.

B-3h3's mutants, each of which must go red at its own leg (not yet run):
- G1, a take syncs nothing; G2, the table ignores a failed ACQUIRE; G13, it
  refuses a peer's ESTALE; G19, a take skips the table:
  `filelock.acquire_syncs`.
- G3, a strict mint keeps no fid; G4a, ACQUIRE re-clones; G5, a read ignores the
  read fid; G7, the close skips the read fid: `dev9p.cached_open_lock`.
- G4b, G4c, a plain store replaces either CAS:
  `dev9p.cached_open_install_race`.
- G6, a ring gets the unopened fid: `dev9p.cached_open_loom`.
- G8, a failed mint keeps its fid: `dev9p.cached_open_fallbacks`.
- G9, fstat does not patch the size; G18, a wstat does not bind:
  `dev9p.cached_open_loose_wstat`.
- G10, no generation compare; G11, no number compare; G14, a mint with no known
  generation; G15, no validity check: `dev9p.cached_open_loose_stale`.
- G12, the base is not released; G17, an unlock binds:
  `dev9p.cached_open_loose_lock`.
- G16, a NULL generation is kept: `larder.attr_gen`.

Audit round 1's regressions, each of which must go red at its own leg (not yet run):
- G20, a `_TEST` answer is not range-checked: the kernel extincts at
  `filelock.refusals`.
- G21a, only an unlock flushes: `dev9p.lock_unlock_flushes_first`. G21b, only a
  Proc unlock syncs; G21c, a test syncs: `filelock.proc_owner`.
- G22, `wstat_apply` does not bind before its policy:
  `dev9p.cached_open_loose_wstat`.
- G23, an `O_PATH` close drops the Proc's locks: `filelock.close_drops`.
- G24, every walk failure is ESTALE; G24b, only ENOENT is; G28, a loose mint from
  an opened base: `dev9p.cached_open_loose_stale`.
- G25, a clone's failure is always ENOLCK: `dev9p.lock_owner_clone`.
- G26, only ESTALE stops the close hook: `filelock.reused_number`.
- G27, a range may reach 2^64: `filelock.refusals`.
- G29, G30, `proc_id` 0: `dev9p.lock_wire_and_status`, `dev9p.lock_test_answers`.

## Cached opens and the take's sync (B-3h3, 2026-11-02)

A take must leave the snapshot of every open it covers, or the holder reads the
file as it was at open. `file_lock_acquire_sync` runs before every take attempt,
on `c` and then, for either owner, on every open of the file in the table
(`handle_lock_sync_file(..., DEV_LOCK_SYNC_ACQUIRE)`), because the lock covers what
any of the Proc's opens read. The open-file owner learns
its file first, as an entry's get does, so an open of a dead file sharing its
number is left alone. A failed sync refuses the take, except that a table peer
answering ESTALE keeps its snapshot and the take through the descriptor that can
reach the file stands. A peer's other failures refuse: nothing says the file
moved. The descriptor's own ESTALE refuses first. A cached open's
close now matches entries by generation as any open's does: both kinds learn one
at mint.

## Seams

None open. Pouch `fcntl`, `flock` and the libthyla-rs and libt mirrors landed
with B-3h4 ([[sub-pouch-fs]], [[sub-kernel-syscall-abi]]). A caught note does not
end a native or Pouch wait until item 11c (Caveats). Go's `cmd/go` file lock is
still a no-op on Thylacine (OPEN-BUGS 2026-11-02).

## Caveats

- A native caller's wait ends only on grant or kill until item 11c lands
  (ARCH 9.6.11).
- `_WAIT` is a poll (1 to 64 ms), not a wakeup: 9P2000.L has no lock
  callback.

## History

Boosty B-3h2 (commits d1853e821, c95ef3916, b693e2729 on branch b3h; close
commit in [[chg-2026-10-29-b3h2-file-locks]]). Scripture 87ecc8153. B-3h3 and
B-3h4: [[chg-2026-11-02-b3h3-b3h4-cached-locks-pouch]]. Stratum's half (POSIX range records,
the zero-name clone of an open fid, ENOLCK caps) lands as one Stratum commit
after the B-3h audit.

## Provenance

(generated -- incoming `touched` backlinks, newest first; never hand-written)
