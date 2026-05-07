// Territory primitives — Plan 9 bind/unmount + Territory lifecycle (P2-Eb).
//
// Per ARCHITECTURE.md §9.1 + specs/territory.tla. Implements the spec's
// Bind / Unbind / ForkClone actions on a SLUB-allocated Territory struct
// holding a fixed-size bind-table.
//
// State invariant (NoCycle, ARCH §28 I-3): for every Territory `p` and every
// path `x`, `x` is not reachable from its own binding set via the
// transitive closure. Enforced by `would_create_cycle` (DFS over the
// existing binds) called from `bind()` BEFORE inserting; if a cycle
// would form, bind returns -1.
//
// At v1.0 P2-Eb the territory has no real path resolution — `path_id_t`
// is `u32` and callers pick numeric IDs. When the 9P client lands at
// Phase 4, path_id_t becomes `struct Spoor *` (or qid_t for the RB-tree
// key per ARCH §9.1 design intent), and `walk` traverses the bind graph
// from a starting Spoor.
//
// Bootstrap order (kernel/main.c calls):
//   1. slub_init
//   2. territory_init        — territory SLUB cache + kproc's empty Territory
//   3. proc_init        — kproc; assigns kproc->territory = kpgrp()
//   4. thread_init      — kthread

#include <thylacine/extinction.h>
#include <thylacine/territory.h>
#include <thylacine/types.h>

#include "../mm/slub.h"

static struct kmem_cache *g_pgrp_cache;
static struct Territory       *g_kpgrp;
static u64                g_pgrp_created;
static u64                g_pgrp_destroyed;

// =============================================================================
// Territory lifecycle.
// =============================================================================

static void territory_init_fields(struct Territory *p) {
    p->magic  = PGRP_MAGIC;
    p->ref    = 1;
    p->nbinds = 0;
    // KP_ZERO already cleared binds[]; nothing more to do.
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
    // corruption that leaves magic intact but corrupts nbinds. The bound
    // is fixed at PGRP_MAX_BINDS; validating here catches a torn nbinds
    // before the read past binds[]. Same defense lands in bind() /
    // unmount() implicitly via PGRP_MAX_BINDS bounds checks at each
    // operation; territory_clone is the only single-pass-loop-on-trusted-
    // length path, so it gets an explicit check.
    if ((unsigned)parent->nbinds > PGRP_MAX_BINDS)
        extinction("territory_clone: parent->nbinds out of range (corrupted Territory)");

    struct Territory *child = territory_alloc();
    if (!child) return NULL;

    // Deep-copy the bind table. Models the spec's ForkClone action: the
    // child gets an independent function value (in C terms: a separate
    // copy of the binds[] array; subsequent modifications to either are
    // independent because the arrays don't alias).
    child->nbinds = parent->nbinds;
    for (int i = 0; i < parent->nbinds; i++) {
        child->binds[i] = parent->binds[i];
    }
    return child;
}

void territory_ref(struct Territory *p) {
    if (!p) return;
    if (p->magic != PGRP_MAGIC)   extinction("territory_ref of corrupted Territory");
    // R5-H F80: atomic refcount. v1.0 P2-Eb has each Proc owning a
    // private Territory (refcount = 1; never shared) so contention is zero;
    // RFNAMEG (Phase 5+) introduces sharing across Procs running on
    // different CPUs, at which point this primitive must be atomic.
    // Landing the discipline now closes the latent hazard before the
    // first sharing test would expose it.
    __atomic_fetch_add(&p->ref, 1, __ATOMIC_RELAXED);
}

void territory_unref(struct Territory *p) {
    if (!p) return;
    if (p->magic != PGRP_MAGIC)   extinction("territory_unref of corrupted Territory");
    if (p == g_kpgrp)             extinction("territory_unref attempted on kpgrp");
    // R5-H F80: atomic decrement-and-check. Single fetch_sub returns the
    // pre-decrement value; if it was 1, the decrement reached 0 and we
    // own the free. Acquire ordering on the zero-transition load pairs
    // with the release store of the last reference's mutator (matches
    // the std::shared_ptr discipline). pre <= 0 indicates underflow —
    // an unref-of-zero, distinct from the previous "post-decrement is
    // negative" check (which was racy under SMP).
    int pre = __atomic_fetch_sub(&p->ref, 1, __ATOMIC_ACQ_REL);
    if (pre <= 0)                 extinction("territory_unref of zero-ref Territory");
    if (pre == 1) {
        kmem_cache_free(g_pgrp_cache, p);
        __atomic_fetch_add(&g_pgrp_destroyed, 1u, __ATOMIC_RELAXED);
    }
}

int territory_nbinds(struct Territory *p) {
    if (!p) return 0;
    return p->nbinds;
}

u64 territory_total_created(void)   { return __atomic_load_n(&g_pgrp_created, __ATOMIC_RELAXED); }
u64 territory_total_destroyed(void) { return __atomic_load_n(&g_pgrp_destroyed, __ATOMIC_RELAXED); }

// =============================================================================
// bind / unmount.
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
// Maps directly to the spec's WouldCreateCycle:
//   WouldCreateCycle(p, src, dst) ==
//     \/ src = dst
//     \/ dst \in Reachable(p, {src})
//
// O(N²) worst case (N = territory->nbinds); for v1.0's PGRP_MAX_BINDS=32
// that's 32×32 = 1024 inner iterations, well under any latency budget.
static bool would_create_cycle(const struct Territory *territory,
                               path_id_t src, path_id_t dst) {
    if (src == dst) return true;

    // reachable[] holds path IDs reachable from `src` via 0+ existing
    // edges. Sized PGRP_MAX_BINDS+1 — at most one entry per existing
    // edge can be added, plus the initial `src`.
    path_id_t reachable[PGRP_MAX_BINDS + 1];
    int n = 0;
    reachable[n++] = src;

    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < territory->nbinds; i++) {
            path_id_t e_src = territory->binds[i].src;
            path_id_t e_dst = territory->binds[i].dst;
            // Edge (e_src, e_dst) means: walking e_dst yields e_src.
            // For reachability via walking, if e_dst is reachable, so is
            // e_src.
            if (path_in(reachable, n, e_dst) &&
                !path_in(reachable, n, e_src)) {
                if (n >= PGRP_MAX_BINDS + 1) break;   // safety bound
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

    // Idempotency: same (src, dst) edge already exists.
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

int unmount(struct Territory *territory, path_id_t src, path_id_t dst) {
    if (!territory)                    extinction("unmount(NULL territory)");
    if (territory->magic != PGRP_MAGIC)
        extinction("unmount on corrupted Territory");

    for (int i = 0; i < territory->nbinds; i++) {
        if (territory->binds[i].src == src && territory->binds[i].dst == dst) {
            // Remove by swapping with the last element.
            territory->binds[i] = territory->binds[territory->nbinds - 1];
            territory->nbinds--;
            return 0;
        }
    }
    return -1;
}
