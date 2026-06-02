# 104 - stalk: the per-Proc pathname resolver

> **Layer**: kernel namespace / path resolution. **Status**: stalk-1 landed
> (resolution within one Dev; multi-component absolute paths). stalk-2 (mount
> crossing) and stalk-3 (namespace-resident `/srv`) are pending. Binding design:
> `docs/STALK-DESIGN.md`. Invariant **I-28** (ARCHITECTURE.md section 28).

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
#define STALK_WALK  0   // resolve only; do NOT open (O_PATH / walkable base)
#define STALK_OPEN  1   // resolve + Dev.open(quarry, omode)
#define STALK_MAX_DEPTH 40

struct Spoor *stalk(struct Proc *p, struct Spoor *start,
                    const char *path, u64 pathlen, int amode, u32 omode);
```

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

## Data structures

No new persistent struct. The resolver holds an on-stack
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
  success + denial).
- `usr/joey/joey.c` -- the boot-path E2E: mkdir `stalk-e2e-dir`, create
  `stalk-e2e-dir/leaf`, then `t_open(FROM_ROOT, "stalk-e2e-dir/leaf", OREAD)` and
  read it back -- a 2-component absolute resolve on the real dev9p Stratum root.
  Prints `joey: stalk-1 multi-component SYS_OPEN E2E OK (stalk-e2e-dir/leaf)`.
  Idempotent (cleanup before + after).

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
  joey E2E. Resolution within a single Dev; absolute FS paths work. 698/698
  kernel tests; boot + login + the multi-component E2E green.
- **stalk-2 (pending)**: re-key the mount table to mount-point Spoor identity
  (`dc` + `qid.path`) + `cross_mounts` (Plan 9 `domount`) in the resolver +
  path-keyed `SYS_MOUNT`/`UNMOUNT`.
- **stalk-3 (pending)**: devsrv per-territory + namespace-resident `/srv` +
  retire `SYS_SRV_CONNECT` / `SYS_POST_SERVICE`.

## Known caveats / footguns

- **`..` is contained at `start`, not the dirfd's real parent.** For a relative
  resolve from a dirfd, `..` at the base is a no-op (it cannot ascend above the
  Spoor you were handed). This is over-restrictive vs POSIX `openat` (safe -- it
  cannot escape) and is the v1.0 containment choice; full cross-mount `..`
  fidelity (Plan 9 `Chan->mh` back-pointers) is a v1.x refinement.
- **No mount-crossing yet.** A path that would cross a mount point resolves
  entirely within the base's Dev (stalk-2 adds crossing). `/srv` is not yet a
  namespace-resident path (stalk-3).
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
