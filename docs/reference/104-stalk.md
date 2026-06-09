# 104 - stalk: the per-Proc pathname resolver

> **Layer**: kernel namespace / path resolution. **Status**: stalk-1 + stalk-2 +
> stalk-3 landed (multi-component absolute paths + Plan 9 `domount` mount crossing
> keyed by the full `(dc, devno, qid.path)` Spoor identity + namespace-resident
> `/srv`). #957 extended crossing to the single-hop `SYS_WALK_OPEN` (see "Single-hop
> walks cross too" below) so the libthyla-rs `fs::` mutation path crosses into
> per-user `/home/<user>` mounts. Binding design: `docs/STALK-DESIGN.md`. Invariant
> **I-28** (ARCHITECTURE.md section 28).

## Purpose

`stalk` is Thylacine's multi-component pathname resolver -- the Plan 9 `namec`
(*name-to-channel*), renamed for the bestiary (the predator **stalks** its quarry
along a path through the per-Proc namespace to the target **Spoor**). Before
stalk, `SYS_WALK_OPEN` resolved a *single* path component and never consulted the
mount table; absolute paths (`/sbin/login`, `/home/<user>`, `/var/lib/corvus`)
could not be resolved in one syscall. `stalk` walks a full `/`-separated path
from a base Spoor, applying a per-component permission X-search and `.`/`..`
(contained at the base), then optionally opens the target.

It is the foundation under A-5b's isolation requirement: namespace-resident
`/srv` (stalk-3) is its first consumer, but stalk also unlocks every absolute
path in the OS.

## Public API

### Kernel: `stalk()` (`kernel/include/thylacine/stalk.h`)

```c
#define STALK_WALK  0   // resolve only; do NOT open (O_PATH / walkable base).
                        //   The quarry IS crossed (open a mount point -> the
                        //   mounted root).
#define STALK_OPEN  1   // resolve + Dev.open(quarry, omode). Quarry crossed.
#define STALK_MOUNT 2   // resolve to the mount point's OWN identity (final
                        //   component NOT crossed) + no open. SYS_MOUNT/UNMOUNT
                        //   use it so MREPL re-keys the same underlying point.
#define STALK_MAX_DEPTH 40

struct Spoor *stalk(struct Proc *p, struct Spoor *start,
                    const char *path, u64 pathlen, int amode, u32 omode);
```

stalk-2 added `STALK_MOUNT` and the mount-crossing behavior (below). The `amode`
is validated at entry (`amode != STALK_WALK && != STALK_OPEN && != STALK_MOUNT
-> NULL`); a sub-chunk adding a new amode MUST extend this guard AND give it a
final-hop dispatch arm (stalk-1 audit F1).

- `p` -- the calling Proc (for the per-component `perm_check`).
- `start` -- the base Spoor. **BORROWED**: the caller owns it (a handle's Spoor
  or the Territory `root_spoor`); stalk never refs or clunks it.
- `path` -- `pathlen` bytes, NUL-free (the caller has copied it from user space
  and rejected embedded NUL). `/`-separated; leading `/` and `//` collapse.
- Returns the resolved Spoor (the **quarry**; `ref == 1`, opened iff
  `STALK_OPEN`) or `NULL` on any failure. The caller installs the handle and
  derives its rights.

### Syscall: `SYS_OPEN = 65` (`kernel/include/thylacine/syscall.h`)

```
x0 = start_fd : a KOBJ_SPOOR handle (RIGHT_READ) OR SYS_WALK_OPEN_FROM_ROOT
                ((u64)-1) to resolve from the Territory root_spoor.
x1 = path_va  : user-VA of the path bytes (NUL-free; '/'-separated).
x2 = path_len : 1 .. SYS_OPEN_PATH_MAX (1024).
x3 = omode    : OREAD/OWRITE/ORDWR/OEXEC (+ OTRUNC); SYS_WALK_OPEN_OPATH (0x80)
                selects a walk-only (unopened) handle.
-> opened (or O_PATH walkable) KOBJ_SPOOR fd (>= 0) or -1.
```

