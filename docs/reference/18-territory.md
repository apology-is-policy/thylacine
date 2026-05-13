# 18 — Territory primitives (P2-E + P5-attach-mount)

The Plan 9 territory — a process's view of the resource tree, composed via `bind` / `unbind` / `mount` / `unmount`. Per `ARCHITECTURE.md §9.1` + `§9.6`. v1.0 lands the kernel-internal API; the syscall surface is deferred until fd-syscall infrastructure exists (P5-fd-syscalls / P6).

The Territory carries two parallel tables:

- **`binds[]`** — Plan 9 path-to-path bindings. Walking `dst` yields `src`. Cycle-checked.
- **`mounts[]`** — filesystem-as-Spoor grafts. Walking `target_path` dispatches through the Spoor's Dev vtable. Each entry holds one refcount on the source Spoor.

---

## Purpose

A `Territory` (process group, Plan 9 idiom) holds one process's namespace. Each Proc has its own `Territory` at v1.0; RFNAMEG-shared territories are Phase 5+ syscall surface.

Key invariants (proven in `specs/territory.tla`):

- **Cycle-freedom (I-3)**: the bind graph is acyclic. Adding a bind that would close a cycle is rejected.
- **MountRefcountConsistency (§9.6.6)**: for every Spoor `s`, the kernel's refcount equals the cardinality of mount entries referencing `s` across all Territories. Maintained by `mount` (bump) / `unmount` (drop) / `territory_clone` (bump per cloned entry) / `territory_unref` (drop per remaining entry at final release).
- **Isolation (I-1)**: structural — `bindings[p]` / `mounts[p]` for different Territories are independent.

---

## Public API — `<thylacine/territory.h>`

```c
#define PGRP_MAGIC      0x50475250C0DEFADEULL
#define PGRP_MAX_BINDS  8                       // v1.0 cap; Phase 5+ → growable RB tree
#define PGRP_MAX_MOUNTS 8

typedef u32 path_id_t;                          // abstract; future Spoor-walk → struct Spoor *

struct PgrpBind {
    path_id_t src;                              // bound content
    path_id_t dst;                              // mount point; walking dst yields src
};

struct PgrpMount {
    struct Spoor   *source;                     // 8-byte aligned first for compact layout
    path_id_t       target;
    u32             flags;                      // MREPL | MBEFORE | MAFTER | MCREATE
};
_Static_assert(sizeof(struct PgrpMount) == 16, ...);

#define MREPL    0x0001
#define MBEFORE  0x0002
#define MAFTER   0x0004
#define MCREATE  0x0008

struct Territory {
    u64                 magic;                  // PGRP_MAGIC
    int                 ref;                    // refcount; rfork(RFNAMEG) shares (Phase 5+)
    int                 nbinds;
    int                 nmounts;
    u32                 _pad;
    struct PgrpBind     binds[PGRP_MAX_BINDS];
    struct PgrpMount    mounts[PGRP_MAX_MOUNTS];
};
_Static_assert(sizeof(struct Territory)
               == 24 + 8 * PGRP_MAX_BINDS + 16 * PGRP_MAX_MOUNTS, ...);

void               territory_init(void);
struct Territory  *kpgrp(void);
struct Territory  *territory_alloc(void);
struct Territory  *territory_clone(struct Territory *parent);
void               territory_ref(struct Territory *p);
void               territory_unref(struct Territory *p);

int                bind(struct Territory *p,
                        path_id_t src, path_id_t dst);
int                unbind(struct Territory *p,
                          path_id_t src, path_id_t dst);

int                mount(struct Territory *p, struct Spoor *source,
                         path_id_t target, u32 flags);
int                unmount(struct Territory *p, path_id_t target_path);

int                territory_nbinds(struct Territory *p);
int                territory_nmounts(struct Territory *p);
u64                territory_total_created(void);
u64                territory_total_destroyed(void);
```

### `bind(p, src, dst)` — return semantics

