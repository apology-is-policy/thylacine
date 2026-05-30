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
shape, mode-bit hygiene, sentinel reject); the permission *policy* — who may
chmod/chown — is the A-2d layer below.

### Enforcement (A-2d) — `kernel/perm.c` + the syscall chokepoints

The Linux-VFS model (IDENTITY-DESIGN.md §3.7 / §3.7.1): the kernel enforces
owner/group/other rwx at the FS-access chokepoint, since no backing self-enforces
file rwx (Stratum enforces dataset-scope only). I-22 holds — no `principal_id`
bypasses; only `CAP_HOSTOWNER` (a console-gated capability, never an identity) is
the DAC-override.

Helpers (`kernel/perm.c`; `PERM_R=4`/`PERM_W=2`/`PERM_X=1` positioned to match the
rwx triple):
- `proc_in_group(p, gid)` — `gid == primary_gid || gid ∈ supp_gids[0..count)`;
  `GID_INVALID` never matches.
- `perm_check(p, st, want)` — owner-first POSIX: `CAP_HOSTOWNER` ⇒ allow; else
  owner bits if `principal_id == st->uid`, else group bits if `proc_in_group`,
  else other bits; allow iff `(bits & want) == want`. Owner-first is
  authoritative (owner judged on owner bits only).
- `perm_want_for_omode(omode)` — OREAD→R, OWRITE→W, ORDWR→R|W, OEXEC→X, +OTRUNC→W.
- `perm_wstat_check(p, cur_uid, valid, new_gid)` — the ownership-change policy:
  MODE ⇒ owner|`CAP_HOSTOWNER`; UID ⇒ `CAP_HOSTOWNER` only (no give-away); GID ⇒
  (owner ∧ member of `new_gid`) | `CAP_HOSTOWNER`.

Chokepoints (`kernel/syscall.c`), each gated on `src->dev->perm_enforced`:
- `sys_walk_open_handler`: **X** on `src` (search) before the walk; **R/W** per
  `omode` on the walked target before `dev->open` (skipped for `O_PATH`, which has
  no access semantics — the X-search still applies).
- `sys_walk_create_handler`: **W|X** on the parent dir before the create.
- `sys_wstat_handler`: `perm_wstat_check` against the file's current owner, before
  the `dev->wstat_native` call.

All checks read the file metadata via `spoor_stat_native` and **fail closed** if
the Dev cannot stat. The check is additive to the handle RIGHT (capability axis);
both must pass.

**`Dev.perm_enforced`** decides whether a backing is enforced. `devramfs = true`
(system-owned, world-r/x → the `PRINCIPAL_SYSTEM` boot chain owns everything it
touches; a non-system principal gets other-r/x but not write). `dev9p = false` —
enforcement **deferred to A-3**: ground truth (the host-bake stamps pool entries
owned by the host uid, 0644/0755; the boot chain is `PRINCIPAL_SYSTEM` without
`CAP_HOSTOWNER`) shows uniform dev9p enforcement would brick the post-pivot creates
(`/var/lib/corvus`, the cross-reboot `susan`). dev9p stays handle-RIGHT-gated only
at v1.0; the A-3 activation flips this one flag. Because devramfs has no
`wstat_native` and dev9p is deferred, the **`perm_wstat_check` policy is dormant in
production at v1.0** (unit-tested via the helper; activated with dev9p at A-3).

---

## Tests

| Test | File | Covers |
|------|------|--------|
| `dev9p.stat_native_maps_getattr` | `test_dev9p.c` | loopback Rgetattr → t_stat.{uid,gid,mode,size} |
| `dev9p.wstat_native_drives_setattr` | `test_dev9p.c` | T_WSTAT_* mask + mode/uid/gid reach the Tsetattr wire intact |
| `devramfs.stat_native_system_owned` | `test_devramfs.c` | root + file report PRINCIPAL_SYSTEM/GID_SYSTEM |
| joey `/system.key` A-2a probe | `usr/joey/joey.c` | end-to-end: SYS_FSTAT uid/gid = system; SYS_WSTAT rejects empty-mask / reserved-bit / setuid-mode / chmod-on-read-only-Dev |
| `perm.check_owner_group_other` | `test_perm.c` | owner/group/other (+ supp) branch selection on r/w/x |
| `perm.check_owner_first` | `test_perm.c` | owner-first authority (owner denied where group/other would grant) |
| `perm.check_hostowner_override` | `test_perm.c` | `CAP_HOSTOWNER` DAC-override; `PRINCIPAL_SYSTEM` gets no ambient bypass (I-22) |
| `perm.in_group` | `test_perm.c` | primary/supp membership; `GID_INVALID` never matches |
| `perm.want_for_omode` | `test_perm.c` | OREAD/OWRITE/ORDWR/OEXEC + OTRUNC → rwx |
| `perm.wstat_policy` | `test_perm.c` | chmod owner-only; no-give-away chown; chgrp-to-own-group; `CAP_HOSTOWNER` any |
| `perm.devramfs_enforced_real_metadata` | `test_perm.c` | real initrd: system traverses, user gets other-r/x not write; owner/other branch off the file's actual mode |
| `perm.dev_flags` | `test_perm.c` | `devramfs.perm_enforced == true`, `dev9p.perm_enforced == false` |

