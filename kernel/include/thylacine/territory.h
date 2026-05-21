// Process group (territory) — Plan 9 Territory, Thylacine adaptation
// (P2-Eb + P5-attach-mount).
//
// Per ARCHITECTURE.md §9.1 + §9.6 + specs/territory.tla. A Territory owns
// a process's namespace: two parallel tables —
//
//   binds[]   : path-to-path bind edges (Plan 9 `bind`, the symbolic
//               mapping). Walking `dst` produces `src`. Cycle-checked at
//               every bind() — invariant I-3 (mount points form a DAG).
//
//   mounts[]  : (target_path, Spoor *) grafts (Plan 9 `mount`, the
//               filesystem-as-Spoor primitive of §9.6). Walking
//               target_path dispatches through the source Spoor's Dev
//               vtable. Each entry holds one refcount on its source.
//
// At v1.0 path_id_t is `u32` — abstract path tokens. The fd-syscall
// surface (deferred) populates these with real path identifiers; tests
// pick numeric IDs.
//
// State invariants (proven in specs/territory.tla):
//
//   NoCycle (I-3): for every Territory `p` and every path `x`, x is not
//   reachable from its own binding set via the transitive closure.
//   Enforced by would_create_cycle inside bind().
//
//   MountRefcountConsistency (§9.6.6): for every Spoor s, the kernel's
//   refcount agrees with the cardinality of mount entries referencing s.
//   Maintained structurally by mount() / unmount() / territory_clone() /
//   territory_unref()'s final-release path.
//
//   Isolation (I-1): bind/mount tables are per-Territory. No action
//   updates two Territories simultaneously. RFNAMEG (shared territory)
//   is deferred to Phase 5+ syscall surface.
//
// Naming (P5-attach-mount): the verb `unmount` is reserved for the
// mount-table primitive; `unbind` is the inverse of `bind`. This matches
// Plan 9's syscall surface (bind / unbind / mount / unmount). Prior to
// this chunk the C function was misnamed `unmount`.

#ifndef THYLACINE_PGRP_H
#define THYLACINE_PGRP_H

#include <thylacine/types.h>

struct Spoor;

// PGRP_MAGIC — sentinel set at territory_alloc; checked at territory_unref
// final release. SLUB's freelist write at kmem_cache_free clobbers offset
// 0, catching double-free.
#define PGRP_MAGIC 0x50475250C0DEFADEULL    // 'PGRP' || 0xC0DE'FADE

// Bind-table size. Per the original P2-Eb sizing rationale: small fixed
// cap (8 edges per Territory). Sufficient for v1.0's bind tests + the
// eventual ramfs + /proc + /dev + /net binds.
#define PGRP_MAX_BINDS  8

// Mount-table size. Sufficient for the v1.0 boot path: root mount + a
// handful of /dev synthetic devices + /proc + /ctl. The proc.rfork
// stress test clones Territories en masse; each clone deep-copies
// mounts[] and bumps a spoor_ref per entry, so the cap is kept tight to
// hold the per-clone cost in check.
#define PGRP_MAX_MOUNTS  8

// Path identifier. At v1.0 abstract `u32` — bind/mount take whatever
// numeric ID the caller decides on (tests pick small integers). The
// fd-syscall surface (deferred) will translate strings to path IDs
// before reaching this layer.
typedef u32 path_id_t;

// One edge in the bind graph: walking `dst` produces `src` per ARCH §9.1
// ("bind(old, new) — attach old's contents at new"). Spec maps:
//   - PgrpBind.dst   <- spec's `dst` (the mount point)
//   - PgrpBind.src   <- spec's `src` (the bound content)
// An edge `(src, dst)` is `dst -> src` in the walk direction.
struct PgrpBind {
    path_id_t src;
    path_id_t dst;
};

// One entry in the mount table: at `target` in the Territory, the Spoor
// `source` is grafted. The entry holds one refcount on source; dropped
// at unmount() OR at Territory final release.
//
// Field order chosen for compact layout: pointer first (8 bytes;
// 8-byte aligned), then two u32s pack into 8 bytes. sizeof(PgrpMount) = 16.
struct PgrpMount {
    struct Spoor   *source;
    path_id_t       target;
    u32             flags;    // MREPL / MBEFORE / MAFTER / MCREATE
};