| Return | Meaning |
|---|---|
| `0`  | success; edge `dst -> src` added |
| `-1` | cycle would be created (existing edges would form `src -> ... -> dst`, then `dst -> src` closes loop) |
| `-2` | edge already exists (idempotent re-bind) |
| `-3` | binds[] full (PGRP_MAX_BINDS reached) |
| `-4` | self-bind (`src == dst`); treated as a degenerate length-1 cycle |

### `unbind(p, src, dst)` — return semantics

| Return | Meaning |
|---|---|
| `0`  | success; edge removed |
| `-1` | edge not present |

(Renamed from `unmount` at P5-attach-mount — the verb `unmount` is now reserved for the mount-table primitive.)

### `mount(p, source, target, flags)` — return semantics

| Return | Meaning |
|---|---|
| `0`  | success (entry added or idempotent no-op) |
| `-1` | source is NULL |
| `-2` | mounts[] full (PGRP_MAX_MOUNTS reached) |

- **Idempotency**: re-mounting the same `(target, source)` pair is a no-op success; no second `spoor_ref`.
- **MREPL**: if `flags & MREPL` and an entry at `target` exists with a different `source`, the existing entry is replaced (old source's ref dropped; new source's ref taken); `nmounts` unchanged.
- **MBEFORE / MAFTER / MCREATE**: recorded in the entry's `flags` field; at v1.0 treated as "append a new entry" for union mounts. Union walk semantics land at Phase 5+ when the walk algorithm grows union support.

### `unmount(p, target_path)` — return semantics

| Return | Meaning |
|---|---|
| `0`  | success; one entry removed; source's refcount dropped |
| `-1` | no entry at `target_path` |

Removes the FIRST entry at `target_path`. For union mounts with multiple entries, call repeatedly.

---

## Implementation

`kernel/territory.c` (~290 LOC).

### Territory lifecycle