Compile-time: the `t_stat` size + offset `_Static_assert`s (kernel + libt +
libthyla-rs `const _: () = assert!`) and the `T_WSTAT_* == P9_SETATTR_*` asserts.

**Boot regression:** the suite (637/637) + `boot OK` + the cross-reboot test all
pass under the enforced kernel — the `PRINCIPAL_SYSTEM` boot chain reads/traverses
devramfs (owner of every entry) and dev9p creates (`susan`, `/var/lib/corvus`)
still succeed (dev9p unenforced).

**Not covered at v1.0:** the dev9p enforcement path + the `perm_wstat_check` policy
in a *live* production path (both activate at A-3, when dev9p flips
`perm_enforced` true); the full `CAP_SET_IDENTITY`-spawned-child E2E (the kernel
test proves the same denial deterministically against real devramfs metadata).

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
- **A-2d (the kernel rwx-enforcement layer): LANDED (devramfs-live).** `kernel/
  perm.c` + the walk/open/create/wstat chokepoints (see Enforcement above). The
  privilege model voted 2026-05-30: `CAP_HOSTOWNER` is the unified fs-admin
  authority (DAC-override + chmod/chown/chgrp-any); owner|self-group chmod +
  chgrp-to-own-group; no-give-away chown. Enforcement is **live on devramfs**
  (system-owned); **dev9p deferred to A-3** via `Dev.perm_enforced = false` (uniform
  enforcement would brick the boot's post-pivot creates on host-baked uids).
  Closes A-2a audit F2 (`dev9p_stat_native` now respects the `Rgetattr` valid
  mask). First real exercise of I-22. 8 `perm.*` tests; suite 637/637; boot OK +
  cross-reboot PASS under the enforced kernel.

---

## Known caveats / footguns

- **t_stat is 80 bytes now.** Any new user of the SYS_FSTAT ABI must size its
  buffer at 80, not 72. The pouch `0010` patch's stack `struct t_stat` was grown
  in lockstep — the kernel writes 80 bytes and a 72-byte buffer would be a stack
  overflow. The `_Static_assert`s catch a stale mirror at build time.
- **chmod/chown policy is dormant in production at v1.0.** A-2d's
  `perm_wstat_check` exists + is unit-tested, but devramfs has no `wstat_native`
  (SYS_WSTAT on it returns -1 before the policy) and dev9p is deferred
  (`perm_enforced = false`), so no live path reaches the policy yet. It activates
  with dev9p at A-3. Against Stratum directly (which applies `Tsetattr`
  unconditionally) a chmod/chown still *succeeds* at v1.0 — the kernel does not
  yet gate it because dev9p enforcement is off.
- **dev9p rwx is unenforced at v1.0** (`Dev.perm_enforced = false`); its uid/gid is
  the connection identity until A-3 (per-user stratumd). A single boot connection
  presents one identity for the whole FS. The A-3 activation is one flag flip.
- **`dev9p_stat_native` respects the Rgetattr `valid` mask** (A-2a F2, closed in
  A-2d): mode/uid/gid are copied only if the server set their valid bits, else the
  pre-zeroed field stands (mode 0 / `PRINCIPAL_INVALID` / `GID_INVALID` —
  fail-closed for enforcement). Dormant for Stratum (always fills BASIC); sound for
  any server when dev9p enforcement activates at A-3.
  **A-2a audit F2 (P3), deferred to A-2d**: when enforcement lands, gate the map
  on `attr.valid & P9_STATS_{UID,GID}` and report `PRINCIPAL_NONE`/`GID_NONE`
  (or fail) when absent.
- **devramfs is system-owned + has no `.wstat_native`** — chmod/chown on a boot-FS
  file always returns -1 (read-only). This is correct, not a gap.
