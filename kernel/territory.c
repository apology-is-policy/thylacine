// Territory primitives — Plan 9 bind/unbind/mount/unmount + Territory
// lifecycle (P2-Eb + P5-attach-mount).
//
// Per ARCHITECTURE.md §9.1 + §9.6 + specs/territory.tla. Implements the
// spec's Bind / Unbind / Mount / Unmount / ForkClone actions on a
// SLUB-allocated Territory struct holding fixed-size bind + mount
// tables.
//
// State invariants:
//
//   NoCycle (ARCH §28 I-3): for every Territory `p` and every path `x`,
//   `x` is not reachable from its own binding set via the transitive
//   closure. Enforced by `would_create_cycle` (DFS over existing edges)
//   called from `bind()` BEFORE inserting; if a cycle would form, bind
//   returns -1.
//
//   MountRefcountConsistency (ARCH §9.6.6): every mount entry holds one
//   refcount on its source Spoor. Maintained by:
//     - mount(): spoor_ref(source) before insert.
//     - unmount(): spoor_unref(source) after remove.
//     - territory_clone(): spoor_ref(source) per cloned entry.
//     - territory_unref() final release: spoor_unref(source) for each
//       entry BEFORE kmem_cache_free.
//
//   Isolation (ARCH §28 I-1): all operations take a single Territory *.
//   Two Territories' tables are never modified in the same call.
//
// Bootstrap order (kernel/main.c calls):
//   1. slub_init
//   2. territory_init       — Territory SLUB cache + kproc's empty Territory
//   3. proc_init            — kproc; assigns kproc->territory = kpgrp()
//   4. thread_init          — kthread
//   5. dev_init             — which internally calls spoor_init (Spoor cache)
//
// territory_init runs BEFORE spoor_init. Safe because territory_init
// only allocates EMPTY Territories (nmounts = 0); the territory_unref
// final-release path only calls spoor_unref when nmounts > 0, which
// requires mount() to have been called first, which requires a Spoor,
// which requires spoor_init. The dependency is therefore satisfied
// automatically by call ordering, not by init ordering.

#include <thylacine/extinction.h>
#include <thylacine/spoor.h>
#include <thylacine/territory.h>
#include <thylacine/types.h>

#include "../mm/slub.h"

static struct kmem_cache *g_pgrp_cache;
static struct Territory  *g_kpgrp;
static u64                g_pgrp_created;
static u64                g_pgrp_destroyed;

// =============================================================================
// Territory lifecycle.
// =============================================================================

static void territory_init_fields(struct Territory *p) {
    p->magic      = PGRP_MAGIC;
    p->ref        = 1;
    p->nbinds     = 0;
    p->nmounts    = 0;
    p->root_spoor = NULL;
    // KP_ZERO already cleared binds[] and mounts[]; nothing more to do.
}

void territory_init(void) {
    if (g_pgrp_cache) extinction("territory_init called twice");

    g_pgrp_cache = kmem_cache_create("territory",
                                     sizeof(struct Territory),
                                     8,
                                     KMEM_CACHE_PANIC_ON_FAIL);
    if (!g_pgrp_cache) {
        extinction("kmem_cache_create(territory) returned NULL");
    }

    g_kpgrp = kmem_cache_alloc(g_pgrp_cache, KP_ZERO);
    if (!g_kpgrp) extinction("kmem_cache_alloc(kpgrp) failed");
    territory_init_fields(g_kpgrp);
    __atomic_fetch_add(&g_pgrp_created, 1u, __ATOMIC_RELAXED);
}

struct Territory *kpgrp(void) {
    return g_kpgrp;
}

struct Territory *territory_alloc(void) {
    if (!g_pgrp_cache) extinction("territory_alloc before territory_init");
    struct Territory *p = kmem_cache_alloc(g_pgrp_cache, KP_ZERO);
    if (!p) return NULL;
    territory_init_fields(p);
    __atomic_fetch_add(&g_pgrp_created, 1u, __ATOMIC_RELAXED);
    return p;
}

