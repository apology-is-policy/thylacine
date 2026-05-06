// Namespace primitives — Plan 9 bind/unmount + Pgrp lifecycle (P2-Eb).
//
// Per ARCHITECTURE.md §9.1 + specs/namespace.tla. Implements the spec's
// Bind / Unbind / ForkClone actions on a SLUB-allocated Pgrp struct
// holding a fixed-size bind-table.
//
// State invariant (NoCycle, ARCH §28 I-3): for every Pgrp `p` and every
// path `x`, `x` is not reachable from its own binding set via the
// transitive closure. Enforced by `would_create_cycle` (DFS over the
// existing binds) called from `bind()` BEFORE inserting; if a cycle
// would form, bind returns -1.
//
// At v1.0 P2-Eb the namespace has no real path resolution — `path_id_t`
// is `u32` and callers pick numeric IDs. When the 9P client lands at
// Phase 4, path_id_t becomes `struct Chan *` (or qid_t for the RB-tree
// key per ARCH §9.1 design intent), and `walk` traverses the bind graph
// from a starting Chan.
//
// Bootstrap order (kernel/main.c calls):
//   1. slub_init
//   2. pgrp_init        — pgrp SLUB cache + kproc's empty Pgrp
//   3. proc_init        — kproc; assigns kproc->pgrp = kpgrp()
//   4. thread_init      — kthread

#include <thylacine/extinction.h>
#include <thylacine/pgrp.h>
#include <thylacine/types.h>

#include "../mm/slub.h"

static struct kmem_cache *g_pgrp_cache;
static struct Pgrp       *g_kpgrp;
static u64                g_pgrp_created;
static u64                g_pgrp_destroyed;

// =============================================================================
// Pgrp lifecycle.
// =============================================================================

static void pgrp_init_fields(struct Pgrp *p) {
    p->magic  = PGRP_MAGIC;
    p->ref    = 1;
    p->nbinds = 0;
    // KP_ZERO already cleared binds[]; nothing more to do.
}

void pgrp_init(void) {
    if (g_pgrp_cache) extinction("pgrp_init called twice");

    g_pgrp_cache = kmem_cache_create("pgrp",
                                     sizeof(struct Pgrp),
                                     8,
                                     KMEM_CACHE_PANIC_ON_FAIL);
    if (!g_pgrp_cache) {
        extinction("kmem_cache_create(pgrp) returned NULL");
    }

    g_kpgrp = kmem_cache_alloc(g_pgrp_cache, KP_ZERO);
    if (!g_kpgrp) extinction("kmem_cache_alloc(kpgrp) failed");
    pgrp_init_fields(g_kpgrp);
    __atomic_fetch_add(&g_pgrp_created, 1u, __ATOMIC_RELAXED);
}

struct Pgrp *kpgrp(void) {
    return g_kpgrp;
}

struct Pgrp *pgrp_alloc(void) {
    if (!g_pgrp_cache) extinction("pgrp_alloc before pgrp_init");
    struct Pgrp *p = kmem_cache_alloc(g_pgrp_cache, KP_ZERO);
    if (!p) return NULL;
    pgrp_init_fields(p);
    __atomic_fetch_add(&g_pgrp_created, 1u, __ATOMIC_RELAXED);
    return p;
}

struct Pgrp *pgrp_clone(struct Pgrp *parent) {
    if (!parent)                  extinction("pgrp_clone(NULL)");
    if (parent->magic != PGRP_MAGIC)
        extinction("pgrp_clone of corrupted Pgrp");
    // R5-H F92: defense-in-depth against single-bit-flip / partial-
    // corruption that leaves magic intact but corrupts nbinds. The bound
    // is fixed at PGRP_MAX_BINDS; validating here catches a torn nbinds
    // before the read past binds[]. Same defense lands in bind() /
    // unmount() implicitly via PGRP_MAX_BINDS bounds checks at each
    // operation; pgrp_clone is the only single-pass-loop-on-trusted-
    // length path, so it gets an explicit check.
    if ((unsigned)parent->nbinds > PGRP_MAX_BINDS)
        extinction("pgrp_clone: parent->nbinds out of range (corrupted Pgrp)");

