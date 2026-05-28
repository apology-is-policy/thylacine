# 96 — Filesystem-mutation syscalls (the FS foundation)

**Status:** FS-alpha (`SYS_WALK_CREATE`) + FS-beta (`SYS_FSYNC` + `SYS_READDIR`)
LANDED + audit-CLEAN (opus prosecutor R1: 0 P0 / 0 P1 / 1 P2 / 3 P3 — the fid
lifecycle, buffer bounds, rights gates, and vtable ABI all sound; F1 doc + F2
reject-leak + F3 readdir-copy-order fixed, F4 already documented). 622/622 PASS
on default + UBSan. **KNOWN BLOCKER (not this code):** under ASan the first
runtime Thylacine->Stratum WRITE (the joey fs-mut create E2E) deterministically
fails `rc=-1` — heap-layout-sensitive (default + UBSan pass), i.e. the
documented Phase 6 AEGIS/mallocng write-path corruption (task #713). Root-cause
of #713 is the user-mandated gate before A-1b persistence. Design pin:
`IDENTITY-DESIGN.md §9.2`.

## Purpose

The create / durability / enumeration trio that turns the kernel's read-mostly
FS surface into a writable one. Pulled forward ahead of the corvus identity-DB
persistence (A-1b) per the convergence-detour sequencing: corvus's real
persistence needs to create files, fsync them, and enumerate a directory, and
the A-2 coreutils (`ls`, `mkdir`) plus the shell need the same shortly after.

The kernel 9P client already implements the wire half (`p9_client_lcreate` /
`p9_client_mkdir` / `p9_client_fsync` / `p9_client_readdir`) and the `Dev`
vtable already had a `.create` slot (a `dev9p_create` stub until FS-alpha), so
this layer is **syscall wrappers + the real `dev9p_create` + new `Dev` vtable
slots + rights gates + tests + audit**, not new protocol work.

## Public API

### `SYS_WALK_CREATE = 54` (FS-alpha)

```
SYS_WALK_CREATE(parent_fd, name_va, name_len, omode, perm) -> opened_fd / -1
  x0 = parent_fd   KOBJ_SPOOR with RIGHT_WRITE, or SYS_WALK_OPEN_FROM_ROOT
                   ((u64)-1) for the caller's Territory root.
  x1 = name_va     user-VA of the single component name.
  x2 = name_len    1 .. SYS_WALK_OPEN_NAME_MAX (64); reject '/' '\0' "." "..".
  x3 = omode       Plan 9 open mode for the returned fd (SYS_WALK_OPEN_OMODE_VALID
                   = OREAD/OWRITE/ORDWR + OTRUNC). A directory is opened OREAD.
  x4 = perm        u32 Plan 9 perm; low 9 bits = mode, DMDIR (0x80000000) bit
                   selects a directory. Bits outside SYS_WALK_CREATE_PERM_VALID
                   (0x800001FF) -> -1.
```

The create-then-open sibling of `SYS_WALK_OPEN`. Creates the single component
`name` inside the directory `parent_fd` and returns a **new opened
`KOBJ_SPOOR`** fd (rights `RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER`, matching
`SYS_WALK_OPEN`) referring to the created object — a file, or (when `perm`
carries `DMDIR`) a directory.

The created object's group is the **caller's `primary_gid`** (A-1a identity on
the Proc), carried into the 9P `Tlcreate`/`Tmkdir` `gid` field. Full owner
attribution (= caller `principal_id`) and per-file rwx **enforcement** are A-2
(A-2c mount-cape / A-2d kernel rwx); this layer is the create **mechanism**.
I-22 holds — there is no rwx enforcement yet to bypass.

libt: `t_walk_create(parent_fd, name, name_len, omode, perm)`
(`usr/lib/libt/include/thyla/syscall.h`). libthyla-rs:
`t_walk_create(parent_fd, name, name_len, omode, perm) -> i64`
(`usr/lib/libthyla-rs/src/lib.rs`), with `T_WALK_CREATE_DMDIR`.

### `Dev` vtable change (FS-alpha)

```c
struct Spoor *(*create)(struct Spoor *c, const char *name,
                        int omode, u32 perm, u32 gid);
```

Widened from the old `void (*create)(c, name, omode, perm)`: returns the opened
Spoor (`c`) on success or `NULL` on failure (mirrors `.open`), and takes the
creator's `gid`. Every Dev fills the slot (the §9.2 vtable-full-population
contract, `test_dev.vtable_slot_coverage`); all Devs except `dev9p` return
`NULL` (not creatable). The `gid` is an opaque number to the Dev layer —
identity-agnostic.