// Mount flags (ARCH §9.6.1 — mirror Plan 9). At v1.0 only MREPL has
// distinguished semantics in the impl (it replaces an existing entry at
// the same target); the others land at v1.x when union semantics get
// their own implementation work.
#define MREPL     0x0001
#define MBEFORE   0x0002
#define MAFTER    0x0004
#define MCREATE   0x0008

struct Territory {
    u64                  magic;      // PGRP_MAGIC
    int                  ref;        // refcount; rfork(RFNAMEG) shares (Phase 5+)
    int                  nbinds;
    int                  nmounts;
    u32                  _pad;       // 8-byte alignment for root_spoor + binds[]
    // P5-stratumd-stub-bringup-e2: the pivoted root Spoor (NULL until the
    // first territory_chroot). When SYS_WALK_OPEN is called with the
    // spoor_fd == -1 sentinel, the handler uses this Spoor as the walk
    // source. Holds one refcount on its target Spoor (taken at chroot,
    // dropped at re-chroot OR at territory_unref final release).
    struct Spoor        *root_spoor;
    struct PgrpBind      binds[PGRP_MAX_BINDS];
    struct PgrpMount     mounts[PGRP_MAX_MOUNTS];
};

_Static_assert(sizeof(struct PgrpMount) == 16,
               "struct PgrpMount pinned at 16 bytes (8 ptr + 4 target + 4 flags)");
_Static_assert(sizeof(struct Territory)
               == 32 + 8 * PGRP_MAX_BINDS + 16 * PGRP_MAX_MOUNTS,
               "struct Territory size pinned (P5-stratumd-stub-bringup-e2: "
               "24 header + 8 root_spoor + 8*PGRP_MAX_BINDS + 16*PGRP_MAX_MOUNTS).");
_Static_assert(__builtin_offsetof(struct Territory, magic) == 0,
               "magic must be at offset 0 (SLUB freelist write "
               "clobbers it on free, double-free defense)");
// F5 close (P5-stratumd-stub-bringup audit): pin every load-bearing
// field offset, not just the size. A future field-reorder that shifts
// root_spoor / binds / mounts without changing the total size would
// silently break syscall.c::sys_walk_open_handler's FROM_ROOT path,
// territory_chroot's ref discipline, and the mount-table iteration.
_Static_assert(__builtin_offsetof(struct Territory, root_spoor) == 24,
               "root_spoor pinned at offset 24 (after 8B magic + "
               "4B ref + 4B nbinds + 4B nmounts + 4B _pad)");
_Static_assert(__builtin_offsetof(struct Territory, binds) == 32,
               "binds[] pinned at offset 32 (after the 32B header)");
_Static_assert(__builtin_offsetof(struct Territory, mounts)
               == 32 + 8 * PGRP_MAX_BINDS,
               "mounts[] pinned after binds[]");

// Bring up the territory subsystem. Allocates the Territory SLUB cache
// and kproc's initial Territory (empty bindings, empty mounts; ref=1).
// Must be called after slub_init and BEFORE proc_init.
void territory_init(void);

// Accessor for kproc's initial Territory.
struct Territory *kpgrp(void);

// SLUB-allocate a fresh empty Territory with ref=1. Returns NULL on OOM.
struct Territory *territory_alloc(void);

// Allocate a new Territory and deep-copy parent's bindings + mounts into
// it. For every cloned mount entry, spoor_ref(source) is called — each
// clone contributes one new reference per mount entry. Returns NULL on
// OOM (parent unchanged). Models the spec's ForkClone action.
struct Territory *territory_clone(struct Territory *parent);

// Increment/decrement refcount. territory_unref frees the struct when
// the last reference goes away; the final-release path FIRST calls
// spoor_unref on every mount entry's source (releasing the per-entry
// reference held since mount()), THEN kmem_cache_free's the struct.
// Both are safe to call on NULL.
void territory_ref(struct Territory *p);
void territory_unref(struct Territory *p);

// =============================================================================
// Bind (path-to-path).
// =============================================================================