struct Territory *territory_clone(struct Territory *parent) {
    if (!parent)                  extinction("territory_clone(NULL)");
    if (parent->magic != PGRP_MAGIC)
        extinction("territory_clone of corrupted Territory");
    // R5-H F92: defense-in-depth against single-bit-flip / partial-
    // corruption that leaves magic intact but corrupts the count
    // fields. The bound is fixed at compile time; validating here
    // catches a torn nbinds/nmounts before the deep-copy loop reads
    // past the array.
    if ((unsigned)parent->nbinds > PGRP_MAX_BINDS)
        extinction("territory_clone: parent->nbinds out of range (corrupted Territory)");
    if ((unsigned)parent->nmounts > PGRP_MAX_MOUNTS)
        extinction("territory_clone: parent->nmounts out of range (corrupted Territory)");

    struct Territory *child = territory_alloc();
    if (!child) return NULL;

    // Deep-copy the bind table. Models the spec's ForkClone action's
    // bindings update: the child gets an independent function value.
    child->nbinds = parent->nbinds;
    for (int i = 0; i < parent->nbinds; i++) {
        child->binds[i] = parent->binds[i];
    }

    // Deep-copy the mount table. For each cloned entry, spoor_ref(source)
    // — each Territory holding an entry contributes one reference per
    // entry. Models the spec's ForkClone refcount update:
    //   refcount' = refcount + Cardinality(entries-in-parent-pointing-at-s)
    //
    // spoor_ref extincts on a NULL or corrupted source — that's a kernel
    // invariant violation (we put a corrupted Spoor into the mount table)
    // and the extinct is the correct response.
    child->nmounts = parent->nmounts;
    for (int i = 0; i < parent->nmounts; i++) {
        child->mounts[i] = parent->mounts[i];
        spoor_ref(child->mounts[i].source);
    }

    // Deep-copy the root_spoor pivot (P5-stratumd-stub-bringup-e2). Each
    // clone is its own holder; spoor_ref(parent->root_spoor) here is the
    // analog of the per-mount-entry ref bump above. Models the spec's
    // ForkClone refcount update: + (IF root_spoor[parent] = s THEN 1
    // ELSE 0).
    child->root_spoor = parent->root_spoor;
    if (child->root_spoor) {
        spoor_ref(child->root_spoor);
    }
    return child;
}

void territory_ref(struct Territory *p) {
    if (!p) return;
    if (p->magic != PGRP_MAGIC)   extinction("territory_ref of corrupted Territory");
    __atomic_fetch_add(&p->ref, 1, __ATOMIC_RELAXED);
}

void territory_unref(struct Territory *p) {
    if (!p) return;
    if (p->magic != PGRP_MAGIC)   extinction("territory_unref of corrupted Territory");
    if (p == g_kpgrp)             extinction("territory_unref attempted on kpgrp");
    int pre = __atomic_fetch_sub(&p->ref, 1, __ATOMIC_ACQ_REL);
    if (pre <= 0)                 extinction("territory_unref of zero-ref Territory");
    if (pre == 1) {
        // Final release. Drop each mount entry's source refcount BEFORE
        // freeing the Territory's storage. Models the spec's
        // "DestroyTerritory requires mounts[p] = {}" precondition: the
        // impl satisfies it by iterating + spoor_unref'ing each entry
        // here, which is equivalent to a sequence of Unmount actions
        // immediately preceding the destroy.
        //
        // If this loop is skipped (the BUGGY_DESTROY_LEAK class), each
        // source's refcount stays bumped while the entries vanish from
        // mounts[] — the Spoor's storage is leaked. The spec's
        // MountRefcountConsistency invariant catches that desync.
        //
        // Use spoor_clunk (Plan 9 cclose) not spoor_unref: if this is
        // the Spoor's LAST holder (e.g., the user already closed any
        // attach_9p fd; this Territory was the last mount-table entry
        // for it), the Dev's close hook needs to run to release
        // per-Spoor Dev state (pipe endpoints, 9P sessions, etc.).
        // P5-mount-syscall closed this gap; pre-fix, spoor_unref's
        // close-less last-drop would have leaked Dev state on
        // Territory destruction.
        //
        // Walk in reverse so the array indexing stays valid even if we
        // ever add side-effects that clear the slot during the close
        // path; current Dev close hooks don't touch the Territory, so
        // the direction is cosmetic.
        for (int i = p->nmounts - 1; i >= 0; i--) {
            spoor_clunk(p->mounts[i].source);
        }
        // Clear nmounts so a post-free read (UAF) sees consistent state
        // (zero entries) — SLUB's freelist write will clobber magic but
        // not the count fields immediately; defensive.
        p->nmounts = 0;

        // Drop the root_spoor pivot's per-Territory ref. Mirrors the
        // mount-entry discipline above: spoor_clunk (not spoor_unref) so
        // the Dev's close hook runs if this was the last holder
        // (e.g., the user already closed any attach_9p fd; this Territory
        // was the only remaining holder of the pivoted root). Same
        // rationale as P5-mount-syscall's mount-table-drop fix.
        // P5-stratumd-stub-bringup-e2.
        if (p->root_spoor) {
            struct Spoor *r = p->root_spoor;
            p->root_spoor = NULL;
            spoor_clunk(r);
        }

        kmem_cache_free(g_pgrp_cache, p);
        __atomic_fetch_add(&g_pgrp_destroyed, 1u, __ATOMIC_RELAXED);
    }
}

