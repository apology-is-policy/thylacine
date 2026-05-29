# 98 — Capability-scoped service storage + FS-delta (O_PATH)

**Status**: A-1.7 + FS-delta landed 2026-05-29 (commits `41e417d` scripture,
`aa11b17` corvus-side, `cd22572` FS-delta scripture, `bb59c6b` FS-delta impl,
`67f3ec6` joey-side, audit-close fixes following). Boot-verified against the real
disk-backed Stratum FS (corvus logs "storage capability OK (confined)"; 624/624).
**Audit R1 CLEAN** (0 P0 / 1 P1 / 1 P2 / 3 P3): F1 (the "withholding `TRANSFER`
blocks re-handing" claim was false — scripture-corrected to the monotonic bound),
F2 (confinement was cooperative — corvus now chroots FIRST), F5 (`T_OPATH` born
`R|W`); F3 (smoke proof), F4 (no T_OPATH unit test) documented as P3. The feared
shared-9P-session lifetime (corvus outlives joey) + fid/tag interleaving were
traced SOUND (the `p9_attached_ref` chain + the per-op `c->lock` round-trip hold).

Scripture: `NOVEL.md §3.10` (lead angle #10), `ARCHITECTURE.md §3.6` + invariant
**I-23**, `IDENTITY-DESIGN.md §9.4` (FS-delta), `docs/detour-status.md` (FS-delta
+ A-1.7).

---

## Purpose

A system service reaches its persistent storage through a **storage-root
capability it is handed** — a `KObj_Spoor` for its storage subtree, endowed at
spawn like fd 0/1/2 but for state — rather than naming a path in an ambient
namespace. Its filesystem authority is bounded by that capability (**I-23**):
there is no path it can name that escapes the granted subtree. This is POLA for
service state, enforced by mechanism (chroot + the `..`-reject + holding no other
Spoor), not policy. corvus is the first consumer (its identity DB + key wraps,
A-1b, live inside its handed capability).

**FS-delta** is the small FS primitive the model needs: `SYS_WALK_OPEN` with the
`T_OPATH` flag walks to a component but does **not** open it, returning a
*non-opened, walkable* handle. This is the Linux `O_PATH` / Plan 9 `walk`
equivalent — Thylacine had conflated walk + open (always `Tlopen`), which made it
impossible to create or walk a child under any returned handle (9P forbids
`Twalk` from an opened fid). FS-delta restores the lineage norm.

---

## Public API

### FS-delta (`T_OPATH`)

```
// kernel/include/thylacine/syscall.h, usr/lib/libt, usr/lib/libthyla-rs
#define SYS_WALK_OPEN_OPATH        0x80u   // T_OPATH (libt / libthyla-rs)
#define SYS_WALK_OPEN_OMODE_VALID  0x93u   // was 0x13; + T_OPATH

// SYS_WALK_OPEN(spoor_fd, name_va, name_len, omode) -> fd / -1
//   omode & T_OPATH: walk to `name` but do NOT open it. Returns a
//   NON-OPENED, walkable KObj_Spoor (rights R|W|TRANSFER, the same
//   envelope as a normal walk_open). Access bits (OREAD/...) are ignored.
```

A `T_OPATH` handle is the valid **base** for `SYS_WALK_CREATE` / `SYS_WALK_OPEN`
/ `SYS_RENAME` / `SYS_UNLINK` / `SYS_FSYNC` of *children*, and a valid
`SYS_CHROOT` target. It is **not** opened for byte I/O: `SYS_READ` / `SYS_WRITE`
/ `SYS_READDIR` on a `T_OPATH` fd fail at the 9P server (the fid is not opened) —
gracefully, with an error, never a crash.

### Capability-scoped storage (the convention)

The spawner (joey today; a service manager later) is the single policy point:

```c
// joey: build the storage dir + hand a reduced capability at spawn.
long dir = mkdir_or_open(T_WALK_OPEN_FROM_ROOT, "var", 3);  // O_PATH handle
dir = mkdir_or_open(dir, "lib", 3);                          // (chained)
dir = mkdir_or_open(dir, "corvus", 6);                       // the storage root
long cap = t_dup(dir, T_RIGHT_READ | T_RIGHT_WRITE);         // drop TRANSFER
unsigned int fds[1] = { (unsigned int)cap };
t_spawn_with_perms("corvus", 6, fds, 1, caps, perms);        // endow at fd 0
```

```rust
// corvus: confine to the handed capability.
unsafe { if t_chroot(0) == 0 { /* filesystem world IS the capability */ } }
```

`mkdir_or_open(parent_opath, name, len)` is the `mkdir -p` step over FS-delta:
`SYS_WALK_CREATE`(parent, name, DMDIR) [opened] → `SYS_CLOSE` →
`SYS_WALK_OPEN`(parent, name, `T_OPATH`) [non-opened, the next parent]. The first
parent is `FROM_ROOT` (non-opened). Idempotent across reboot: if the dir exists,
the create fails (EEXIST) and the `T_OPATH` walk_open still yields the handle.

---

## Implementation

### FS-delta — `kernel/syscall.c::sys_walk_open_handler`

The handler walks (`spoor_clone` + `dev->walk`), validates the walk (reuse-nc,
`nqid`), then — for a normal open — calls `nc->dev->open(nc, omode)`. FS-delta
wraps that call:

```c
if (!(omode_raw & SYS_WALK_OPEN_OPATH)) {
    if (!nc->dev->open(nc, (int)omode_raw)) { spoor_clunk(nc); return -1; }
}
// handle_alloc(p, KOBJ_SPOOR, RIGHT_READ|RIGHT_WRITE|RIGHT_TRANSFER, nc)
```

`dev9p_walk` (`kernel/dev9p.c`) already sets `nc`'s fid (a fresh walked fid),
qid (from the last walked qid, line ~225), and aux (`new_priv`, with
`fid_owned = true` and `attached_owner` inherited from the source — so the
walked Spoor holds a `p9_attached_ref`, keeping the session alive). Skipping
`dev->open` (which would `Tlopen` + set `COPEN` + reset offset + refresh qid)
therefore leaves a **complete, non-opened** Spoor: correct fid + qid + aux, no
`COPEN`. `dev9p` itself needs no change — the walk already produces the
non-opened fid; the handler just declines to open it.

**Why a non-opened fid is the valid base.** 9P2000.L: `Twalk` from an *opened*
fid is an error. `dev9p_walk` issues `p9_client_walk(src_priv->fid, new_fid, ...)`
— so the source must be a non-opened fid. A normal `walk_open` result has an
opened fid → `Twalk` from it is rejected by Stratum → child create/walk fails.
A `T_OPATH` result has a non-opened fid → `Twalk` succeeds.

### Capability handoff — `usr/joey/joey.c`

`do_corvus_bringup(long storage_dup_fd)` (extracted from `main` so it runs
**post-pivot**, on the persistent Stratum root) spawns corvus with `storage_dup_fd`
as its fd 0. The cap-prep block (end of `main`, after the pivot + fs probes):
builds `/var/lib/corvus` via `mkdir_or_open` chaining (each level a non-opened
O_PATH handle, born `R|W` per FS-delta's F5 envelope), `t_dup`s the leaf to `R|W`
(dropping `TRANSFER` as least-authority hardening — `handle_dup`'s subset check is
the I-6 enforcement point, so a delegate can never exceed `R|W`), and hands it as
fd 0. joey closes its own handles after the spawn (corvus holds its own ref via
the spawn-fd endow).

### Confinement — `usr/corvus/src/main.rs`

corvus `SYS_CHROOT`s to fd 0 (the non-opened capability) as the **first action**
in `rs_main` — before `heap_init` / the hardening sequence / `SYS_POST_SERVICE` —
so there is no ambient-FS window (corvus inherits joey's broad Stratum root via
`territory_clone` until this chroot displaces it; A-1.7 audit F2). The chroot is
**required**: a missing/invalid fd 0 is a fatal boot error (joey always hands the
capability), not a fallback. `territory_chroot` `spoor_ref`s the new root and
`spoor_clunk`s the displaced (shared Stratum-root) ref — so corvus's territory
root becomes the capability and it no longer references the broader tree.
`corvus_cap_smoke()` (run after the ready banner, post-chroot) then proves: create
+ write + fsync + read-verify a file at `FROM_ROOT` (now the capability — the
positive proof that `FROM_ROOT` IS the writable cap), and that `/thylacine-version`
(which exists *above* the capability at the Stratum root) is **unreachable**.
Idempotent across reboot (a surviving probe file is read-verified instead of
re-created).

---

## Invariant — I-23

> A service's filesystem authority is bounded by the storage capability it is
> handed: it reaches only that subtree, at only the handle's rights R, with no
> ambient FS authority beyond it; authority is **monotonic** (any delegate is `<=`
> R + same subtree).

Enforced by: the spawner endows a `handle_dup`-reduced (`R|W`, no `TRANSFER`)
non-opened capability; the service chroots to it as its first action (root becomes
the capability; `..` rejected by `sys_walk_open_handler`; post-chroot it holds no
Spoor outside the cap). A composition of I-2/I-4/I-6 (handle rights monotonic) +
I-1/I-3 (per-Proc Territory isolation).

**Two A-1.7 audit reconciliations (R1):**
- **`RIGHT_TRANSFER` does not block re-handing at v1.0** (F1). The earlier claim
  was false: `sys_bump_inherit_fds` (spawn-fd endow) and `handle_dup` gate on the
  handle *kind* + rights *subset*, not on `RIGHT_TRANSFER`. So a grantee holding
  only `R|W` can still delegate its capability to its own spawned children — which
  is **sound** (the delegate stays `<=` R and confined to the same subtree;
  corvus cannot manufacture rights it lacks). The security-bearing property is the
  *monotonic bound*, which holds. Withholding `TRANSFER` is least-authority
  hardening reserved for the Phase-5+ cross-Proc 9P-transfer surface.
- **Confinement is cooperative, not spawner-set** (F2). The service establishes
  confinement by chroot-ing to the capability as its first action; the broad root
  it inherits via `territory_clone` is displaced only at that chroot. corvus
  chroots first (window ≈ 0). A spawner-set-root variant (child born with root =
  the capability, no ambient window at all) is the v1.x mechanism-enforced form.

---

## Lineage

The fusion of Thylacine's two heritages, over 9P + Spoor:
- **Plan 9** — the per-process namespace (Territory); FS-delta restores Plan 9's
  walk-without-open (an unopened Chan is the walkable namespace base).
- **Linux** — `O_PATH` (a directory handle for `*at()` ops with no I/O); `T_OPATH`
  is the same idea.
- **Fuchsia / Genode** — a directory handle *is* the routed capability; the
  capability microkernels hand a session/handle rather than naming a path.

Thylacine does all three: a 9P-backed Spoor capability (any 9P server can back a
service's storage), endowed at spawn, mountable into (or chroot-able as) a Plan 9
Territory.

---

## Tests

- **A-1.7 joey E2E (behavioral, against real Stratum):** joey builds
  `/var/lib/corvus` via O_PATH `mkdir -p`, hands the `R|W` capability, corvus
  chroots + the confinement smoke logs
  `corvus: storage capability OK (confined; /thylacine-version unreachable)`.
  Runs every boot.
- **Suite:** 624/624 PASS (default), boot OK, joey status=0 — FS-delta added no
  regression.
- **Not testable in isolation:** the FS-delta wall is a *real-9P-server* behavior
  (Stratum enforcing "no `Twalk` from an opened fid"); the in-kernel `dev9p`
  loopback mock does not reproduce it, and `sys_walk_open_handler` is `static`
  (not unit-reachable). The joey E2E against real Stratum is the authoritative
  test.

---

## Known caveats / footguns

- **Shared 9P session.** corvus's capability Spoor (and post-chroot territory
  root) reference the *same* kernel `p9_client` session as joey (the storage dir
  was walked from joey's pivoted Stratum root). joey exits after the corvus E2E;
  corvus outlives it. The session survives because corvus's walked Spoor holds a
  `p9_attached_ref` (inherited in `dev9p_walk`'s `new_priv`). This is the
  load-bearing lifetime the audit prosecutes; it is the first multi-Proc
  shared-session case.
- **Non-dir O_PATH.** `T_OPATH` on a walk to a regular file yields a non-opened
  file handle; using it as a create/chroot base fails gracefully at Stratum (not
  a directory). No dir-only check is enforced at the syscall layer — harmless but
  worth knowing.
- **chroot-to-capability is the v1.0 confinement form** for a single-storage
  daemon. `SYS_MOUNT`-into-Territory at a named mount point (e.g. `/state`) is the
  alternative for a service that must keep a broader root — a v1.x ergonomic.
- **Nested-path creation requires O_PATH intermediates.** `mkdir -p` and any
  multi-component path build must thread non-opened (O_PATH) parents; a normal
  `walk_open`/`walk_create` result (opened) is not a valid base. `File::open`
  multi-component was latently broken by the same wall (it walks from opened
  intermediates) — fixing it to use `T_OPATH` for intermediates is owed
  (tracked; not in this chunk).
