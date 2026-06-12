# 18 — Territory primitives (P2-E + P5-attach-mount + P5-stratumd-stub-bringup-e2)

The Plan 9 territory — a process's view of the resource tree, composed via `bind` / `unbind` / `mount` / `unmount` + the v1.0 `chroot` root-pivot. Per `ARCHITECTURE.md §9.1` + `§9.6` + `CORVUS-DESIGN.md §10.1`. v1.0 lands the kernel-internal API; the user-visible `mount` / `unmount` / `chroot` syscalls were added P5-mount-syscall + P5-stratumd-stub-bringup-e2.

The Territory carries two parallel tables + one root pointer:

- **`binds[]`** — Plan 9 path-to-path bindings. Walking `dst` yields `src`. Cycle-checked.
- **`mounts[]`** — filesystem-as-Spoor grafts. Walking `target_path` dispatches through the Spoor's Dev vtable. Each entry holds one refcount on the source Spoor.
- **`root_spoor`** — the pivoted root Spoor (the v1.0 chroot mechanism per `CORVUS-DESIGN.md §10.1`). `NULL` by default; stamped via `territory_chroot`. Consumed by `SYS_WALK_OPEN(spoor_fd == -1, ...)` ("walk from my root"). Holds one refcount on its target Spoor (taken at `chroot`, dropped at re-`chroot` displacement OR at `territory_unref` final release). Spec: `root_spoor[p]` ∈ `Spoors ∪ {NONE}` in `specs/territory.tla`.

---

## Purpose

A `Territory` (process group, Plan 9 idiom) holds one process's namespace. Each Proc has its own `Territory` at v1.0; RFNAMEG-shared territories are Phase 5+ syscall surface.

Key invariants (proven in `specs/territory.tla`):

- **Cycle-freedom (I-3)**: the bind graph is acyclic. Adding a bind that would close a cycle is rejected.
- **MountRefcountConsistency (§9.6.6, extended P5-stratumd-stub-bringup-e2)**: for every Spoor `s`, the kernel's refcount equals `|MountEntriesForSpoor(s)| + |{p : root_spoor[p] = s}|` — the per-Territory contribution now includes both mount-table entries AND `root_spoor` pivots. Maintained by `mount` (bump) / `unmount` (drop) / `territory_clone` (bump per cloned entry + bump for cloned root_spoor) / `territory_chroot` (bump new + drop displaced) / `territory_unref` (drop per remaining entry + drop root_spoor at final release).
- **Isolation (I-1)**: structural — `bindings[p]` / `mounts[p]` / `root_spoor[p]` for different Territories are independent.

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

struct PgrpMount {                              // stalk-2: re-keyed from an
    struct Spoor   *source;                     //   abstract path_id_t target to
    u64             mp_qid_path;                //   the mount point's full Plan 9
    int             mp_dc;                       //   (type, dev, qid) identity:
    u32             mp_devno;                   //   (dc, devno, qid.path).
    u32             flags;                      // MREPL | MBEFORE | MAFTER | MCREATE
    u32             _pad;                        // 8-byte array-stride alignment
};
_Static_assert(sizeof(struct PgrpMount) == 32, ...);   // was 16 (stalk-2)

#define MREPL    0x0001
#define MBEFORE  0x0002
#define MAFTER   0x0004
#define MCREATE  0x0008

struct Territory {
    u64                 magic;                  // PGRP_MAGIC
    int                 ref;                    // refcount; rfork(RFNAMEG) shares (Phase 5+)
    int                 nbinds;
    int                 nmounts;
    u32                 _pad;                   // 8-byte alignment for root_spoor + binds[]
    struct Spoor       *root_spoor;             // P5-stratumd-stub-bringup-e2; NULL until first chroot
    struct PgrpBind     binds[PGRP_MAX_BINDS];
    struct PgrpMount    mounts[PGRP_MAX_MOUNTS];
};
_Static_assert(sizeof(struct Territory)
               == 32 + 8 * PGRP_MAX_BINDS + 32 * PGRP_MAX_MOUNTS, ...);  // mounts[] 16->32/entry

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
                         struct Spoor *mountpoint, u32 flags);   // stalk-2: Spoor-keyed
int                unmount(struct Territory *p, struct Spoor *mountpoint);
struct Spoor      *mount_lookup(struct Territory *p, struct Spoor *probe);  // stalk-2: domount probe

int                territory_chroot(struct Territory *p, struct Spoor *source);

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

### `mount(p, source, mountpoint, flags)` — return semantics

