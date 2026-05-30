# 97. corvus identity DB + the first secret-on-disk path (A-1b)

> As-built reference for A-1b: corvus as the authoritative `id <-> name <->
> groups` resolver with real on-disk persistence, and the first end-to-end
> SECRET-on-disk write/read-back path in Thylacine. Design intent lives in
> `docs/CORVUS-DESIGN.md` section 16 + `docs/IDENTITY-DESIGN.md` section 9.1;
> this doc describes what is in the tree.

## Purpose

corvus is the per-host identity authority (`docs/reference/95-identity.md` covers
the kernel-side principal_id/gid model A-1a stamps on `struct Proc`). A-1b gives
corvus two things it lacked: (1) a queryable `id <-> name <-> groups` map exposed
as three new `/srv/corvus` verbs, and (2) **real persistence** so a user
provisioned in one boot survives a reboot -- both the non-secret identity record
AND the per-user secret keypair wrap, so AUTH works after a reboot.

corvus runs confined to its storage capability (A-1.7,
`docs/reference/98-capability-storage.md`): joey hands it a R|W-no-TRANSFER Spoor
at spawn and corvus `SYS_CHROOT`s to it, so every path below resolves WITHIN
`/var/lib/corvus`. corvus authors no threads; its `/srv/corvus` server loop is
request-serialized, which is the safety basis for the `static mut` tables.

## The two on-disk artifacts (the secret boundary)