- `territory_init`: SLUB cache + `kpgrp` (kproc's empty Territory; ref=1). Called from `boot_main` BEFORE `proc_init`.
- `territory_alloc`: SLUB-allocate via `kmem_cache_alloc(KP_ZERO)`. Sets magic + ref=1 + nbinds=nmounts=0. Returns NULL on OOM.
- `territory_clone`: allocate fresh + deep-copy `parent->binds[]` and `parent->mounts[]`. For each cloned mount entry, **`spoor_ref(source)`** — each cloned entry contributes one new reference. Models the spec's ForkClone refcount update.
- `territory_ref` / `territory_unref`: refcount.
- `territory_unref` final release: BEFORE `kmem_cache_free`, iterate `mounts[]` and **`spoor_unref(source)`** on each entry. The order in the table is not load-bearing at v1.0; the loop walks in reverse (cosmetic). After the loop, sets `p->nmounts = 0` defensively; SLUB's freelist write then clobbers magic.

### Bootstrap order discipline

`territory_init` runs BEFORE `spoor_init` (which is inside `dev_init`). Safe because `territory_init` only creates EMPTY Territories (nmounts = 0). `territory_unref`'s final-release path only calls `spoor_unref` when `nmounts > 0`, which requires `mount` to have been called, which requires a Spoor, which requires `spoor_init`. The dependency is satisfied automatically by call ordering.

### Cycle detection (`would_create_cycle`)

Unchanged from P2-Eb. Fixed-point reachability over `binds[]`; O(N²) worst case at PGRP_MAX_BINDS = 8 → 64 inner iterations.

### Mount table operations

```c
int mount(struct Territory *p, struct Spoor *source,
          path_id_t target, u32 flags) {
    // Validate, idempotency check, MREPL replace, table-full check,
    // spoor_ref(source), append entry.
}
int unmount(struct Territory *p, path_id_t target_path) {
    // Find first entry at target_path, swap-with-last to remove,
    // spoor_unref the removed entry's source, return.
}
```

### Integration with rfork

`kernel/proc.c::rfork` calls `territory_clone(parent->territory)` which now ALSO deep-copies the mount table and bumps each source's refcount. No change at the rfork call site; the discipline is inside `territory_clone`.

`kernel/proc.c::proc_free` calls `territory_unref(p->territory)`. The unref's final-release path drops each mount entry's per-entry refcount BEFORE freeing the Territory.

---

## Spec cross-reference

`specs/territory.tla` at P5-attach-mount:

- **Actions**: `Init`, `Bind`, `BuggyBind`, `Unbind`, `Mount`, `BuggyMountNoRefbump`, `Unmount`, `BuggyUnmountNoRefdrop`, `ForkClone`, `BuggyDestroyLeak`.
- **Invariants**: `TypeOk`, `NoCycle`, `MountRefcountConsistency`, `MountRefcountNonNegative`.
- **Configs**: 1 clean + 4 buggy:
  - `territory.cfg` — TLC explores 2,560,000 distinct states; no error.
  - `territory_buggy.cfg` (BUGGY_CYCLE) — NoCycle violated at depth 4 / 106 states.
  - `territory_buggy_mount_no_refbump.cfg` — MountRefcountConsistency violated at depth 2 / 172 states.
  - `territory_buggy_unmount_no_refdrop.cfg` — MountRefcountConsistency violated at depth 3 / 884 states.
  - `territory_buggy_destroy_leak.cfg` — MountRefcountConsistency violated at depth 3 / 855 states.

| Spec action | Source location |
|---|---|
| `Init` | `kernel/territory.c::territory_init` |
| `Bind(p, src, dst)` | `kernel/territory.c::bind` |
| `Unbind(p, src, dst)` | `kernel/territory.c::unbind` |
| `Mount(p, s, path)` | `kernel/territory.c::mount` |
| `Unmount(p, s, path)` | `kernel/territory.c::unmount` |
| `ForkClone(parent, child)` | `kernel/territory.c::territory_clone` |
| `BuggyBind` / `BuggyMountNoRefbump` / `BuggyUnmountNoRefdrop` / `BuggyDestroyLeak` | none (bug classes statically prevented by impl discipline) |

| Spec invariant | Source enforcement |
|---|---|
| `NoCycle` | `bind`'s `would_create_cycle` precondition |
| `MountRefcountConsistency` | spoor_ref/spoor_unref discipline at every mount-table mutation site (mount/unmount/territory_clone/territory_unref) |
| `MountRefcountNonNegative` | `spoor_unref`'s own underflow extinct |

---

## Tests

10 tests total (3 bind-table + 7 mount-table):

### Bind-table (P2-Eb)

- `territory.bind_smoke`: alloc Territory, bind non-cyclic edges, verify nbinds + idempotent rebind detection + unbind round-trip.
- `territory.cycle_rejected`: chain `a → b → c`; attempt cycle-closing bind; verify `-1`. Self-bind rejected with `-4`.
- `territory.fork_isolated`: parent binds; territory_clone child; parent + child evolve independently.

### Mount-table (P5-attach-mount)

- `territory_mount.smoke`: mount one Spoor at a target; verify nmounts + source ref bumped; unmount; verify ref dropped.
- `territory_mount.idempotent_same_source`: re-mount same `(target, source)` is a no-op success; no second ref bump.
- `territory_mount.mrepl_replaces`: MREPL replaces an existing entry's source; old ref dropped, new ref taken; nmounts stays at 1.
- `territory_mount.unmount_missing_returns_error`: unmount of a non-existent target returns -1.
- `territory_mount.table_full`: fill `PGRP_MAX_MOUNTS` entries; next mount returns -2; overflow source's ref is NOT bumped.
- `territory_mount.clone_bumps_refs`: mount source; territory_clone parent → child; verify ref bumped to test+parent+child=3; destroy each Territory drops one ref.
- `territory_mount.destroy_drops_all_refs`: mount two sources; territory_unref → both refs dropped.

---

## Known caveats / footguns

### `path_id_t` is u32 at v1.0

The kernel-internal mount/unmount API uses abstract numeric path IDs. The fd-syscall surface (deferred) translates strings to path IDs before reaching this layer; tests pick numeric IDs.

### PGRP_MAX_BINDS = PGRP_MAX_MOUNTS = 8

Sufficient for v1.0's test scenarios + the eventual ramfs / proc / dev / ctl mount sequence at boot. Container init flows that mount more hit the per-table `-3` / `-2` errors. Phase 5+ replaces with growable RB trees keyed on qid.

### MREPL is the only mount flag with distinguished semantics at v1.0

MBEFORE / MAFTER / MCREATE are stored in the entry's `flags` but the v1.0 walk algorithm treats every entry uniformly (no union ordering). The flags are preserved for binary compatibility when Phase 5+ adds union walk support.

### `unmount` removes ONE entry per call

Plan 9's `unmount(name, old)` can remove a specific entry; `unmount(name)` removes everything at name. Thylacine's kernel-internal `unmount(territory, target_path)` removes ONE entry (the first found). To unmount a union, call repeatedly until -1.

### RFNAMEG (shared territory) is not implemented

`rfork(RFNAMEG)` extincts at v1.0. The Territory `ref` field exists for forward-looking sharing semantics but is always `1`. Phase 5+ syscall surface lands the share path with a per-Territory lock.

### Single-CPU lifecycle at v1.0

bind / unbind / mount / unmount / territory_clone are not internally synchronized; concurrent callers on different CPUs would race. At v1.0 only the boot CPU calls these (rfork is single-CPU; tests run on boot). Phase 5+ adds a per-Territory lock.

### `territory_clone` bumps refcount per-entry; failed mid-loop is partial

If `spoor_ref` were to ever fail (it extincts instead), the partial state during the deep-copy loop would have incremented refs on entries `[0..i]` but not `[i+1..]`. Since `spoor_ref` extincts on corruption (rather than returning an error), this is structurally impossible in well-formed state. The audit-trigger surface for `kernel/territory.c` covers the failure-injection case.

### Mount-syscall surface is deferred

ARCH §9.6 specifies `mount(source_spoor_fd, target_path, flags)` as a user-visible syscall. At v1.0 the fd-syscall infrastructure (open/close/read/write/dup → KOBJ_SPOOR handles) doesn't exist, so the SVC handler is deferred. Kernel-internal callers (this chunk's tests + future P5-stratumd boot path) call `mount` directly with a Spoor pointer.