stalk-2: keyed by the `mountpoint` Spoor's `(dc, devno, qid.path)` identity (the
Spoor is NOT retained -- only its identity is copied; the caller `stalk`s it and
clunks it). `mount_lookup(p, probe)` is the `stalk` cross-mount probe -- it
returns a **REF-HELD** source of the first entry matching `probe`'s identity, or
NULL. **The caller MUST `spoor_clunk` the result** (RW-4 SA-F1 changed the contract
from borrow to owned: the lookup + `spoor_ref` happen atomically under `ns_lock` so
a concurrent `unmount` cannot free the source mid-cross; `stalk_cross_mounts` clunks
it after `clone_walk_zero`). `territory_root_ref(p)` is the companion atomic
read+ref of `root_spoor` (caller clunks) -- the only sound way to take the FROM_ROOT
walk base in a multi-thread Proc. See `docs/reference/104-stalk.md` "Mount crossing".

| Return | Meaning |
|---|---|
| `0`  | success (entry added or idempotent no-op) |
| `-1` | source or mountpoint NULL / corrupted |
| `-2` | mounts[] full (PGRP_MAX_MOUNTS reached) |

- **Idempotency**: re-mounting the same `(key(mountpoint), source)` pair is a no-op success; no second `spoor_ref`.
- **MREPL**: if `flags & MREPL` and an entry at the same mount-point identity exists with a different `source`, the existing entry is replaced (old source's ref dropped; new source's ref taken); `nmounts` unchanged. (Re-mounting onto an already-mounted point keys on the SAME underlying identity because `SYS_MOUNT` resolves it with `STALK_MOUNT` -- the final mount is NOT crossed.)
- **MBEFORE / MAFTER / MCREATE**: recorded in the entry's `flags` field; at v1.0 treated as "append a new entry" for union mounts. Union walk semantics land at Phase 5+ when the walk algorithm grows union support.

### `unmount(p, mountpoint)` — return semantics

| Return | Meaning |
|---|---|
| `0`  | success; one entry removed; source's refcount dropped |
| `-1` | no entry matching `mountpoint`'s identity / mountpoint NULL |

Removes the FIRST entry at `target_path`. For union mounts with multiple entries, call repeatedly.

### `territory_chroot(p, source)` — return semantics

| Return | Meaning |
|---|---|
| `0`  | success; `root_spoor` stamped (or idempotent no-op if already pointing at `source`) |
| `-1` | `source` is NULL |

Lifecycle:
- **Bump-before-swap** discipline: `spoor_ref(source)` runs before the pointer assignment, so a corrupted source (which would extinct in `spoor_ref`) leaves `root_spoor` unchanged.
- **`spoor_clunk` on displaced root**: if a prior `root_spoor` exists, it is `spoor_clunk`'d after the new pointer is installed — same discipline as `mount()`'s MREPL displacement, so the Dev's close hook fires when this was the Spoor's last holder.
- **Idempotent same-source**: `territory_chroot(p, S)` where `root_spoor == S` is a no-op success; refcount unchanged.

Spec: `specs/territory.tla::Chroot(p, s)`.

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

### Chroot (root-pivot) — P5-stratumd-stub-bringup-e2

```c
int territory_chroot(struct Territory *p, struct Spoor *source) {
    // Validate source. Idempotent same-pointer → 0. Else:
    //   spoor_ref(source);  (bump BEFORE swap)
    //   old = p->root_spoor;
    //   p->root_spoor = source;
    //   if (old) spoor_clunk(old);  (MREPL-style displacement)
    //   return 0;
}
```

Refcount discipline:

| Step | Old | New |
|---|---|---|
| First chroot (`old == NULL`) | n/a | `spoor_ref(new)`; +1 to `refcount[new]` |
| Idempotent (`old == new`) | unchanged | unchanged; no ref bump |
| Replace (`old != NULL`, `old != new`) | `spoor_clunk(old)`; -1 | `spoor_ref(new)`; +1 |

### Integration with rfork

`kernel/proc.c::rfork` calls `territory_clone(parent->territory)` which deep-copies the mount table AND `root_spoor`, bumping a fresh `spoor_ref` on each cloned mount entry and on the cloned `root_spoor` (if non-NULL). No change at the rfork call site; the discipline is inside `territory_clone`.

`kernel/proc.c::proc_free` calls `territory_unref(p->territory)`. The unref's final-release path drops each mount entry's per-entry refcount AND drops `root_spoor`'s refcount (via `spoor_clunk` so the Dev's close hook runs if this was the last holder), BEFORE freeing the Territory.

---

## Spec cross-reference

