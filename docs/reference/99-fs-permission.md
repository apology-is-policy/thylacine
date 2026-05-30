# 99 — File permission + ownership surface (A-2)

As-built reference for the kernel's per-file ownership + permission surface.
Audience: developers, auditors. Binding per the reference-doc discipline.

This doc covers **A-2a** (the metadata read+write *mechanism*): `struct t_stat`'s
owner/group fields, `dev9p`'s `Tgetattr`-backed `stat_native`, and `SYS_WSTAT`
(chmod/chown over `Tsetattr`). The kernel rwx **enforcement** layer (A-2d) is
**not yet built** — see "Status" and IDENTITY-DESIGN.md §3.7 + §9.5.

Design: IDENTITY-DESIGN.md §3.4 (mode-bit vocabulary), §3.5 (owner-on-create),
§3.7 (the kernel-VFS enforcement model, A-2d), §9.5 (the A-2a ABI).

---

## Purpose

A 9P server is not guaranteed to enforce file rwx, and Stratum (the reference
backing) does not (IDENTITY-DESIGN.md §3.7). Thylacine therefore enforces
permission in the **kernel**, at the single FS-access chokepoint every op already
passes through — the Linux-VFS model, the only layer uniform across heterogeneous
backings. That enforcement is A-2d. A-2a builds the **metadata plumbing** it
needs: the kernel must be able to *read* a file's owner/group/mode (`stat_native`
→ `t_stat`) and *write* them (`SYS_WSTAT` → `Tsetattr`) before it can check them.

---

## Public API

### Syscalls

```
SYS_FSTAT(spoor_fd, stat_va)          -> 0 / -1     # now fills t_stat.uid/gid
SYS_WSTAT(fd, valid, mode, uid, gid)  -> 0 / -1     # A-2a; chmod/chown
```

`SYS_WSTAT = 59`. Register-passed; no user buffer. `valid` is a bitmask of
`T_WSTAT_MODE` (1<<0) / `T_WSTAT_UID` (1<<1) / `T_WSTAT_GID` (1<<2). At least one
bit; any other bit rejects. `mode` carries the 9 rwx bits only — setuid/setgid/
sticky + any bit outside `0777` reject (setuid is explicitly unsupported,
IDENTITY-DESIGN.md §S5). `uid == PRINCIPAL_INVALID` / `gid == GID_INVALID` reject.

### libt / libthyla-rs wrappers

```c
long t_wstat(long fd, unsigned long valid, unsigned long mode,
             unsigned long uid, unsigned long gid);
long t_chmod(long fd, unsigned long mode);            // t_wstat(fd, T_WSTAT_MODE, ...)
long t_chown(long fd, unsigned long uid, unsigned long gid); // T_WSTAT_UID|GID
```

```rust
pub unsafe fn t_wstat(fd: i64, valid: u32, mode: u32, uid: u32, gid: u32) -> i64;
// t::fs::Metadata::uid() / gid()  (read side)
```

### Dev vtable

```c
int (*stat_native)(struct Spoor *c, struct t_stat *out);            // read (existing slot)
int (*wstat_native)(struct Spoor *c, u32 valid, u32 mode,           // write (A-2a; NULL-permitted)
                    u32 uid, u32 gid);
```

`wstat_native` NULL ⇒ `SYS_WSTAT` returns -1 (the Dev has no setattr backing).

---

## Data structures

### `struct t_stat` (80 bytes — A-2a grew it from 72)

`kernel/include/thylacine/syscall.h`. The SYS_FSTAT ABI record. A-2a appended two
`u32` after the 72-byte 16b-gamma tail (existing offsets unchanged):

| off | field | note |
|----:|-------|------|
| 0 | `u64 size` | |
| 8 | `u64 qid_path` | |
| 16/24/32 | `atime/mtime/ctime_sec` | 0 at v1.0 |
| 40 | `u32 mode` | POSIX mode bits |
| 44 | `u32 nlink` | |
| 48 | `u32 qid_vers` | |
| 52 | `u8 qid_type` (+3 pad) | |
| 56 | `u32 blksize` (+4 pad) | |
| 64 | `u64 blocks` | |
| **72** | **`u32 uid`** | **A-2a: owner principal-id** |
| **76** | **`u32 gid`** | **A-2a: owning group** |

Size + every offset pinned by `_Static_assert`. Mirrored byte-for-byte in the
libt `struct t_stat`, the libthyla-rs `Metadata`, and the pouch `0010` patch's
hand-rolled `struct t_stat` (all rebuilt in lockstep; no persistent consumer).

### `T_WSTAT_*` valid-mask (== `P9_SETATTR_*`)

`T_WSTAT_MODE`/`UID`/`GID` = `1<<0`/`1<<1`/`1<<2`, chosen equal to the wire
`P9_SETATTR_MODE`/`UID`/`GID` so `dev9p_wstat_native` maps with no translation.
The equality is pinned by three `_Static_assert`s in `dev9p.c` — the only TU that
includes both `<thylacine/syscall.h>` and `<thylacine/9p_wire.h>`.

---

## Implementation

### Read side — `dev9p_stat_native` (`kernel/dev9p.c`)