---

## Status

| Component | State |
|---|---|
| `territory.h` API + `territory.c` impl (bind/unbind/mount/unmount) | **Landed (P5-attach-mount)** |
| `struct Territory.mounts[]` field + lifecycle integration | **Landed (P5-attach-mount)** |
| `territory_clone` deep-copies mounts + bumps refcounts | **Landed (P5-attach-mount)** |
| `territory_unref` final-release drops per-entry refs | **Landed (P5-attach-mount)** |
| `rfork(RFPROC)` clones mount table | **Landed (P5-attach-mount)** via territory_clone |
| `proc_free` releases mount table | **Landed (P5-attach-mount)** via territory_unref |
| Cycle detection in `bind` | Landed (P2-Eb) |
| In-kernel tests | 10 total (3 bind + 7 mount) |
| Spec `territory.tla` + buggy configs | **Landed (P5-attach-mount)** — 1 clean + 4 buggy cfgs |
| Per-Territory lock | Phase 5+ |
| RFNAMEG shared territory | Phase 5+ |
| Mount-union walk (MBEFORE/MAFTER ordering at walk time) | Phase 5+ |
| `mount` user-visible syscall (SVC handler) | Deferred to P5-fd-syscalls |
| RB tree key=qid (replacing flat arrays) | Phase 5+ when count growth justifies |
| Walk through mount entries | Phase 5+ alongside path resolution |