`specs/territory.tla` at P5-stratumd-stub-bringup-e2:

- **State**: `bindings`, `mounts`, `root_spoor`, `refcount`. `NONE == "NONE"` (string sentinel; guaranteed distinct from the symbolic Spoor model values).
- **Actions**: `Init`, `Bind`, `BuggyBind`, `Unbind`, `Mount`, `BuggyMountNoRefbump`, `Unmount`, `BuggyUnmountNoRefdrop`, `Chroot`, `BuggyChrootNoRefbump`, `ForkClone`, `BuggyDestroyLeak`.
- **Invariants**: `TypeOk`, `NoCycle`, `MountRefcountConsistency` (extended: `refcount[s] = |MountEntriesForSpoor(s)| + |{p : root_spoor[p] = s}|`), `MountRefcountNonNegative`.
- **Configs**: 1 clean + 5 buggy:
  - `territory.cfg` — clean.
  - `territory_buggy.cfg` (BUGGY_CYCLE) — NoCycle violated.
  - `territory_buggy_mount_no_refbump.cfg` — MountRefcountConsistency violated.
  - `territory_buggy_unmount_no_refdrop.cfg` — MountRefcountConsistency violated.
  - `territory_buggy_destroy_leak.cfg` — MountRefcountConsistency violated.
  - `territory_buggy_chroot_no_refbump.cfg` (P5-stratumd-stub-bringup-e2) — MountRefcountConsistency violated at depth 2 / 205 states (BuggyChrootNoRefbump stamps `root_spoor[p]` without bumping `refcount[s]` or dropping the old root's contribution).

| Spec action | Source location |
|---|---|
| `Init` | `kernel/territory.c::territory_init` |
| `Bind(p, src, dst)` | `kernel/territory.c::bind` |
| `Unbind(p, src, dst)` | `kernel/territory.c::unbind` |
| `Mount(p, s, path)` | `kernel/territory.c::mount` |
| `Unmount(p, s, path)` | `kernel/territory.c::unmount` |
| `Chroot(p, s)` | `kernel/territory.c::territory_chroot` |
| `ForkClone(parent, child)` | `kernel/territory.c::territory_clone` |
| `BuggyBind` / `BuggyMountNoRefbump` / `BuggyUnmountNoRefdrop` / `BuggyChrootNoRefbump` / `BuggyDestroyLeak` | none (bug classes statically prevented by impl discipline) |

| Spec invariant | Source enforcement |
|---|---|
| `NoCycle` | `bind`'s `would_create_cycle` precondition |
| `MountRefcountConsistency` (extended) | `spoor_ref`/`spoor_clunk` discipline at every mount-table + root_spoor mutation site (`mount` / `unmount` / `territory_chroot` / `territory_clone` / `territory_unref`) |
| `MountRefcountNonNegative` | `spoor_unref`'s own underflow extinct |

---

## Tests

16 tests total (3 bind-table + 7 mount-table + 6 chroot):

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

### Chroot (P5-stratumd-stub-bringup-e2)

- `territory.chroot_smoke`: chroot one Spoor; verify root_spoor + ref bumped 1→2 (test + Territory); territory_unref drops back to 1.
- `territory.chroot_idempotent_same_spoor`: chroot same Spoor twice; second call is 0 no-op; ref unchanged.
- `territory.chroot_replace_clunks_old`: chroot s1 then chroot s2; verify s1's per-Territory ref dropped, s2's bumped.
- `territory.chroot_clone_bumps_ref`: chroot, then territory_clone parent → child; verify root_spoor ref += 1 (test + parent + child). Destroy each → drops 1.
- `territory.chroot_destroy_drops_ref`: chroot + territory_unref → root ref dropped (final-release path's `spoor_clunk` on root_spoor).
- `territory.chroot_null_returns_error`: territory_chroot(p, NULL) returns -1; no state change.

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

`rfork(RFNAMEG)` extincts at v1.0, so the Territory `ref` field is normally `1` (its multi-holder semantics are forward-looking). The per-Territory `ns_lock` already serializes multi-**Thread** access (peer Threads of one Proc share the Territory -- the RW-4 SA-F1 surface); the RFNAMEG cross-**Proc** *share* path is the remaining Phase 5+ work.

### Per-Territory locking (RW-4 SA-F1)

`mount` / `unmount` / `bind` / `unbind` / `territory_chroot` / `territory_pivot_root` / `territory_clone` / `mount_lookup` / `territory_root_ref` serialize `mounts[]` / `nmounts` / `binds[]` / `nbinds` / `root_spoor` under the per-Territory **`ns_lock`** (a near-leaf spinlock). Peer Threads of a Proc share the Territory, so a concurrent `pivot_root` / `unmount` on one thread must not free a Spoor a walking thread is mid-read on -- `ns_lock` closes that UAF (the #848 race, promoted P3-dormant -> P1 by the P6 multi-thread lift and fixed in RW-4). The lock is held ONLY for the table read-modify-write, **NEVER across `stalk`** (it blocks on 9P) or across a `spoor_clunk` (the Dev close hook may sleep): the displaced/removed source is captured under the lock and clunked outside it (the `dot_lock` discipline). The `cwd` (`dot_path`) has its own separate `dot_lock`.

### `territory_clone` bumps refcount per-entry; failed mid-loop is partial

If `spoor_ref` were to ever fail (it extincts instead), the partial state during the deep-copy loop would have incremented refs on entries `[0..i]` but not `[i+1..]`. Since `spoor_ref` extincts on corruption (rather than returning an error), this is structurally impossible in well-formed state. The audit-trigger surface for `kernel/territory.c` covers the failure-injection case.

### Mount-syscall surface

ARCH §9.6 specifies `mount(source_spoor_fd, target_path, flags)` as a user-visible syscall. At v1.0 it landed at P5-mount-syscall (`SYS_MOUNT` = 14, `SYS_UNMOUNT` = 15). Kernel-internal callers (tests + the joey stub-bringup path) call `mount`/`unmount` directly with a Spoor pointer.

### `chroot` is one-way at v1.0 (no `unchroot`)

`SYS_CHROOT` stamps `root_spoor`; there is no `SYS_UNCHROOT` (or `chroot(NULL)`-clear) at v1.0. A long-running Proc that pivots cannot un-pivot mid-life — the `root_spoor` reference is released only at Proc exit (via `territory_unref`'s final-release path). This is the load-bearing reason joey does NOT exercise `chroot` in its stub-bringup phase: joey is the init Proc that never exits during boot, so an in-flight chroot on the attach Spoor would hold the underlying `p9_attached` + transport-Spoors alive past joey's `t_close(attach_fd)`, the stratumd-stub would never see EOF on its `c2s_rd`, and `t_wait_pid` would deadlock. P5-stratumd-stub-bringup-e2 routes the chroot path through `stub-walk-probe` (a child Proc whose exit naturally releases the chroot) + the six `territory.chroot_*` kernel-internal tests. v1.x adds proper `pivot_root` semantics (per `CORVUS-DESIGN.md §10.1 Q2`).

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
| `struct Territory.root_spoor` field + `territory_chroot` | **Landed (P5-stratumd-stub-bringup-e2)** |
| `territory_clone` deep-copies root_spoor + bumps refcount | **Landed (P5-stratumd-stub-bringup-e2)** |
| `territory_unref` final-release drops root_spoor ref | **Landed (P5-stratumd-stub-bringup-e2)** |
| `mount` / `unmount` user-visible syscalls (SYS_MOUNT / SYS_UNMOUNT) | **Landed (P5-mount-syscall)** |
| `chroot` user-visible syscall (SYS_CHROOT) | **Landed (P5-stratumd-stub-bringup-e2)** |
| `PgrpMount.mp_path` (the mount-point's namespace name, I-33) + lifecycle (ref at mount/MREPL; share at clone; drop at unmount/MREPL-displace/final-release) | **Landed (#66b)** — `struct PgrpMount` 32→40B; introspection-only (the table keys on `(dc, devno, qid.path)`, never `mp_path`) |
| `territory_format_ns` (renders the mount list for `/proc/<pid>/ns`, under `ns_lock`) | **Landed (#66b)** — see `docs/reference/32-devproc.md` |
| In-kernel tests | 16 total (3 bind + 7 mount + 6 chroot) |
| Spec `territory.tla` + buggy configs | **Landed (P5-stratumd-stub-bringup-e2)** — 1 clean + 5 buggy cfgs |
| Per-Territory `ns_lock` (mounts/binds/root_spoor) | **Done (RW-4 SA-F1)** |
| RFNAMEG cross-Proc shared territory | Phase 5+ |
| RFNAMEG shared territory | Phase 5+ |
| Mount-union walk (MBEFORE/MAFTER ordering at walk time) | Phase 5+ |
| `pivot_root` / `unchroot` (replace one-way chroot) | v1.x per CORVUS-DESIGN §10.1 Q2 |
| RB tree key=qid (replacing flat arrays) | Phase 5+ when count growth justifies |
| Multi-component walker consuming mount table | Phase 5+ alongside path resolution |