int territory_nbinds(struct Territory *p) {
    if (!p) return 0;
    return p->nbinds;
}

int territory_nmounts(struct Territory *p) {
    if (!p) return 0;
    return p->nmounts;
}

u64 territory_total_created(void)   { return __atomic_load_n(&g_pgrp_created, __ATOMIC_RELAXED); }
u64 territory_total_destroyed(void) { return __atomic_load_n(&g_pgrp_destroyed, __ATOMIC_RELAXED); }

// =============================================================================
// bind / unbind.
// =============================================================================

// Path-in-set helper. Linear scan; PGRP_MAX_BINDS+1 is the worst-case
// `reachable[]` size, so this is at most ~33 comparisons.
static bool path_in(const path_id_t *arr, int n, path_id_t p) {
    for (int i = 0; i < n; i++) {
        if (arr[i] == p) return true;
    }
    return false;
}

// would_create_cycle(territory, src, dst): would adding the edge `dst -> src`
// produce a cycle in territory's bind graph?
//
// Algorithm: starting from {src}, iteratively follow existing edges in
// the walk direction (for each existing edge `e_dst -> e_src`, if
// e_dst is in the reachable set, add e_src). After fixed point, check
// whether `dst` is reachable from `src`. If yes, the new edge `dst ->
// src` would close a cycle: src -> ... -> dst -> (new) -> src.
//
// Maps directly to the spec's WouldCreateCycle.
static bool would_create_cycle(const struct Territory *territory,
                               path_id_t src, path_id_t dst) {
    if (src == dst) return true;

    path_id_t reachable[PGRP_MAX_BINDS + 1];
    int n = 0;
    reachable[n++] = src;

    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < territory->nbinds; i++) {
            path_id_t e_src = territory->binds[i].src;
            path_id_t e_dst = territory->binds[i].dst;
            if (path_in(reachable, n, e_dst) &&
                !path_in(reachable, n, e_src)) {
                if (n >= PGRP_MAX_BINDS + 1) break;
                reachable[n++] = e_src;
                changed = true;
            }
        }
    }

    return path_in(reachable, n, dst);
}

int bind(struct Territory *territory, path_id_t src, path_id_t dst) {
    if (!territory)                    extinction("bind(NULL territory)");
    if (territory->magic != PGRP_MAGIC)
        extinction("bind on corrupted Territory");

    if (src == dst)                       return -4;
    if (would_create_cycle(territory, src, dst)) return -1;

    for (int i = 0; i < territory->nbinds; i++) {
        if (territory->binds[i].src == src && territory->binds[i].dst == dst) {
            return -2;
        }
    }

    if (territory->nbinds >= PGRP_MAX_BINDS)   return -3;

    territory->binds[territory->nbinds].src = src;
    territory->binds[territory->nbinds].dst = dst;
    territory->nbinds++;
    return 0;
}

int unbind(struct Territory *territory, path_id_t src, path_id_t dst) {
    if (!territory)                    extinction("unbind(NULL territory)");
    if (territory->magic != PGRP_MAGIC)
        extinction("unbind on corrupted Territory");

    for (int i = 0; i < territory->nbinds; i++) {
        if (territory->binds[i].src == src && territory->binds[i].dst == dst) {
            territory->binds[i] = territory->binds[territory->nbinds - 1];
            territory->nbinds--;
            return 0;
        }
    }
    return -1;
}

// =============================================================================
// mount / unmount.
// =============================================================================