`priv_of(c)` → `p->client`/`p->fid`; `p9_client_getattr(client, fid,
P9_GETATTR_BASIC, &attr)`; zero `*out`; map `attr.{size,qid,mode,nlink,uid,gid,
times,blocks}` into `t_stat`, with `qid_type = qid_type_p9_to_kernel(attr.qid.type)`
and `blksize` defaulting to 4096. Wired into the `dev9p` Dev struct (`.stat_native`
was previously absent — only the `.stat` -1 stub existed). `devramfs_stat_native`
(`kernel/devramfs.c`) stamps `uid = PRINCIPAL_SYSTEM`, `gid = GID_SYSTEM` in both
the root-dir and file branches.

### Write side — `dev9p_wstat_native` (`kernel/dev9p.c`)

`priv_of(c)`; zero a `struct p9_setattr`; copy `valid`/`mode`/`uid`/`gid` straight
in (the no-translation map); `p9_client_setattr(client, fid, &sa)`. Borrows the
caller's fid; allocates no transient fid.

### Handler — `sys_wstat_handler` (`kernel/syscall.c`)

1. `current_thread()` → Proc.
2. Mask sanity: `valid != 0` and `(valid & ~T_WSTAT_VALID) == 0`.
3. Per-field bounds: `T_WSTAT_MODE` ⇒ `(mode & ~0777) == 0`; `T_WSTAT_UID` ⇒
   `uid != PRINCIPAL_INVALID`; `T_WSTAT_GID` ⇒ `gid != GID_INVALID`. A field whose
   valid bit is clear is forced to 0 before the Dev call.
4. `sys_lookup_rw_handle(p, fd, RIGHT_WRITE)`; reject non-`KOBJ_SPOOR`.
5. `spoor_wstat_native(c, valid, mode, uid, gid)` → `dev->wstat_native` (NULL ⇒ -1).

Validation precedes the handle lookup; the value checks are **structural** (mask
shape, mode-bit hygiene, sentinel reject), NOT the permission policy — who may
chmod/chown is A-2d.

---

## Tests

| Test | File | Covers |
|------|------|--------|
| `dev9p.stat_native_maps_getattr` | `test_dev9p.c` | loopback Rgetattr → t_stat.{uid,gid,mode,size} |
| `dev9p.wstat_native_drives_setattr` | `test_dev9p.c` | T_WSTAT_* mask + mode/uid/gid reach the Tsetattr wire intact |
| `devramfs.stat_native_system_owned` | `test_devramfs.c` | root + file report PRINCIPAL_SYSTEM/GID_SYSTEM |
| joey `/system.key` A-2a probe | `usr/joey/joey.c` | end-to-end: SYS_FSTAT uid/gid = system; SYS_WSTAT rejects empty-mask / reserved-bit / setuid-mode / chmod-on-read-only-Dev |

Compile-time: the `t_stat` size + offset `_Static_assert`s (kernel + libt +
libthyla-rs `const _: () = assert!`) and the `T_WSTAT_* == P9_SETATTR_*` asserts.

**Not covered at A-2a:** a positive chmod/chown round-trip against *real* Stratum
(the loopback proves the wire; the positive on-disk round-trip rides on the A-2
`chmod`/`chown` coreutils). The kernel rwx enforcement (A-2d) is unbuilt.

---

## Error paths

| Return | Trigger |
|--------|---------|
| `SYS_WSTAT` -1 | fd not KOBJ_SPOOR / missing RIGHT_WRITE; `valid == 0`; reserved valid bit; `mode & ~0777`; uid/gid INVALID; Dev has no `.wstat_native`; server Rlerror |
| `SYS_FSTAT` -1 | (unchanged) bad fd / no `stat_native` / Dev error |

---

## Status

- **A-2a (this doc): LANDED.** Mechanism complete — `t_stat` owner/group,
  `dev9p` getattr/setattr, `SYS_WSTAT`, consumers, tests.
- **A-2b** (owner-on-create *semantics* beyond the existing `primary_gid` stamp):
  largely a seam at v1.0 — a dev9p file's owner is the connection identity (A-3).
- **A-2c** (mount-cape metadata source for permissionless backings): dormant —
  no non-POSIX backing exists in the tree yet; folds into A-2d.
- **A-2d (the kernel rwx-enforcement layer): NOT BUILT.** Scripture-first — the
  `inode_permission`-equivalent at walk/open/create + the chmod/chown ownership-
  change policy (planned: chown = `CAP_HOSTOWNER`, chmod = owner|`CAP_HOSTOWNER`,
  chgrp = owner-to-own-group; Plan 9 heritage) land as an IDENTITY-DESIGN.md §3.7
  refinement before any code. First real exercise of I-22.

---

## Known caveats / footguns

- **t_stat is 80 bytes now.** Any new user of the SYS_FSTAT ABI must size its
  buffer at 80, not 72. The pouch `0010` patch's stack `struct t_stat` was grown
  in lockstep — the kernel writes 80 bytes and a 72-byte buffer would be a stack
  overflow. The `_Static_assert`s catch a stale mirror at build time.
- **A-2a chmod/chown is unenforced.** `SYS_WSTAT` gates only on the handle's
  `RIGHT_WRITE`; against Stratum (which applies `Tsetattr` unconditionally) a
  chmod/chown *succeeds*. The owner-only/privileged policy is A-2d. Do not assume
  A-2a's mechanism implies access control.
- **dev9p uid/gid is the connection identity** until A-3 (per-user stratumd). A
  single boot connection presents one identity for the whole FS at v1.0.
- **devramfs is system-owned + has no `.wstat_native`** — chmod/chown on a boot-FS
  file always returns -1 (read-only). This is correct, not a gap.