| File | Magic/version | Secret? | Write cadence | Holds |
|---|---|---|---|---|
| `identity.db` | `CRVS`/**v2** | NO | rewrite-swap on each USER_CREATE / GROUP_CREATE | `next_auto_id` + all user identity records + all group records |
| `users/<name>/hybrid.corvus` | `CRVS`/v1 | YES (ciphertext) | write-once at USER_CREATE | the AEGIS-256-wrapped hybrid keypair (X25519 + ML-KEM-768) |

The shared `CRVS` magic with a distinct version field is the discriminator: a v1
blob is a per-user keypair wrap; a v2 blob is the central identity DB. The
identity DB is **non-secret** (a uid<->name<->gid map is world-readable on every
Unix; `/etc/passwd` is mode 644), which is what keeps the A-1b *secret*-handling
surface confined to the wrap blob -- and `hybrid.corvus` is itself ciphertext
(decryptable only with the passphrase-derived KEK), so even the wrap path never
writes plaintext secret to the FS (C-24).

## Public API -- the verbs (`usr/corvus/src/main.rs`)

Extends the `/srv/corvus` verb set (CORVUS-DESIGN.md section 6.4). All payloads
little-endian. `peer_live_caps(handle)` does a fresh `SYS_SRV_PEER` re-query
(C-22: caps are mutable mid-conversation, so admin gates must never trust the
at-accept snapshot; fail-closed to 0 on a dead/unknown peer).

| verb | id | gate | request | OK reply |
|---|---|---|---|---|
| USER_CREATE | 5 | CAP_HOSTOWNER (except the bootstrap first user) | ...existing... + `supp_gid_count u8` + `supp_gids[count] u32` (absent => 0) | `principal_id u32` + `primary_gid u32` |
| RESOLVE_ID | 11 | **ungated** (getpwuid) | `principal_id u32` | `primary_gid u32` + `supp_gid_count u8` + `supp_gids[] u32` + `name_len u8` + `name` |
| RESOLVE_NAME | 12 | **ungated** (getpwnam) | `name_len u8` + `name` | `principal_id u32` + `primary_gid u32` |
| GROUP_CREATE | 13 | CAP_HOSTOWNER (live re-query) | `name_len u8` + `name` | `gid u32` |

- `handle_user_create` (live-cap gate, except `user_states_count() == 0`
  bootstrap) -> bounds the username + passphrase + the append-only supp_gids
  extension -> generates + AEGIS-wraps the keypair -> `alloc_auto_id` ->
  persist (below) -> returns `{principal_id, primary_gid}`.
- `handle_resolve_id` / `handle_resolve_name` are ungated; they leak only the
  non-secret map (no wrap/keypair/token bytes flow through them). `STATUS_NOT_FOUND`
  on an unknown id/name.
- `handle_group_create` gates FIRST (a non-hostowner learns nothing about the
  group table), then bounds the name, caps the live group count (see F1 below),
  `alloc_auto_id`s a gid from the **shared** counter (so a standalone group's gid
  never collides with a UPG), persists.

## UPG id allocation -- one shared monotonic counter

`alloc_auto_id` (`NEXT_AUTO_ID`, starts at `FIRST_AUTO_ID = 1000`) is shared
between principal_ids and gids (the Red Hat user-private-group scheme): a UPG's
`uid == gid` is collision-free because a gid and a principal_id are never the
same number unless they are the matched UPG pair. USER_CREATE assigns
`principal_id = primary_gid = next_auto_id++` and creates the matching UPG group
`{gid = principal_id, name}`. The counter only climbs and `alloc_auto_id` refuses
to mint `>= PRINCIPAL_SYSTEM` (covers both reserved sentinels `PRINCIPAL_SYSTEM`
and `PRINCIPAL_NONE`, pinned by `const`-asserts on the ordering), so a reserved
value is never assigned (I-22: an assigned id confers no ambient authority).
`next_auto_id` is persisted in the identity.db header, so a deleted id is never
reused even after its record is gone (monotonic-forever; v1.0 has no USER_DELETE).

## Data structures (byte-precise)

### CRVS v2 `identity.db` -- 20-byte header + variable records

```
Header (IDENTITY_DB_HEADER_LEN = 20):
  [0..4)   magic         'CRVS' (0x53565243 LE)
  [4..8)   version       = 2
  [8..12)  next_auto_id  u32  (FIRST_AUTO_ID=1000 .. < PRINCIPAL_SYSTEM)
  [12..16) user_count    u32  (<= MAX_USERS = 256)
  [16..20) group_count   u32  (<= 2 * MAX_USERS = 512)

User record x user_count:
  [0..4)   principal_id  u32   (!= INVALID, < next_auto_id)
  [4..8)   primary_gid   u32   (!= INVALID, < next_auto_id)
  [8]      backend       u8    (0 = passphrase)
  [9]      supp_gid_count u8   (<= PROC_SUPP_GIDS_MAX = 15)
  [10]     name_len      u8    (1 .. MAX_USER_LEN = 32)
  [11 .. 11+4*supp_gid_count)  supp_gids  u32 each (no reserved sentinels)
  [.. + name_len)              name

Group record x group_count:
  [0..4)   gid       u32   (!= INVALID, < PRINCIPAL_SYSTEM)
  [4]      name_len  u8    (1 .. MAX_GROUP_LEN = 32)
  [5 .. + name_len)  name
```

`identity_db_serialize` writes this; `identity_db_parse` reads it. The parser is
the load-bearing safety surface: header length + magic + version checked;
`next_id`/`user_count`/`group_count` range-checked; **every** length field
bounds-checked against the remaining buffer BEFORE the read (no over-read);
`supp_gid_count <= 15`, `name_len in 1..=32`; reserved/illegal id values fail
closed; **trailing bytes past the last record fail the whole load closed**
(`off != blob.len()`); any malformed/truncated record fails the WHOLE load
closed. An atomically-written file is complete by construction, so a short file
means a crash mid-rename, recovered from the surviving real file.

### CRVS v1 `hybrid.corvus` -- the secret wrap (TOTAL_LEN = 3752)

```
Header (HEADER_LEN = 72):
  [0..4)   magic 'CRVS'   [4..8) version = 1
  [8..12)  argon2 t_cost  [12..20) m_cost_kib (u64)  [20..24) parallelism
  [24..40) salt (16)      [40..72) AEGIS-256 nonce (32)
Body:
  [72 .. 72+KEYPAIR_LEN)  ciphertext (KEYPAIR_LEN = 3648: X25519 sk+pk + ML-KEM-768 ek+dk)
  [.. + AEGIS256_TAG_LEN) tag (32)
```

`to_bytes` serializes; `fill_wrap_from_bytes` parses (requires EXACTLY
`TOTAL_LEN`, magic+version checked, `m_cost_kib > u32::MAX` rejected). 3752 B
exceeds `STM_INODE_INLINE_MAX = 100`, so `hybrid.corvus` is an **extent** file --
which is why it is the first consumer to write AND read back a multi-chunk extent
over the disk-backed FS, and why the three-bug masking stack surfaced here.

## State machines

### Persist -- atomic rename-swap (CORVUS-DESIGN section 16.6)

`handle_user_create` ordering (the wrap is durable BEFORE the identity record
commits, so a crash between leaves a harmless orphan wrap, never a record
pointing at a missing wrap):

1. `persist_keypair_wrap`: `mkdir_opath` the per-user dir -> unlink any stale
   orphan -> `SYS_WALK_CREATE hybrid.corvus` -> write -> `SYS_FSYNC(file)` ->
   `SYS_FSYNC(per-user dir)` + `SYS_FSYNC(users/ dir)`. **If any step fails,
   abort before touching identity.db.**
2. In-memory append (`user_states_insert` + `groups_push` UPG), then
   `identity_persist`: serialize -> `SYS_WALK_CREATE identity.db.tmp` -> write ->
   `SYS_FSYNC(tmp)` -> `SYS_RENAME(tmp -> identity.db)` (the atomic commit point)
   -> `SYS_FSYNC` the root dir fd. On any failure, roll back the in-memory
   appends (`groups_pop_last` + `user_states_pop_last`).
3. After commit: `dataset_owner_register(users/<name> -> <name>)` so WRAP/UNWRAP
   gate correctly.

Crash-safety (C-26): a crash during the tmp write leaves a partial tmp + the
intact old DB (rename never happened) -> load unlinks the stale tmp + reads the
old DB. A crash at/after the rename leaves a complete DB (rename is atomic).
There is never a torn `identity.db`.

> **Durability note (audit R1 F5).** On the current Stratum, `Tfsync` (`h_fsync`)
> is a WHOLE-POOL commit (`stm_fs_commit`); the fid is only validated, never used
> to scope the commit. So corvus's file-level `SYS_FSYNC(wf)` already commits the
> new dirents -- the per-directory fsyncs in step 1 are **forward-portable
> insurance** that becomes load-bearing only when Stratum makes `Tfsync` per-fid
> (the POSIX FS contract, where a file fsync does not make a name->inode link
> durable). They are harmless idempotent re-commits today and are kept so the
> path is correct on any 9P server. The load-bearing cross-reboot fixes were the
> Stratum-side bdev partial-tail RMW + read-offset alignment (see "History").

### Load (boot, `identity_load`, CORVUS-DESIGN section 16.5)

1. `mkdir_opath users/` (idempotent).
2. `SYS_UNLINK` any stale `identity.db.tmp` (cleans a crash between create-tmp
   and rename).
3. Open `identity.db`: absent -> empty DB (fresh install, `next_auto_id = 1000`);
   present -> read (bounded by `IDENTITY_DB_MAX = 256 KiB`) + `identity_db_parse`,
   FATAL (corvus refuses to start) on present-but-corrupt -- an attacker who
   corrupts identity.db must not get a free first-user bootstrap.
4. For each parsed user, `load_keypair_wrap users/<name>/hybrid.corvus` to fill
   the wrap fields + re-register the dataset-owner mapping. A user whose wrap is
   missing/corrupt is logged + **dropped fail-closed** (no usable secret -> not
   authoritative for login).

## Error paths

- Verbs: `STATUS_BAD_FORMAT` (malformed payload / bad name / reserved supp_gid),
  `STATUS_PERMISSION_DENIED` (USER_CREATE/GROUP_CREATE without CAP_HOSTOWNER, or
  USER_CREATE of an existing name), `STATUS_NOT_FOUND` (RESOLVE of unknown id/name),
  `STATUS_INTERNAL_ERROR` (RNG/KDF/wrap failure, table full, persist failure,
  group-count cap).
- Load: `identity_load` returns false (-> `rs_main` prints
  `corvus: FATAL identity.db load failed (corrupt/unreadable)` + exits 1) on a
  present-but-corrupt DB or an FS error establishing `users/`.

## Tests

- **joey corvus harness** (`usr/joey/joey.c`, runs every boot): USER_CREATE susan
  (+ supp_gids + gated) -> GROUP_CREATE wheel -> RESOLVE_NAME/ID round-trips ->
  WRAP/UNWRAP (incl. cross-user PermissionDenied + key_id AAD bind + malformed
  reject). Idempotent: on a persistent pool the second boot reports
  "already provisioned" / "already exists".
- **Cross-reboot guard** (`tools/test-cross-reboot.sh`, `make test-cross-reboot`):
  re-bakes a clean pool, boots once to write, boots again on the SAME pool, and
  asserts boot2 shows `USER_CREATE susan already provisioned` (identity.db
  persisted) + `UNWRAP users/michael ok (DEK round-trip verified)` (the secret
  keypair wrap reloaded from disk -- the cold extent read-back), **on a
  successful boot**. It HARD-FAILS (never retry-masks) on two signatures: a
  missing persistence marker (boot OK but no marker = a real A-1b regression) AND
  the recurring stratumd-mount `STM_EBADTAG` corruption (see Known caveats); it
  retries only a genuinely slow boot (no boot-OK, no extinction, no corruption
  signature). The retry budget exists for QEMU-TCG slowness, not to paper over a
  bug.
- **Audit R1**: `memory/audit_a1b_closed_list.md` (0 P0 / 0 P1 / 1 P2 / 4 P3,
  CLEAN). The first secret-on-disk write/read path; prosecuted hard.

## Performance characteristics

identity.db is rewritten in full on each mutation (no incremental update); at 256
users the worst case is ~36 KiB, well under the 256 KiB read cap. The wrap is
write-once. FS I/O is chunked at `FS_IO_CHUNK = 2048` (<= `SYS_RW_MAX`), so the
3752 B wrap is a 2-chunk read/write (offsets 0 and 2048) -- the exact pattern
that exercised the read-offset and partial-tail fixes.

## Status

- A-1b impl: Thylacine `451afd4`. corvus dir-fsync: `573b984`. The audit-close
  fixes (F1 group cap + F2 supp_gid/primary_gid validation + F5 doc) land in the
  A-1b close commit.
- The three cross-reboot fixes: Stratum `91ae5d8` (bdev partial-tail RMW + read
  alignment) + Thylacine `573b984` (corvus dir-fsync). F4 (read-helper scratch
  bound) lands in the Stratum close commit.
- **Cross-reboot acceptance GREEN (2026-05-30):** `tools/test-cross-reboot.sh`
  passes 3/3, first try, both boots. The long-standing mount-`STM_EBADTAG`
  "blocker" was a build-harness stale-key footgun -- fixed Thylacine `b7066e4`
  (the `build.sh pool` target now rebuilds the ramfs so `/system.key` tracks the
  re-baked pool). NOT a corvus or read-path bug. See Known caveats +
  `docs/DEBUGGING-PLAYBOOK.md` section 6.10.

## Known caveats / footguns

- **Build footgun (this WAS the year-long "EBADTAG corruption"; resolved
  2026-05-30).** Re-baking the pool with a bare `tools/build.sh pool` regenerates
  the random `system.key`, but the ramfs bakes `/system.key` in at `build_ramfs`
  time -- so a stale ramfs key against a fresh pool makes stratumd mount with the
  WRONG key and AEAD reject the first btree node (`STM_EBADTAG` -> `rc=-201` at
  mount -> joey FATAL -> extinction). This wore the "AEGIS-256 corruption" mask
  for ~a year; the "intermittency" was build-command history (failed after
  `build.sh pool`, passed after `build.sh kernel`/`all`). Fixed `b7066e4` (the
  `pool` target now rebuilds the ramfs); `build.sh kernel`/`all` always did.
  Sanity check on any mount EBADTAG: `shasum build/fixtures/system.key
  build/ramfs-src/system.key` -- if they differ, it is the key, not the bytes.
  Full coda: `docs/DEBUGGING-PLAYBOOK.md` section 6.10.
- **Dropped-wrap records are not pruned (audit R1 F3).** A user whose
  `hybrid.corvus` is missing/corrupt is dropped from the in-memory table on load
  but the identity.db row is NOT rewritten, so it persists + re-drops + re-logs
  every boot. Harmless (next_auto_id is preserved, so the dropped id is never
  reused). A clean self-heal must also prune the dead user's UPG group; deferred
  to the USER_DELETE/cleanup chunk.
- **`static mut` safety rests on single-threadedness.** corvus spawns no threads
  and serializes requests; if that ever changes, every `static mut` table
  (`NEXT_AUTO_ID`/`USER_STATES`/`GROUPS`/`DATASET_OWNERS`) needs a lock.
- **Whole-pool Tfsync** (F5 above): do not "optimize away" the per-dir fsyncs --
  they are the forward-portable contract for a per-fid Tfsync.

## History -- the masking stack behind the "AEGIS-256 corruption"

A-1b's cross-reboot was blocked for ~a year by what presented as content-sensitive
"AEGIS-256 corruption" (#710/712/714/H1). It was never one bug. The READ-BACK
triplet below -- all first exposed because `hybrid.corvus` is the first
multi-chunk extent corvus writes AND reads back -- was real and is fixed. But the
recurring stratumd-MOUNT `STM_EBADTAG` that kept "re-opening" *afterward* was a
FOURTH, separate cause: a build-harness stale-key footgun (`build.sh pool`
re-baked the key without rebuilding the ramfs), root-caused + fixed `b7066e4` on
2026-05-30 (see Known caveats above + `docs/DEBUGGING-PLAYBOOK.md` section 6.10).
The read-back triplet:

1. **bdev_thylacine `op_write` partial-tail clobber** (Stratum). A block backend
   transfers whole sectors; a non-sector-aligned extent write (4128 B) zero-padded
   the tail sector, destroying an adjacent on-disk object's bytes. Fix: whole-sector
   prefix + read-modify-write of the partial tail. **Load-bearing.**
2. **corvus missing dir-fsync** (Thylacine). Reframed by audit R1 F5: forward-portable
   insurance, NOT load-bearing on current whole-pool-Tfsync Stratum (above).
3. **fs.c raw read offset** (Stratum). `stm_sync_read_extent` rejects a non-4096-aligned
   offset; the read path passed the raw file offset, so the 2nd chunked Tread
   (offset 2048) returned EINVAL. Fix: `fs_read_extent_aligned_locked`. **Load-bearing.**

The full investigation journal + the reusable method is `docs/DEBUGGING-PLAYBOOK.md`
(mandatory reading when an elusive corruption-class bug recurs). The lesson that
shaped the cross-reboot test: a same-boot write-then-read masks both the read-offset
bug (warm DMA buffer) and any dirent-durability bug (warm FS cache); only a genuine
reboot on the same pool exercises the cold read-back.

## Cross-reference

- Design: CORVUS-DESIGN.md section 16; IDENTITY-DESIGN.md section 9.1.
- Kernel identity model: `docs/reference/95-identity.md` (A-1a).
- FS-mutation syscalls used: `docs/reference/96-fs-mutation.md`
  (SYS_WALK_CREATE/FSYNC/READDIR/RENAME/UNLINK).
- Storage capability + T_OPATH: `docs/reference/98-capability-storage.md` (A-1.7).
- Invariants: I-22 (identity confers no caps), I-14 (storage integrity); C-22/C-24/C-26.
- Audit: `memory/audit_a1b_closed_list.md` (supersedes the blocker-fix list).
