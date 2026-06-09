# 96 — Filesystem-mutation syscalls (the FS foundation)

**Status:** FS-alpha (`SYS_WALK_CREATE`) + FS-beta (`SYS_FSYNC` + `SYS_READDIR`)
+ FS-gamma (`SYS_RENAME` + `SYS_UNLINK`) LANDED. FS-alpha+beta audit-CLEAN (opus
prosecutor R1: 0 P0 / 0 P1 / 1 P2 / 3 P3 — fid lifecycle, buffer bounds, rights
gates, vtable ABI all sound). FS-gamma adds the atomic `rename` + `unlink` that
A-1b's corvus identity-DB persistence uses for its write-tmp → fsync →
rename-swap → dir-fsync sequence; it is **audit-CLEAN too** (opus prosecutor R1:
0 P0 / 0 P1 / 0 P2 / 3 P3, all informational/pre-existing — synchronous-client
no-op guard, the pre-existing surface-wide `handle_get` TOCTOU not worsened, a
server-delegated `QTDIR` check; no code change), and is the first end-to-end
exercise of `p9_client_renameat` / `unlinkat`. 624/624 PASS on default + UBSan. **The
Phase-6 "AEGIS/mallocng content-sensitive write-path corruption" that once gated
this work was root-caused (task #713) as an `eret`-window IRQ race in the
EL0-entry trampolines — NOT a write-path or heap bug — and fixed (`cc8a9bd`); the
create / write / fsync / rename path is sound, and there is no kernel ASan (the
old "fails under ASan" framing was invalid; the tree only has UBSan).** Design
pins: `IDENTITY-DESIGN.md §9.2` (alpha+beta) + `§9.3` (gamma).

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

The cookie is an **opaque u64** carried through the SIGNED `s64 Spoor.offset`
field: Stratum derives cookies from an entry hash, so real dirents routinely
exceed `INT64_MAX` (bit 63 set). `dev9p_readdir` therefore reinterprets the
stored bits straight back to `u64` (`(u64)off`) and does **not** apply the
`(off < 0) ? 0` clamp that the byte-offset Devs (`dev9p_read`/`_write`) use --
clamping a high-bit cookie to 0 restarts enumeration and a paginating reader
re-fetches batch 0 forever (#955). The handler also reports EOD if a non-empty
run's last cookie equals the resume cookie (a non-advancing cursor would
otherwise spin a reader) -- defense-in-depth against a buggy/hostile server.

Two new `Dev` vtable slots (NULL-permitted, like `.poll` / `.stat_native`):
`int (*fsync)(c, datasync)` and `long (*readdir)(c, buf, n, off)`. `dev9p`
(Stratum) and, since U-6e-b-1, `devramfs` (flat-root enumeration) set a real
`.readdir`; `dev9p` + `devramfs` (no-op) set `.fsync`. (`devramfs_readdir`
returns -1 -- not 0 -- when the first entry of a run cannot fit the caller's
buffer, the getdents/EINVAL convention, so a too-small buffer is not mistaken
for EOD; see `docs/reference/34-devramfs.md`. Enumerating a *mounted* `/srv` or
`/proc` needs devsrv/devproc `.readdir` -- task #932.) libt: `t_fsync` /
`t_readdir`. libthyla-rs: `t_fsync` / `t_readdir` + the `fs::read_dir` /
`ReadDir` / `DirEntry` streaming iterator (U-6e-b-1, over the stalk resolver so
it opens the root `/` and multi-component paths).

### `SYS_RENAME = 57` + `SYS_UNLINK = 58` (FS-gamma)

```
SYS_RENAME(olddir_fd, oldname_va, oldname_len, newdir_fd, newname_va, newname_len) -> 0 / -1
  x0 = olddir_fd   KOBJ_SPOOR directory, RIGHT_WRITE (or FROM_ROOT).
  x1/x2 = oldname  user-VA + len (1 .. 64; reject '/' '\0' "." "..").
  x3 = newdir_fd   KOBJ_SPOOR directory, RIGHT_WRITE (or FROM_ROOT). Same Dev.
  x4/x5 = newname  user-VA + len.

SYS_UNLINK(parent_fd, name_va, name_len, flags) -> 0 / -1
  x0 = parent_fd   KOBJ_SPOOR directory, RIGHT_WRITE (or FROM_ROOT).
  x1/x2 = name     user-VA + len (1 .. 64; reject '/' '\0' "." "..").
  x3 = flags       0 = unlink a non-directory; SYS_UNLINK_REMOVEDIR (0x200,
                   == P9_UNLINK_AT_REMOVEDIR) = rmdir an empty directory.
                   Other bits -> -1.
```

`SYS_RENAME` atomically renames/moves a single component (POSIX `rename(2)` / 9P
`Trenameat`): an existing destination is **atomically replaced** — the property
A-1b's identity-DB swap relies on (`rename(identity.db.tmp → identity.db)`).
Both directories must be on the **same Dev** (a 9P `renameat` is within one
server; cross-Dev → -1) and, for `dev9p`, the same `p9_client` session.

`SYS_UNLINK` removes a non-directory, or (with `SYS_UNLINK_REMOVEDIR`) an empty
directory (9P `Tunlinkat`).

**Unlike `SYS_WALK_CREATE`, neither clone-walks.** `Trenameat` / `Tunlinkat`
operate on the dirfid(s) **by name** without consuming or transitioning them
(like `Tsync` / `Treaddir`), so the handlers run the Dev op directly on the
looked-up dir Spoor(s) — the `SYS_FSYNC` / `SYS_READDIR` pattern. They borrow the
caller's dir fid and allocate no transient fid, so the FS-alpha failed-create
fid-leak class structurally cannot arise.

Two more `Dev` vtable slots (NULL-permitted): `int (*rename)(olddir, oldname,
newdir, newname)` and `int (*unlink)(parent, name, flags)`. Only `dev9p` sets
them (→ `p9_client_renameat` / `unlinkat`); the pre-existing `void (*remove)`
Plan 9 slot is wrong-shaped (no name, no error return) and left a no-op stub.
libt: `t_rename` / `t_unlink` (+ `T_UNLINK_REMOVEDIR`). libthyla-rs: `t_rename` /
`t_unlink` (+ `T_UNLINK_REMOVEDIR`).

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

### `kernel/syscall.c::sys_rename_handler` / `sys_unlink_handler` (FS-gamma)

Both share two static helpers: `sys_resolve_dir_wr(p, fd_raw)` (FROM_ROOT → the
Territory `root_spoor`; else `sys_lookup_spoor(RIGHT_WRITE)`) and
`sys_copy_component(name_va, name_len, scratch)` (the strict single-component
copy + validate `SYS_WALK_CREATE` does inline — reject empty / >64 / `/` / `\0`
/ `.` / `..`).

`sys_rename_handler`: copy + validate both names (cheap rejects first) → resolve
both dir fds (`RIGHT_WRITE`) → require `od->dev->rename` → **cross-Dev reject**
(`od->dev != nd->dev` → -1, before any Dev op) → `od->dev->rename(od, oldname,
nd, newname)`. No clone-walk; the looked-up Spoors are used directly.

`sys_unlink_handler`: validate `flags` against `{0, SYS_UNLINK_REMOVEDIR}` (any
other bit → -1) → copy + validate the name → resolve the parent (`RIGHT_WRITE`)
→ require `c->dev->unlink` → `c->dev->unlink(c, name, flags)`.

### `kernel/dev9p.c::dev9p_rename` / `dev9p_unlink`

`dev9p_rename` does `priv_of(olddir)` + `priv_of(newdir)`, adds the **same-`p9_client`
guard** (`od->client != nd->client` → -1; two dev9p mounts are distinct sessions,
and a 9P `renameat` is within one), then `p9_client_renameat(client, od->fid,
oldname, nd->fid, newname)`. `dev9p_unlink` does `priv_of(parent)` then
`p9_client_unlinkat(client, parent->fid, name, flags)`. The `flags` arg passes
straight to the wire — a `_Static_assert(SYS_UNLINK_REMOVEDIR ==
P9_UNLINK_AT_REMOVEDIR)` (in `dev9p.c`, the only TU that sees both) pins the
equality.

## Error paths (`SYS_WALK_CREATE -> -1`)

- parent not `KOBJ_SPOOR` / missing `RIGHT_WRITE`; FROM_ROOT with no pivoted root
- backing Dev has no `.walk` or no `.create`; Dev is not creatable (`create`
  returns NULL — every Dev except dev9p)
- `name_len` 0 / > 64 / contains `/` or `\0` / is `.` or `..`
- `omode` outside `SYS_WALK_OPEN_OMODE_VALID`
- `perm` outside `SYS_WALK_CREATE_PERM_VALID` (a reserved `DM*` bit)
- the 9P server rejects (name exists, no space, permission) -> Rlerror
- handle table full

## Error paths (`SYS_RENAME` / `SYS_UNLINK` -> -1)

- either dir fd not `KOBJ_SPOOR` / missing `RIGHT_WRITE`; FROM_ROOT with no
  pivoted root
- name bounds: 0 / > 64 / contains `/` or `\0` / is `.` or `..` (either name)
- rename: the two dir fds are on different Devs (cross-Dev), or different `dev9p`
  sessions (same Dev, different `p9_client`)
- backing Dev has no `.rename` (rename) / `.unlink` (unlink)
- unlink: `flags` has any bit outside `SYS_UNLINK_REMOVEDIR`
- the 9P server rejects (`Rlerror`): rename — source `ENOENT`, dest is a
  non-empty directory, `EXDEV`, permission; unlink — `ENOENT`, `ENOTEMPTY`
  (rmdir on a non-empty dir), `EISDIR`/`ENOTDIR` mode mismatch, permission

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
- `kernel/test/test_dev9p.c::test_dev9p_readdir_cookie_high_bit` (#955 regression)
  — drives `dev9p_readdir` with a resume cookie that has bit 63 set (negative as
  `s64`) and asserts the responder saw the cookie **verbatim** on the wire (not
  sign-clamped to 0). Fails on the pre-fix `(off < 0) ? 0` clamp.
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
- `kernel/test/test_dev9p.c::test_dev9p_rename` — `dev9p_rename` → `Trenameat`
  (responder `Rrenameat`, header-only) returns 0 for a same-dir same-session
  rename. `test_dev9p_unlink` — `dev9p_unlink` → `Tunlinkat` returns 0 for both
  the file (flags 0) and rmdir (`P9_UNLINK_AT_REMOVEDIR`) cases. The loopback
  responder gained `Trenameat` / `Tunlinkat` cases.
- **joey FS-gamma end-to-end probe** (`usr/joey/joey.c`, post-pivot) — against
  the REAL Stratum FS, fully idempotent (cleanup-first + cleanup-after, so it
  leaves no artifact on the persistent pool): create src + create dst with
  *different* bytes → `SYS_RENAME(src → dst)` → verify dst now holds src's bytes
  (**atomic replace**) + src is gone → `SYS_UNLINK(dst)` → verify gone; then
  mkdir → `SYS_UNLINK(REMOVEDIR)` → verify gone. Confirmed at boot: `fs-gamma
  rename(atomic-replace) + unlink OK` + `fs-gamma rmdir(REMOVEDIR) OK`. This is
  the first end-to-end exercise of `p9_client_renameat` / `unlinkat`.

## libthyla-rs userspace wrappers (LS-3b)

The path-based, `std::fs`-shaped wrappers the native coreutils call. They live in
`usr/lib/libthyla-rs/src/fs/mod.rs` and drive the kernel syscalls above:

```rust
fs::create_dir(path)   -> SYS_WALK_CREATE(parent, name, OREAD, DMDIR|0o755)
fs::remove_file(path)  -> SYS_UNLINK(parent, name, 0)
fs::remove_dir(path)   -> SYS_UNLINK(parent, name, REMOVEDIR)
fs::rename(from, to)   -> SYS_RENAME(from_parent, from_name, to_parent, to_name)
```

All four resolve the parent directory through one shared helper,
`fs::file::with_parent_dir(path, |parent_fd, basename| ...)`, which walks the
parent chain and hands the closure the parent dir fd + the final component name
(the `(parent, name)` shape these syscalls take). **The intermediates are walked
with `T_OPATH`, not `T_OREAD`** — two reasons, both load-bearing:

1. **RIGHT_WRITE on the parent.** `sys_walk_create_handler` / `sys_unlink_handler`
   / `sys_rename_handler` resolve the parent fd with `RIGHT_WRITE` (the directory
   is mutated). Since A-3b an `T_OREAD` open yields a `RIGHT_READ`-only handle,
   which those handlers reject; `T_OPATH` is born `RIGHT_READ | RIGHT_WRITE` (the
   navigation/capability base), so the final parent is a valid mutation target.
2. **X-only traversal.** `T_OPATH` is exempt from the open-mode R/W `perm_check`
   but still X-searched per component (POSIX: you need search, not read, to
   traverse a directory). `T_OREAD` would wrongly require read on each ancestor.

This is the same discipline `File::open_create_at_path` uses, and switching *its*
intermediate walk from `T_OREAD` to `T_OPATH` (same chunk) **fixed a latent
`File::create` depth>=2 bug**: a `File::create("/a/b/c")` previously walked `/a/b`
with `T_OREAD` (RIGHT_READ-only) and then failed the `SYS_WALK_CREATE` parent
`RIGHT_WRITE` gate. The fix was pulled forward as a proper-completion dependency
of `touch`/`cp`/`tee` (which create files, often at depth).

`with_parent_dir` closes the owned parent handle after the closure returns (success
or error); `FROM_ROOT` is used for a single-component path (the syscalls resolve it
to the Territory root). `fs::rename` nests two `with_parent_dir` calls so both
parents are held open across the rename. **Recursion footgun for callers**:
`fs::read_dir` yields `.` and `..` (Stratum returns them as cookies 1/2), so a
recursive `rm -r` / `cp -r` MUST skip them — the coreutils do.

**Coreutils (`usr/coreutils/src/bin/`)**: `mkdir` (`-p`), `rmdir`, `rm` (`-r`/`-f`),
`touch`, `cp` (`-r`), `mv`, `tee` (`-a`). `touch` uses `OpenOptions{write,create}`
(create-or-open, no truncate). `mv` is `fs::rename` (same-Dev only at v1.0; a
cross-Dev move would need copy+remove — not emulated).

**Always-on regression**: `usr/fs-mut-smoke` — joey spawns it post-pivot (the
writable Stratum FS) and gates the boot on a 0 exit. 13 checks: `create_dir` at
depth, **depth-3 `File::create` + write + read-back** (the fix), `rename`
atomic-replace, `remove_file` / `remove_dir`, and `NotFound` on a removed path.
Idempotent (reclaims a stale `/fs-mut-smoke` tree before building a fresh one, since
the pool persists). The interactive companion is `tools/interactive/ls-3b.exp`.

## Known caveats / footguns

- **Ownership-on-create is a seam at this layer.** Only `primary_gid` is carried
  to the 9P `gid` field; owner attribution and rwx enforcement land in A-2.
- **`mkdir` is folded into create** via the `DMDIR` perm bit (Plan 9 idiom) —
  there is no separate `SYS_MKDIR`. A directory create returns an fd opened
  `OREAD` on the new directory (you `readdir` it; you never write it).
- **A failed directory create can leave the directory on disk** with no fd (if
  the post-`Tmkdir` walk/open fails). Benign partial-success; the caller sees -1.
- **Readdir cookies are opaque u64; never sign-clamp them** (#955). The Spoor's
  `offset` field is `s64` and shared with byte I/O, but for a directory it holds
  a resume cookie that may have bit 63 set -- reinterpret, don't clamp. The
  non-advancing-cursor EOD guard bounds a *stuck* cursor, but a hostile/buggy
  server with an unbounded synthetic directory (cookies that strictly advance
  forever, never reaching EOD) can still spin a reader. At v1.0 stratumd is
  trusted; a bounded-readdir posture for untrusted servers is a v1.x seam.
- **`SYS_RENAME` / `SYS_UNLINK` deliberately do NOT clone-walk** (unlike
  `SYS_WALK_CREATE`). `Trenameat` / `Tunlinkat` act on the dirfid by name and
  never transition it, so the direct form is both simpler and correct — and it
  means these ops allocate no transient fid, so the FS-alpha failed-create
  fid-leak class cannot arise. The original `§9.3` pin specified a clone-walk;
  that was corrected during impl (it was unnecessary).
- **Rename durability needs a dir-fsync.** `SYS_RENAME` makes the name swap
  atomic, but the dirent change's *durability* needs a barrier on the containing
  directory. A durable-rename caller (corvus's identity-DB swap, A-1b) must
  follow `SYS_RENAME` with `SYS_FSYNC` on the parent dir fd (→ `Tsync` on the
  dir). Whether Stratum honors `Tsync`-on-a-directory as a metadata barrier is
  validated end-to-end by the A-1b cross-reboot persistence test.
- **Cross-Dev / cross-session rename is rejected, not emulated.** Moving a file
  between two different mounts is not a single 9P `renameat`; the caller must
  copy + unlink. v1.0 returns -1.