    struct Pgrp *child = pgrp_alloc();
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

void pgrp_ref(struct Pgrp *p) {
    if (!p) return;
    if (p->magic != PGRP_MAGIC)   extinction("pgrp_ref of corrupted Pgrp");
    // R5-H F80: atomic refcount. v1.0 P2-Eb has each Proc owning a
    // private Pgrp (refcount = 1; never shared) so contention is zero;
    // RFNAMEG (Phase 5+) introduces sharing across Procs running on
    // different CPUs, at which point this primitive must be atomic.
    // Landing the discipline now closes the latent hazard before the
    // first sharing test would expose it.
    __atomic_fetch_add(&p->ref, 1, __ATOMIC_RELAXED);
}

void pgrp_unref(struct Pgrp *p) {
    if (!p) return;
    if (p->magic != PGRP_MAGIC)   extinction("pgrp_unref of corrupted Pgrp");
    if (p == g_kpgrp)             extinction("pgrp_unref attempted on kpgrp");
    // R5-H F80: atomic decrement-and-check. Single fetch_sub returns the
    // pre-decrement value; if it was 1, the decrement reached 0 and we
    // own the free. Acquire ordering on the zero-transition load pairs
    // with the release store of the last reference's mutator (matches
    // the std::shared_ptr discipline). pre <= 0 indicates underflow —
    // an unref-of-zero, distinct from the previous "post-decrement is
    // negative" check (which was racy under SMP).
    int pre = __atomic_fetch_sub(&p->ref, 1, __ATOMIC_ACQ_REL);
    if (pre <= 0)                 extinction("pgrp_unref of zero-ref Pgrp");
    if (pre == 1) {
        kmem_cache_free(g_pgrp_cache, p);
        __atomic_fetch_add(&g_pgrp_destroyed, 1u, __ATOMIC_RELAXED);
    }
}

int pgrp_nbinds(struct Pgrp *p) {
    if (!p) return 0;
    return p->nbinds;
}

u64 pgrp_total_created(void)   { return __atomic_load_n(&g_pgrp_created, __ATOMIC_RELAXED); }
u64 pgrp_total_destroyed(void) { return __atomic_load_n(&g_pgrp_destroyed, __ATOMIC_RELAXED); }

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

// would_create_cycle(pgrp, src, dst): would adding the edge `dst -> src`
// produce a cycle in pgrp's bind graph?
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
// O(N²) worst case (N = pgrp->nbinds); for v1.0's PGRP_MAX_BINDS=32
// that's 32×32 = 1024 inner iterations, well under any latency budget.
static bool would_create_cycle(const struct Pgrp *pgrp,
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
        for (int i = 0; i < pgrp->nbinds; i++) {
            path_id_t e_src = pgrp->binds[i].src;
            path_id_t e_dst = pgrp->binds[i].dst;
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

int bind(struct Pgrp *pgrp, path_id_t src, path_id_t dst) {
    if (!pgrp)                    extinction("bind(NULL pgrp)");
    if (pgrp->magic != PGRP_MAGIC)
        extinction("bind on corrupted Pgrp");

    if (src == dst)                       return -4;
    if (would_create_cycle(pgrp, src, dst)) return -1;

    // Idempotency: same (src, dst) edge already exists.
    for (int i = 0; i < pgrp->nbinds; i++) {
        if (pgrp->binds[i].src == src && pgrp->binds[i].dst == dst) {
            return -2;
        }
    }

    if (pgrp->nbinds >= PGRP_MAX_BINDS)   return -3;

    pgrp->binds[pgrp->nbinds].src = src;
    pgrp->binds[pgrp->nbinds].dst = dst;
    pgrp->nbinds++;
    return 0;
}

int unmount(struct Pgrp *pgrp, path_id_t src, path_id_t dst) {
    if (!pgrp)                    extinction("unmount(NULL pgrp)");
    if (pgrp->magic != PGRP_MAGIC)
        extinction("unmount on corrupted Pgrp");

    for (int i = 0; i < pgrp->nbinds; i++) {
        if (pgrp->binds[i].src == src && pgrp->binds[i].dst == dst) {
            // Remove by swapping with the last element.
            pgrp->binds[i] = pgrp->binds[pgrp->nbinds - 1];
            pgrp->nbinds--;
            return 0;
        }
    }
    return -1;
}