`sys_open_handler` (`kernel/syscall.c`) validates the args (mirroring
`sys_walk_open_handler`), resolves `start`, copies the path into a kernel scratch
(`SYS_OPEN_PATH_MAX + 1` bytes, rejecting embedded NUL), calls `stalk()`, and on
success derives the handle rights (O_PATH -> `R|W` no TRANSFER; else
`rights_for_omode(omode) | RIGHT_TRANSFER`, the A-3b derivation) and
`handle_alloc`s a `KOBJ_SPOOR`.

`SYS_OPEN` **supersedes** `SYS_WALK_OPEN` going forward; `SYS_WALK_OPEN` remains
unchanged as the single-component fast path until its callers migrate (a
deferred cleanup, not pinned to a sub-chunk).

### Userspace wrappers

- libt C: `t_open(long start_fd, const char *path, size_t path_len,
  unsigned long omode)` (`usr/lib/libt/include/thyla/syscall.h`).
- libthyla-rs: `t_open(start_fd: i64, path: *const u8, path_len: usize,
  omode: u32) -> i64` (`usr/lib/libthyla-rs/src/lib.rs`).

## Implementation (`kernel/stalk.c`)

`stalk` generalizes the audited single-hop `sys_walk_open_handler` lifetime
(`spoor_clone` -> `Dev.walk` -> `spoor_clunk`) to N hops via a **trail** of owned
clones.

### The resolution loop

For each `/`-delimited component (empty components from leading `/` and `//`
collapse):

1. `"."` -> no-op (stay).
2. `".."` -> pop the trail (`spoor_clunk(trail[--depth])`); at the bottom
   (`depth == 0`) it is a no-op, so resolution can **never escape above
   `start`** -- the chroot/pivot boundary (I-28).
3. a real component:
   - reject `clen > SYS_WALK_OPEN_NAME_MAX` (the Dev.walk vtable takes a
     NUL-terminated name).
   - **per-component X-search**: `parent = depth ? trail[depth-1] : start`; on a
     `perm_enforced` Dev, `spoor_stat_native(parent)` + `perm_check(p, &st,
     PERM_X)` (fail-closed).
   - reject `depth >= STALK_MAX_DEPTH` (trail full) **before** the push, so the
     `trail[40]` out-of-bounds write cannot occur.
   - `nc = spoor_clone(parent)`; copy the component into a NUL-terminated
     `namebuf`; `w = parent->dev->walk(parent, nc, {namebuf}, 1)`.
   - on the failure shapes, clean `nc` (see Lifetime) and unwind; on success
     push `nc` (now owning its own fid for dev9p) onto the trail.

After the loop the **quarry** is `trail[--depth]` (popped, so the unwind below
does not clunk it). If zero real components survived (`"/"`, `"."`, or a `".."`
run netted back to the base), the quarry is the base itself, minted via a
**clone-walk** (`Dev.walk(start, q, NULL, 0)`) so it is an independently openable
Spoor with its own fid (Twalk-from-an-opened-fid is forbidden by 9P, so the base
must be re-walked to be opened directly).