// Spoor magic check helper. The Spoor's magic is at offset 0; we read it
// without dereferencing through the type (would require <spoor.h>'s full
// struct knowledge — already included). spoor_ref does its own check
// and extincts on mismatch, so we just need to reject NULL early.
//
// Defined inline as a function — caller does:
//   if (!source_is_valid(source)) return -1;
// before any ref-affecting op.
static bool source_is_valid(struct Spoor *s) {
    if (!s) return false;
    // SPOOR_MAGIC check is encapsulated by spoor_ref's own invariant;
    // testing it here would duplicate. Rely on spoor_ref's extinct
    // discipline — if the Spoor is corrupted, the spoor_ref call below
    // will extinct cleanly with a precise message.
    return true;
}

int mount(struct Territory *territory, struct Spoor *source,
          path_id_t target, u32 flags) {
    if (!territory)                    extinction("mount(NULL territory)");
    if (territory->magic != PGRP_MAGIC) extinction("mount on corrupted Territory");

    if (!source_is_valid(source))       return -1;

    // Idempotency: (target, source) pair already in the table → no-op.
    // Spec: <<path, s>> \notin mounts[p] precondition. Caller doesn't
    // see a refcount bump; the existing entry's refcount is unchanged.
    for (int i = 0; i < territory->nmounts; i++) {
        if (territory->mounts[i].target == target &&
            territory->mounts[i].source == source) {
            return 0;
        }
    }

    // MREPL semantics at v1.0: if `flags & MREPL` and an entry at the
    // same target exists (with a different source — we already checked
    // for idempotent same-source above), replace the FIRST matching
    // entry. Drop the old source's refcount; install the new source
    // with a fresh ref.
    //
    // MBEFORE/MAFTER/MCREATE union semantics are recorded but treated
    // as "append a new entry" at v1.0; union walking is Phase 5+ once
    // the walk algorithm grows union support.
    if (flags & MREPL) {
        for (int i = 0; i < territory->nmounts; i++) {
            if (territory->mounts[i].target == target) {
                struct Spoor *old = territory->mounts[i].source;
                territory->mounts[i].source = source;
                territory->mounts[i].flags  = flags;
                spoor_ref(source);
                // spoor_clunk (not spoor_unref): MREPL displaces a
                // holder; if this was the last ref on `old`, the
                // Dev's close hook must run to release per-Spoor
                // state. P5-mount-syscall fix.
                spoor_clunk(old);
                return 0;
            }
        }
        // MREPL with no existing entry: fall through to append.
    }

    if (territory->nmounts >= PGRP_MAX_MOUNTS)  return -2;

    // Take the per-entry reference BEFORE installing the entry. If the
    // ref bump succeeds, the entry is committed; if installation fails
    // (it can't here — we already validated nmounts), we'd need to
    // un-ref. Since the path below is infallible after the ref, no
    // rollback is needed.
    spoor_ref(source);

    territory->mounts[territory->nmounts].target = target;
    territory->mounts[territory->nmounts].source = source;
    territory->mounts[territory->nmounts].flags  = flags;
    territory->nmounts++;
    return 0;
}

int unmount(struct Territory *territory, path_id_t target_path) {
    if (!territory)                    extinction("unmount(NULL territory)");
    if (territory->magic != PGRP_MAGIC) extinction("unmount on corrupted Territory");

    for (int i = 0; i < territory->nmounts; i++) {
        if (territory->mounts[i].target == target_path) {
            struct Spoor *source = territory->mounts[i].source;
            // Remove by swapping with the last element. Order within
            // mounts[] is not load-bearing at v1.0; MBEFORE/MAFTER union
            // walking (Phase 5+) will introduce an ordering invariant
            // and the removal will switch to shift-down at that point.
            territory->mounts[i] = territory->mounts[territory->nmounts - 1];
            territory->nmounts--;
            // Drop the per-entry refcount LAST. spoor_clunk (not
            // spoor_unref) so the Dev's close hook runs if this is
            // the last holder — required for the ARCH §9.6.6
            // lifecycle where a user has already closed the
            // attach_9p fd and the mount-table was the only holder
            // keeping the 9P session alive. P5-mount-syscall fix.
            spoor_clunk(source);
            return 0;
        }
    }
    return -1;
}

// =============================================================================
// chroot (root-Spoor pivot) — P5-stratumd-stub-bringup-e2.
// =============================================================================