### `SYS_FSYNC = 55` + `SYS_READDIR = 56` (FS-beta)

```
SYS_FSYNC(fd, datasync) -> 0 / -1
  x0 = fd        KOBJ_SPOOR with RIGHT_WRITE (the write-side flush).
  x1 = datasync  0 = full (data + metadata), non-zero = data only.

SYS_READDIR(fd, buf_va, buf_len) -> bytes (>=0) / -1
  x0 = fd        KOBJ_SPOOR opened on a directory, RIGHT_READ.
  x1 = buf_va    user-VA out buffer.
  x2 = buf_len   1 .. SYS_RW_MAX (4096).
```

`SYS_FSYNC` is the durability barrier ("write-then-fsync = durable" on the
integrity FS); dispatches `dev->fsync` (dev9p -> `p9_client_fsync` -> Stratum
`Tsync`; in-memory-durable Devs no-op success). A NULL `.fsync` slot -> -1.

`SYS_READDIR` reads the next run of **raw 9P2000.L dirents** into `buf` and
advances the Spoor's offset; 0 bytes == end-of-directory. Each entry:
`qid(13) + offset(8 LE) + dtype(1) + name_len(2 LE) + name`. The Treaddir
`offset` is a **resume cookie** (the last returned entry's offset field), not a
byte position, so the handler parses the returned run for the last entry's
cookie and stores THAT in the Spoor offset (mirrors Linux v9fs). A NULL
`.readdir` slot -> -1.

Two new `Dev` vtable slots (NULL-permitted, like `.poll` / `.stat_native`):
`int (*fsync)(c, datasync)` and `long (*readdir)(c, buf, n, off)`. Only `dev9p`
sets a real `.readdir`; `dev9p` + `devramfs` (no-op) set `.fsync`. libt:
`t_fsync` / `t_readdir`. libthyla-rs: `t_fsync` / `t_readdir`.

## Implementation

### `kernel/syscall.c::sys_walk_create_handler`

Mirrors `sys_walk_open_handler` with three differences: (1) `RIGHT_WRITE` on the
parent (create mutates the directory) instead of `RIGHT_READ`; (2) a `perm`
validation against `SYS_WALK_CREATE_PERM_VALID`; (3) a **clone-walk** (`nname=0`)
to give the cloned `nc` its own fid pointing at the parent, then `dev->create`
instead of `dev->open`.

Sequence: validate name/omode/perm -> resolve parent (RIGHT_WRITE | FROM_ROOT)
-> require `dev->walk` + `dev->create` -> copy + shape-check the name ->
`spoor_clone(parent)` -> clone-walk -> `nc->dev->create(nc, name, omode, perm,
primary_gid)` -> `handle_alloc(KOBJ_SPOOR, R|W|TRANSFER)`.

**Cross-Dev clone-walk safety** (the first userspace path to call a Dev walk with
`nname==0`). Three safe shapes (F1 audit — note `devramfs` is in the REUSE
bucket, not the reject bucket):
- **(a) leaf Devs** (`cons`/`null`/`zero`/`full`/`random`/`pipe`/`notes`/`none`)
  return `NULL` -> the walk-fail cleanup (`nc->aux = NULL; spoor_unref(nc)`).
- **(b) reuse-nc Devs** (`devcap`/`devsrv`/`devramfs` return `w->spoor == nc`,
  nqid==0): the create call proceeds but their create stub returns `NULL`, and
  the cloned Spoor carries `aux==NULL` (or a `dev_simple_close` no-op), so the
  eventual `spoor_clunk(nc)` is harmless. (Post-16b-gamma `devramfs_walk` REUSES
  a non-NULL `nc`; the handler always passes one — so devramfs is here, made safe
  by its NULL create, NOT by the `w->spoor != nc` reject.)
- **(c) self-cloning dir Devs** (`devproc`/`devctl` IGNORE nc and clone
  internally) return `w->spoor != nc` -> the reject path, which clunks the
  leaked clone (F2 audit) then unrefs `nc`.

Only `dev9p` replaces `nc->aux` with a fresh fid. (The identical `w->spoor != nc`
leak exists pre-existing in `SYS_WALK_OPEN`'s F4 path; unreachable at v1.0 — no
writable self-cloning Dev is user-exposed — so it is deferred there, fixed here.)

### `kernel/dev9p.c::dev9p_create`

`c`'s fid is a private clone at the parent dir (the handler clone-walked it).
- **File** (`perm & DMDIR == 0`): `p9_client_lcreate(client, c->fid, name,
  name_len, flags, mode=perm&0777, gid)` — creates AND opens; afterward `c->fid`
  refers to the new file. `flags` = `omode & 3` (+ `O_TRUNC` if `OTRUNC`).
- **Directory** (`perm & DMDIR`): `p9_client_mkdir(client, c->fid [parent],
  name, mode, gid)` leaves the fid at the parent, so the impl allocs a fresh
  `dir_fid`, `p9_client_walk(parent -> dir_fid, [name])`, clunks the parent
  clone, adopts `dir_fid`, and `p9_client_lopen(dir_fid, OREAD)`.

On any 9P failure `dev9p_create` returns `NULL`; the fid it owns at that point
(parent clone, or `dir_fid` after the swap) is clunked by `dev9p_close` when the
handler `spoor_clunk`s `nc`. No double-clunk, no fid leak on the failure paths.

## Error paths (`SYS_WALK_CREATE -> -1`)

- parent not `KOBJ_SPOOR` / missing `RIGHT_WRITE`; FROM_ROOT with no pivoted root
- backing Dev has no `.walk` or no `.create`; Dev is not creatable (`create`
  returns NULL — every Dev except dev9p)
- `name_len` 0 / > 64 / contains `/` or `\0` / is `.` or `..`
- `omode` outside `SYS_WALK_OPEN_OMODE_VALID`
- `perm` outside `SYS_WALK_CREATE_PERM_VALID` (a reserved `DM*` bit)
- the 9P server rejects (name exists, no space, permission) -> Rlerror
- handle table full

## Tests

- `kernel/test/test_dev9p.c::test_dev9p_create_file` — clone-walk + `dev9p_create`
  drives `Tlcreate` against the loopback responder (qid 0x77); asserts the
  returned Spoor is opened (`COPEN`) with the Rlcreate qid.
- `kernel/test/test_dev9p.c::test_dev9p_create_dir` — `perm & DMDIR` drives
  `Tmkdir` + walk + `Tlopen`; asserts the returned Spoor is opened.
- `kernel/test/test_dev.c::vtable_slot_coverage` — every Dev still fills `.create`
  (now the widened signature); `devnone.create(...) == NULL`.
- `kernel/test/test_dev9p.c::test_dev9p_fsync` — `dev9p_fsync` -> `Tsync` (full +
  datasync) returns 0.
- `kernel/test/test_dev9p.c::test_dev9p_readdir` — `dev9p_readdir` -> `Treaddir`
  returns the responder's 27-byte dirent ("foo"); asserts the dirent layout.
- The loopback responder (`dev9p_responder`) gained `Tlcreate` / `Tmkdir` /
  `Tfsync` / `Treaddir` cases.
- **joey end-to-end probe** (`usr/joey/joey.c`, post-pivot) — against the REAL
  disk-backed Stratum FS: `SYS_WALK_CREATE` a file + `SYS_WRITE` + `SYS_FSYNC` +
  `SYS_WALK_OPEN` read-back + verify; then mkdir-via-DMDIR + `SYS_READDIR` +
  the cookie-advance EOD check (a non-empty first run must return 0 on the next
  call). This covers the syscall HANDLERS (clone-walk orchestration, the
  readdir cookie parse, rights gates) end-to-end through stratumd, which the
  loopback unit tests do not reach. Confirmed at boot: `fs-mut
  create+write+fsync+read-back OK` + `fs-mut mkdir+readdir OK (d1=51 bytes)`.

## Known caveats / footguns

- **Ownership-on-create is a seam at this layer.** Only `primary_gid` is carried
  to the 9P `gid` field; owner attribution and rwx enforcement land in A-2.
- **`mkdir` is folded into create** via the `DMDIR` perm bit (Plan 9 idiom) —
  there is no separate `SYS_MKDIR`. A directory create returns an fd opened
  `OREAD` on the new directory (you `readdir` it; you never write it).
- **A failed directory create can leave the directory on disk** with no fd (if
  the post-`Tmkdir` walk/open fails). Benign partial-success; the caller sees -1.