`STALK_OPEN` then runs the final-hop R/W `perm_check` (`perm_want_for_omode`) and
`Dev.open(quarry, omode)`; `STALK_WALK` (O_PATH) returns the walkable quarry
unopened (exempt from R/W, matching the single-hop walk-open's O_PATH carve-out).
Finally `stalk_unwind` clunks every trail ancestor, and the quarry is returned.

`Dev.open` returns EITHER the same Spoor opened in place (dev9p / devramfs: a
read/write cursor over the walked node, ref unchanged) OR a DIFFERENT owned Spoor
that REPLACES the quarry (stalk-3b-β: devsrv open=connect consumes the resolved
`/srv/<name>` service node and returns the connection endpoint -- a dev9p root
Spoor for a 9p-mode service, a byte-conn Spoor for a byte-mode one;
STALK-DESIGN.md §5.2). The resolver adopts the returned Spoor: if it differs from
the quarry, the spent quarry is `spoor_clunk`'d (open did not consume its ref) and
the replacement becomes the returned quarry. `stalk.open_replace` (a fixture Dev
whose open returns a marked clone) covers the replacement branch + its leak
balance.

### Lifetime discipline (the audit-critical part)

A `spoor_clone(parent)` copies the parent's `aux` **shallowly** -- for dev9p that
is a SHARED fid pointer -- until a successful `Dev.walk` REPLACES `nc->aux` with
`nc`'s own fid. Therefore:

| Event | Cleanup | Why |
|---|---|---|
| `Dev.walk` returns NULL | `nc->aux = NULL; spoor_unref(nc)` | nc still shares the parent's fid -- DETACH + unref, never clunk (clunk would clunk the parent's fid). |
| `w->spoor != nc` (reuse-nc contract violated) | `walkqid_free(w); nc->aux = NULL; spoor_unref(nc)` | same shared-aux state. |
| `w->nqid != 1` (a devramfs/fixture miss; dev9p returns NULL) | `walkqid_free(w); spoor_clunk(nc)` | nc was reused with a non-heap (NULL) aux -- clunk-safe; matches `sys_walk_open_handler`. |
| success | push on the trail | nc owns its fid; clunk-safe at unwind. |

`start` is borrowed and is **never** clunked or unref'd. The quarry is popped off
the trail before the unwind, so it is never double-clunked. On any failure path,
`fail:` clunks the quarry (if set) and unwinds every remaining trail ancestor
exactly once. The leak/UAF balance is verified by `stalk.lifetime_no_leak` (the
`spoor_total_allocated - spoor_total_freed` live count returns to baseline across
both a successful resolve+clunk and a denied resolve).

### No component batching at v1.0

stalk walks **one** component per `Dev.walk` with a kernel X-check at each hop,
even though `dev9p_walk` can batch up to `P9_MAX_WALK = 16` components into one
`Twalk`. Batching would skip the kernel's intermediate X-checks (delegating them
to the server); correctness-first, batching is a documented v1.x perf
optimization. Cost: a deep dev9p path is N x [Tgetattr + Twalk].

### Mount crossing (stalk-2, Plan 9 `domount`)

The mount table (`Territory.mounts[]`, `kernel/territory.c`) is keyed by the
mount point's full Plan 9 identity `(dc, devno, qid.path)` -- the
`(type, dev, qid)` triple. `stalk_cross_mounts(p, probe, &out)` (public since #957;
was the static `cross_mounts`) tests `probe`'s identity
against the table (`mount_lookup`); on a match it mints an INDEPENDENT clone of
the mounted source via `clone_walk_zero` (a zero-element `Dev.walk`, which for
dev9p allocates a fresh fid so the crossed Spoor does not share the table's
source fid) and loops to follow a mount-over-a-mount chain to the leaf (bounded
by `PGRP_MAX_MOUNTS`; cycle-free by I-3). `probe` is never consumed -- the caller
decides whether to clunk it.

**Why the `devno` axis.** `qid.path` is unique only WITHIN a `(dc, devno)`
instance. Every dev9p attach session shares `dc = '9'` and every attach root has
`qid.path == 0`, so `(dc, qid.path)` alone cannot distinguish two concurrent 9P
sessions' mount points -- exactly the A-5b case (corvus + a per-user stratum-fs
mounted in one Territory). stalk-2 added `u32 Spoor.devno` (Plan 9 `Chan.dev`),
minted per attach session by `spoor_next_devno()` (`kernel/spoor.c`); dev9p stamps
the attach root in `dev9p_attach_client`, and walked/cloned descendants inherit
it via `spoor_clone`. Static single-instance Devs (devramfs, the test fixture)
leave it 0. `test_territory_mount.c::devno_disambiguates` proves two mount points
with the same `(dc, qid.path)` and distinct `devno` are distinct entries.