// Maps to specs/territory.tla::Chroot(p, s). Refcount discipline:
//
//   - if old == NULL: spoor_ref(new); root_spoor = new. (single bump)
//   - if old == new: idempotent no-op; no ref change.
//   - if old != new, old != NULL: spoor_ref(new); root_spoor = new;
//     spoor_clunk(old). (one bump, one drop — matches the spec's
//     two-key EXCEPT update in Chroot.)
//
// spoor_ref BEFORE the pointer swap so a failure (spoor_ref extincts on
// corruption; doesn't fail-soft) leaves root_spoor unchanged. After the
// swap, the old pointer is still locally referenced for the spoor_clunk.
// spoor_clunk (not spoor_unref) for the displaced root: if the previous
// root held its last ref via this Territory's root_spoor, the Dev's
// close hook needs to run to release per-Spoor Dev state — same
// discipline as MREPL displacement in mount().
int territory_chroot(struct Territory *territory, struct Spoor *source) {
    if (!territory)                    extinction("territory_chroot(NULL territory)");
    if (territory->magic != PGRP_MAGIC) extinction("territory_chroot on corrupted Territory");

    if (!source_is_valid(source))       return -1;

    // Idempotent same-pointer: same Spoor reasserted as root → no-op
    // success; refcount unchanged. Matches spec precondition
    // `root_spoor[p] # s` (the action simply doesn't fire when s ==
    // old).
    if (territory->root_spoor == source) return 0;

    struct Spoor *old = territory->root_spoor;

    // Bump BEFORE swap. spoor_ref extincts on a corrupted source; under
    // a clean Spoor the swap is then infallible.
    spoor_ref(source);
    territory->root_spoor = source;

    if (old) {
        // spoor_clunk (not spoor_unref) — see header comment + mount()
        // MREPL precedent. If this Territory was the last holder of
        // `old`, the Dev's close hook runs here.
        spoor_clunk(old);
    }
    return 0;
}

// =============================================================================
// Pivot root (long-running-Proc root swap) — P6-pouch-stratumd-boot 16c.
// =============================================================================
//
// Semantics identical to territory_chroot's atomic swap, with one
// added pre-condition: REQUIRES root_spoor != NULL. The semantic
// distinction (pivot vs initial chroot) is enforced at this layer so
// SYS_PIVOT_ROOT cannot be used to establish an initial root on a
// fresh Territory; that's territory_chroot's job. A pivot on a no-root
// Territory is a contract violation and returns -1.
//
// Implementation footprint deliberately mirrors territory_chroot's
// post-precondition path; refactoring to a shared helper is deferred
// to a v1.x cleanup. At v1.0 the two semantics are explicit and
// independent so a future divergence (e.g., v1.x bind-survivor
// semantics in pivot but NOT chroot) lands as a localized change.
int territory_pivot_root(struct Territory *territory, struct Spoor *source) {
    if (!territory)                    extinction("territory_pivot_root(NULL territory)");
    if (territory->magic != PGRP_MAGIC) extinction("territory_pivot_root on corrupted Territory");

    if (!source_is_valid(source))       return -1;

    // Pre-condition: pivot requires an existing root. A Territory with
    // NULL root_spoor has never been chrooted (only kproc + Procs
    // whose ancestor never chrooted are in this state at v1.0; userspace
    // Procs that reach SYS_PIVOT_ROOT will always have a root inherited
    // from kproc's territory_chroot at boot). Returning -1 here makes
    // the syscall fail-closed -- the caller MUST have established a
    // root via SYS_CHROOT first (which all v1.0 Procs do, via kproc).
    if (!territory->root_spoor)         return -1;

    // Idempotent same-pointer: same Spoor reasserted as root → no-op
    // success; refcount unchanged. Matches territory_chroot's
    // behavior + Plan 9 / Linux pivot-to-same semantics.
    if (territory->root_spoor == source) return 0;

    struct Spoor *old = territory->root_spoor;

    // Bump BEFORE swap. spoor_ref extincts on a corrupted source under
    // its own invariant; pre-swap any failure leaves the territory
    // unchanged. Post-swap the swap is infallible.
    spoor_ref(source);
    territory->root_spoor = source;

    // spoor_clunk (NOT spoor_unref) on the displaced root: if THIS
    // Territory was the last holder, the Dev's close hook runs. Same
    // discipline as territory_chroot + mount()'s MREPL displacement.
    // For joey's pivot the old devramfs root is held by kproc as well
    // (system-wide; ARCH-invariant for the v1.0 boot path), so this
    // clunk drops joey's per-Territory ref but does NOT free the
    // underlying tree structurally.
    spoor_clunk(old);
    return 0;
}