// bind: add the edge `dst -> src` to `territory`'s bind graph. Walking
// `dst` in this territory will subsequently produce `src`. Cycle-checked:
// rejects if adding the edge would create a cycle.
//
// Return values:
//    0   success.
//   -1   would create a cycle (rejects per `WouldCreateCycle` in
//        territory.tla; corresponds to Plan 9's "namespace cycle:
//        cannot bind X onto Y" errstr).
//   -2   the edge already exists (idempotent rebind is a no-op-error
//        at v1.0; future MREPL/MBEFORE/MAFTER union semantics treat
//        this differently).
//   -3   binds[] full (PGRP_MAX_BINDS reached).
//   -4   trivial self-bind (src == dst); treated as a cycle.
int bind(struct Territory *territory, path_id_t src, path_id_t dst);

// unbind: remove the edge `dst -> src` from `territory`'s bind graph.
// Returns 0 on success, -1 if the edge does not exist.
//
// (Renamed from `unmount` at P5-attach-mount — `unmount` is now the
// mount-table primitive below.)
int unbind(struct Territory *territory, path_id_t src, path_id_t dst);

// =============================================================================
// Mount (graft-a-Spoor-at-a-path).
// =============================================================================

// mount: graft Spoor `source` at `target` in `territory`'s mount table.
// Bumps source's refcount; the entry holds the reference until either
// unmount() or Territory destruction releases it.
//
// `flags` mirror Plan 9 (MREPL / MBEFORE / MAFTER / MCREATE); only MREPL
// has distinguished semantics at v1.0 — when MREPL is set and an entry
// at `target` already exists, the existing entry is replaced (its
// source's ref is dropped). The other flags are stored for future
// union-mount work; at v1.0 they're treated as "append a new entry."
//
// Idempotency: mount(territory, S, target, flags) where (target, S) is
// already in the mount table is a no-op success (returns 0; refcount not
// bumped; matches the spec's `<<path, s>> \notin mounts[p]` precondition).
//
// Return values:
//    0   success (entry added or idempotent no-op).
//   -1   source is NULL or has corrupted magic.
//   -2   mounts[] full (PGRP_MAX_MOUNTS reached).
int mount(struct Territory *territory, struct Spoor *source,
          path_id_t target, u32 flags);

// unmount: remove ONE mount entry at `target_path` from `territory`'s
// mount table and drop the source's refcount. Models the spec's Unmount
// action.
//
// If multiple entries exist at the same target (union mount), this
// removes the FIRST found; subsequent calls drop the others one by one.
//
// Return values:
//    0   success (entry removed; refcount dropped).
//   -1   no entry exists at `target_path`.
int unmount(struct Territory *territory, path_id_t target_path);

// =============================================================================
// Chroot (root-Spoor pivot) — P5-stratumd-stub-bringup-e2.
// =============================================================================

// territory_chroot: stamp `source` as `territory`'s pivoted root Spoor.
// The root is the Spoor at which name resolution starts when a userspace
// SYS_WALK_OPEN is called with the spoor_fd == -1 sentinel — i.e., when
// the caller asks the kernel to walk "from my root" rather than from an
// explicitly-named handle.
//
// Lifecycle: bumps source's refcount (the Territory holds one reference
// until either a subsequent territory_chroot replaces it OR Territory
// destruction releases it). If a previous root was set, its refcount is
// dropped via spoor_clunk (so the Dev's close hook runs if this was the
// last holder — same lifecycle discipline as mount()/MREPL displacement).
//
// Idempotency: territory_chroot(territory, S) where root_spoor == S is a
// no-op success (returns 0; refcount not bumped). Mirrors mount()'s
// idempotent-same-source semantics.
//
// Spec: maps to `specs/territory.tla::Chroot(p, s)`. Refcount discipline
// pinned by MountRefcountConsistency (refcount[s] = mount-table-count +
// |{p : root_spoor[p] = s}|).
//
// Return values:
//    0   success (root stamped or idempotent no-op).
//   -1   source is NULL.
int territory_chroot(struct Territory *territory, struct Spoor *source);

// Diagnostic.
int  territory_nbinds(struct Territory *territory);
int  territory_nmounts(struct Territory *territory);
u64  territory_total_created(void);
u64  territory_total_destroyed(void);

#endif // THYLACINE_PGRP_H