**Crossing is "on descent" (Plan 9 namec).** A trail Spoor is crossed the moment
it is used as a directory to walk THROUGH -- replaced in place by the mounted
root, which is then X-checked (the MOUNTED root's perms govern, not the shadowed
mount point's). The quarry is crossed at the end (so `open("/mnt")` yields the
mounted root), EXCEPT under `STALK_MOUNT`, which returns the mount point's own
identity. The base Spoor (`start`) is crossed before the loop -- if it crosses,
the owned crossed clone becomes `trail[0]` (since `start` is borrowed and cannot
be crossed in place). When no mounts exist, `cross_mounts` is a table-lookup
no-op, so stalk-1 behavior is preserved exactly.

**Mount points must exist** (Plan 9 M1, design D4). devramfs gained synthetic
`/srv` + `/proc` directories (empty, world-r/x, SYSTEM-owned; qid range above any
file index) so the boot root has walkable mount points; the disk FS provides its
own (host-baked).

### Single-hop walks cross too (#957)

`stalk()` is not the only path resolver: the single-hop `SYS_WALK_OPEN`
(`sys_walk_open_handler`, "walk ONE component from a parent fd") and its
create/rename/unlink siblings are the lower-level primitive that libthyla-rs
`fs::` navigates with (`file::with_parent_dir` walks the parent chain
component-by-component via `t_walk_open`; `File::open` / `create_dir` / `rename`
build paths this way). Plan 9 has no non-crossing walk -- ALL walking crosses
mounts -- so `sys_walk_open_handler` calls `stalk_cross_mounts` at BOTH the
**source** (before the X-search + walk: walk INTO the mounted root if the parent
fd is a mount point -- mirrors stalk's base cross) and the **result** (after the
walk, before open: a walked mount point yields the mounted root -- mirrors stalk's
quarry cross). The crossed clone is OWNED (its own fid); the handler clunks the
shadowed Spoor and adopts it, so the X-search / `perm_check` / `Dev.open` / the
installed handle's rights all run on the MOUNTED root.

Before #957 the single-hop walk did NOT cross, so a logged-in user's
`mkdir`/`touch`/`cp` into their own `/home/<user>` (a per-user dev9p mount over a
SYSTEM-owned placeholder dir) resolved the shadowed placeholder and was denied by
A-3 rwx (the user is `other` on the placeholder's 0755). The source cross is a
no-op for every current caller (no API yields a mount-point fd once walks cross,
and the Territory root is never a mount-table entry) -- it is present for exact
one-component-stalk symmetry + correctness if a mount-point fd ever exists.

### Per-Proc cwd (LS-4)

`stalk()` only ever resolves an absolute-from-root path; the per-Proc current
directory ("dot", the Plan 9 concept) lives a layer ABOVE it as a **name-based**
cleaned path string on the Territory (`Territory.dot_path`; `NULL` == `"/"`,
heap-allocated lazily, freed at `territory_unref`). A relative path is joined to
`dot_path` and lexically cleaned into an absolute path BEFORE `stalk` runs --
exactly POSIX `openat(AT_FDCWD, ...)`. So **I-28 containment is unchanged and
gains no new mechanism**: `stalk` is still handed an absolute-from-root path and
re-clamps `..` at `root_spoor`, so even a hostile un-cleaned join cannot escape.

- **`territory.c::cwd_lexical_resolve(dot, input, inlen, out, outcap)`** -- the
  pure (no-lock, no-alloc) join + clean: an absolute `input` ignores `dot`; a
  relative one is seeded with `dot`'s components; `.` is dropped, `//` collapsed,
  `..` pops the last component (clamped at `/`); an empty result -> `"/"`.
  Returns the output length or -1 if it would not fit `outcap`. Unit-tested in
  isolation (`territory.cwd_lexical`).
- **`territory_resolve_cwd(p, ...)`** -- the locked wrapper: reads `dot_path`
  under the per-Territory leaf `dot_lock` (the lexical resolve is bounded CPU, so
  holding the lock across it is safe) and calls `cwd_lexical_resolve`. The
  SYS_CHDIR + SYS_OPEN-relative-join entry point.
- **`territory_getdot` / `territory_setdot`** -- read / replace `dot_path` under
  `dot_lock`. `setdot` kmalloc's the new copy BEFORE taking the lock, swaps, then
  frees the old OUTSIDE the lock (readers copy under the lock and never retain
  the pointer -> no UAF). `"/"` is stored as the `NULL` sentinel.

`SYS_CHDIR = 69`(path, len) resolves the cleaned target from `root_spoor`,
requires it to be a directory (`QTDIR`) the caller can SEARCH (the open-path
`perm_check(PERM_X)`, gated on `Dev.perm_enforced`), then swaps `dot_path`.
`SYS_GETCWD = 70`(buf, len) copies `dot_path` out NUL-terminated (`-1` if the
path + NUL does not fit). `sys_open_handler` applies the join ONLY for the
FROM_ROOT sentinel + a relative path; an absolute path or an explicit dirfd is
unchanged.

`dot_path` lives on the per-Proc `Territory`, so a Proc's threads SHARE it (POSIX
per-process cwd, serialized by `dot_lock`) and a child INHERITS an independent
snapshot via `territory_clone`. `dot_lock` is the first per-Territory lock;
extending it to cover `root_spoor` would close the dormant pivot-vs-walk race
(#848).

**v1.0 is name-based** (a string). A handle-based dot Spoor -- the rename-robust
Plan 9/Linux form -- is the v1.x upgrade, landing WITH symlinks (which force it:
a live-dot start would strand `cd ..` unless `..` becomes a device parent-walk, a
new mechanism on the I-28 surface, and the symlink/`..` interaction is the only
correctness argument that justifies that cost). See LIFE-SUPPORT.md LS-4 +
STALK-DESIGN.md 4.3.

## Data structures

The mount-table entry (`struct PgrpMount`, `kernel/include/thylacine/territory.h`)
grew from 16 to 32 bytes when re-keyed: `{ Spoor *source; u64 mp_qid_path; int
mp_dc; u32 mp_devno; u32 flags; u32 _pad; }`. The size-pinned `Territory`
static_asserts re-bumped accordingly (entry 16->32, total `32 + 8*BINDS +
32*MOUNTS`). `struct Spoor` gained `u32 devno` after `dc`.

No new persistent resolver struct. The resolver holds an on-stack
`struct Spoor *trail[STALK_MAX_DEPTH]` (40 pointers) plus a
`char namebuf[SYS_WALK_OPEN_NAME_MAX + 1]` per component. `sys_open_handler`
holds a `char path_scratch[SYS_OPEN_PATH_MAX + 1]` (1025 bytes) -- comfortably
within the 16 KiB kernel stack.

## Naming rationale

- **stalk** -- the resolver (Plan 9 `namec`; the apex-predator verb whose quarry
  is reached along a path).
- **trail** -- the in-call stack of resolved Spoors stalk follows and that `..`
  pops back along (a spoor *is* a trail; the predator follows it).
- **quarry** -- the target Spoor stalk returns.

The mount-crossing step (`cross_mounts` / `domount`, stalk-2) and the path
tokenizer keep their plain descriptive names -- no outback word cleared the
clarity bar there, and the discipline is not to force it.

## Tests

- `kernel/test/test_stalk.c` -- 12 unit tests against an in-file fixture Dev
  (`stalkfix`, a nested qid-based tree, since devramfs is flat): `resolve_multi`,
  `resolve_deep`, `leading_and_double_slash`, `dot_noop`, `dotdot_pop`,
  `dotdot_containment` (cannot escape the base), `xsearch_deny` (a 0644 dir with
  no x denies traversal even for its SYSTEM owner), `missing_component`,
  `opath_no_open` (STALK_WALK leaves COPEN clear), `open_root` (the 0-component
  clone-walk path), `depth_cap` (a self-referential `loop` node overflows the
  trail cap -> clean NULL), `lifetime_no_leak` (Spoor count balance across
  success + denial). **stalk-2 cross-mount** (6 more): `cross_mount` (graft
  subtree onto a dir, resolve THROUGH it), `cross_mount_final_quarry` (open a
  mount point -> the mounted root), `cross_mount_xsearch_deny` (the MOUNTED
  root's no-x perms deny traversal), `mount_amode_no_cross` (STALK_MOUNT returns
  the mount point's own identity + MREPL re-keys the same point), `cross_mount_chain`
  (mount-over-a-mount follows to the leaf), `cross_mount_no_leak` (the
  `clone_walk_zero` transient is clunked, not leaked).
- `kernel/test/test_territory_mount.c::devno_disambiguates` -- two mount points
  with the same `(dc, qid.path)` but distinct `devno` are distinct entries +
  `mount_lookup` resolves each to its own source (the dev9p two-session fix).
- `usr/joey/joey.c` -- two boot-path E2Es on the real dev9p Stratum root.
  stalk-1: mkdir `stalk-e2e-dir`, create `stalk-e2e-dir/leaf`, `t_open(FROM_ROOT,
  "stalk-e2e-dir/leaf", OREAD)`, read back. stalk-2: graft `stalk-x-src` onto the
  sibling `stalk-x-mnt` and `t_open(FROM_ROOT, "stalk-x-mnt/xleaf", OREAD)` --
  resolves THROUGH the mount (a real dev9p `domount` cross). Both idempotent
  (cleanup before + after); print `... E2E OK`.
- The `/attach-probe` + `/stub-driver` userspace probes (kernel-test harnesses)
  exercise the path-keyed `SYS_MOUNT`/`SYS_UNMOUNT` cycle end-to-end with a real
  userspace 9P attach (mount the attached root onto devramfs `/srv`, unmount).

## Error paths

`stalk` returns `NULL` (mapped to `-1` by `sys_open_handler`) on: a missing
component (`Dev.walk` miss), a per-component X-search denial, the final R/W
denial, `Dev.open` failure, a component longer than `SYS_WALK_OPEN_NAME_MAX`,
trail-depth overflow, or `spoor_clone` / `walkqid` OOM. `sys_open_handler`
returns `-1` on `path_len == 0` or `> SYS_OPEN_PATH_MAX`, an invalid user buffer,
an unknown omode bit, a missing `root_spoor` (FROM_ROOT with no chroot), a
missing/RIGHT_READ-failing `start_fd` handle, an embedded NUL in the path, or a
full handle table (the quarry is clunked).

## Performance characteristics

One `Dev.walk` per path component; for dev9p each hop is a `Tgetattr` (the
X-search) + a `Twalk` round-trip (no batching at v1.0). devramfs / the fixture
resolve locally (no RPC). The trail + scratch are stack-allocated (no heap
allocation in the resolver beyond the Spoor clones, which are SLUB).

## Status

- **stalk-1 (landed)**: the resolver core + `SYS_OPEN` + the wrappers + the
  joey E2E. Resolution within a single Dev; absolute FS paths work.
- **stalk-2 (landed)**: the mount table re-keyed to the full `(dc, devno,
  qid.path)` mount-point Spoor identity + `Spoor.devno` (Plan 9 `Chan.dev`) +
  `cross_mounts` (Plan 9 `domount`, cross-on-descent) + `STALK_MOUNT` +
  path-keyed `SYS_MOUNT`/`SYS_UNMOUNT` + devramfs synthetic `/srv`+`/proc` mount
  points + the migrated `/attach-probe`/`/stub-driver`/`alloc-smoke` callers.
  705/705 kernel tests (default + UBSan + smp8); the 5 `territory_buggy*` TLA
  invariant-detection gates green; boot + login + both joey E2Es green.
- **stalk-3 (pending)**: devsrv per-territory + namespace-resident `/srv` +
  retire `SYS_SRV_CONNECT` / `SYS_POST_SERVICE`.
- **LS-4a (landed)**: the per-Proc cwd substrate -- `Territory.dot_path` +
  `dot_lock`, `cwd_lexical_resolve` / `territory_resolve_cwd` / `getdot` /
  `setdot`, `SYS_CHDIR = 69` / `SYS_GETCWD = 70`, and the `sys_open_handler`
  relative->cwd join. Name-based (a cleaned path string); I-28 preserved (no new
  mechanism). Kernel tests `territory.cwd_lexical` + `territory.cwd_dot`. The
  userspace wiring (libthyla-rs `chdir`/`getcwd` + the shell `cd` + the LS-CI
  relative-`cat` E2E) is LS-4b.

## Known caveats / footguns

- **`..` is contained at `start`, not the dirfd's real parent.** For a relative
  resolve from a dirfd, `..` at the base is a no-op (it cannot ascend above the
  Spoor you were handed). This is over-restrictive vs POSIX `openat` (safe -- it
  cannot escape) and is the v1.0 containment choice; full cross-mount `..`
  fidelity (Plan 9 `Chan->mh` back-pointers) is a v1.x refinement.
- **The per-Proc cwd is name-based + combined-length-bounded (LS-4; audit F1).**
  `cwd_lexical_resolve` joins `dot_path` (<= `SYS_OPEN_PATH_MAX`) with the
  relative input into one buffer; a deep cwd + a long relative path whose
  *joined* length exceeds `SYS_OPEN_PATH_MAX` is rejected (`-1`) even though it
  would resolve from root. This is the same combined-length bound the
  single-component surfaces carry; there is no overflow (every write is
  capacity-guarded). Separately, renaming an ancestor of a live cwd makes
  `dot_path` stale (the Proc re-`cd`s) -- the name-based v1.0 limitation; the
  handle-based dot (v1.x, lands with symlinks) removes it.
- **Mount crossing is in-call only; `..` does not un-cross.** stalk-2 crosses
  mounts forward (Plan 9 `domount`), and `..` pops the in-call trail (contained
  at the base). Full Plan-9 cross-mount `..` fidelity (a `..` at a mounted root
  returning to the mount point's parent-in-the-underlying-fs across separate
  `stalk` calls, via persisted `Chan->mh` back-pointers) is a v1.x refinement;
  v1.0's in-call trail containment is the audited invariant. `/srv` is not yet a
  namespace-resident path (stalk-3 mounts devsrv there).
- **No per-Territory mount-table lock.** Like the rest of the per-Proc Territory
  (binds/mounts have never been locked), `cross_mounts`/`mount_lookup` read
  `mounts[]` without a lock. A peer thread mutating the table concurrently
  (multi-thread Proc) is the same inherited lock-free-Territory class as the
  `handle_get` TOCTOU; SMP Territory locking is a Phase-5+ item.
- **`SYS_WALK_OPEN` still exists.** Single-component callers (joey bringup
  probes, the pouch openat seam) are unchanged; migrating them to `SYS_OPEN` and
  retiring `SYS_WALK_OPEN` is a deferred cleanup.
- **The X-search is open-time only.** As with `sys_walk_open_handler`, perms are
  snapshotted at resolve time; `SYS_READ`/`SYS_WRITE` re-check only the handle
  RIGHT (the A-3 open-time-snapshot model).
- **The borrowed-`start` TOCTOU is amplified to N hops (stalk-1 audit F3).** The
  pre-existing surface-wide lockless `handle_get` TOCTOU (it returns a raw
  `Spoor *` without a ref) is unchanged, but `SYS_OPEN` from a `start_fd` handle
  holds the borrowed `start` across up to `STALK_MAX_DEPTH` *blocking* dev9p
  walks, so a concurrent same-Proc `t_close(start_fd)` race is N-hop-wide rather
  than single-hop. `SPOOR_MAGIC` yields a clean extinction (not silent
  corruption). The fix belongs to the planned handle-lifetime hardening pass, not
  a stalk-local band-aid -- a local `spoor_ref` after an already-racy lookup
  could itself ref a freed Spoor.
