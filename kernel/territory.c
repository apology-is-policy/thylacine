// Territory primitives — Plan 9 bind/unbind/mount/unmount + Territory
// lifecycle (P2-Eb + P5-attach-mount).
//
// Per ARCHITECTURE.md §9.1 + §9.6 + specs/territory.tla. Implements the
// spec's Bind / Unbind / Mount / Unmount / ForkClone actions on a
// SLUB-allocated Territory struct holding a fixed-size bind table and a
// mount table of per-mount nodes (below).
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
//     - unmount(): spoor_clunk(source) after remove (outside the lock).
//     - territory_clone(): spoor_ref(source) per cloned entry.
//     - territory_unref() final STRONG release: spoor_clunk(source) for
//       each entry, detached under ns_lock; the struct outlives it while a
//       weak ref (devproc's ns read) holds it.
//   Always spoor_clunk, never spoor_unref: the entry's ref may be the LAST
//   (the Proc closed its fd), and the source Dev's close hook must run.
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
// only allocates EMPTY Territories (no mount entry); the territory_unref
// final-release path only clunks an entry's source when there is one, which
// requires mount() to have been called first, which requires a Spoor,
// which requires spoor_init. The dependency is therefore satisfied
// automatically by call ordering, not by init ordering.

#include <thylacine/dev.h>
#include <thylacine/dev9p.h>
#include <thylacine/extinction.h>
#include <thylacine/path.h>
#include <thylacine/spoor.h>
#include <thylacine/territory.h>
#include <thylacine/types.h>

#include "../mm/slub.h"

static struct kmem_cache *g_pgrp_cache;
static struct kmem_cache *g_mount_cache;    // struct PgrpMount
static struct kmem_cache *g_mhead_cache;    // struct PgrpMhead
static struct Territory  *g_kpgrp;
static u64                g_pgrp_created;
static u64                g_pgrp_destroyed;

// =============================================================================
// The mount table's nodes (ARCH 9.1; dec-2026-10-19-mount-table-dynamic).
// =============================================================================
//
// Plan 9's shape: one PgrpMount per mount, under one PgrpMhead per mount point
// (Plan 9's Mount and Mhead). A point is found by hashing its key into
// Territory.mhash[]; it holds its members in search order, and it is in the
// hash only while it hosts at least one. Every entry is also on the table-order
// list, mfirst .. mlast: the order the mounts were placed in, which is what
// /proc/<pid>/ns prints and territory_clone copies. MBEFORE places an entry
// just before its point's first member and every other placement appends it,
// so a point's members appear in the table in their search order -- the order
// the fixed array kept, which the lists reproduce exactly.
//
// Nothing is allocated or freed under ns_lock: mount() and territory_clone
// allocate before taking it, and what an edit removes under it is chained on a
// struct mount_dead and released after it is dropped, the source clunk
// included (a Dev close hook may sleep).

// A mount entry: the Spoor `source` grafted at the point `head`. stalk, after
// resolving a component to Spoor S, crosses to a clone of `source` iff S's
// (dc, devno, qid.path) is head's key (Plan 9 `domount`). The entry holds one
// ref on source, dropped when the entry is unmounted, replaced, shed, or
// released with its Territory.
//
// mp_path (#66, I-33): the mount POINT's namespace name (the Plan 9 Chan.path of
// the directory mounted onto -- "/srv", "/proc"), captured by path_ref'ing the
// resolved mountpoint Spoor's ->path at mount() and shared, one ref each, by
// every clone of the entry. STRICTLY introspection-only (I-33): the table keys
// on the point's identity, NEVER on mp_path -- a NULL or stale mp_path changes
// only what /proc/<pid>/ns displays, never which entry stalk crosses. It stays
// per entry because two mounts at one point may have named it differently.
// NULL when the mountpoint had no retained name (kernel-internal direct walks).
struct PgrpMount {
    struct PgrpMount *tnext, *tprev;   // table order
    struct PgrpMount *next, *prev;     // head's members, in search order
    struct PgrpMhead *head;
    struct Spoor     *source;
    struct Path      *mp_path;
    u32               flags;           // MREPL / MBEFORE / MAFTER / MCREATE /
                                       // MNOEXEC / MPHENO_LINUX, or MCOVERED alone
    u32               _pad;
};
_Static_assert(sizeof(struct PgrpMount) == 64,
               "PgrpMount is 7 pointers + flags + pad = 64 B, the figure "
               "PGRP_MOUNT_CAP's memory cost is stated in");

// A mount point: the directory identified by (dc, devno, qid_path), the Plan 9
// (type, dev, qid) identity (stalk-2). Why the full triple: qid.path is unique
// only WITHIN a (dc, devno) instance. Every dev9p session shares dc='9' and
// every attach root has qid.path 0, so (dc, qid.path) alone cannot distinguish
// two concurrent 9P sessions' mount points (the A-5b corvus + per-user-stratum-fs
// case). devno (Plan 9 Chan.dev, minted per attach by spoor_next_devno) closes
// that.
//
// `visit` and `work` belong to the passes that walk the points as a graph -- the
// cycle check and the shed -- and mean nothing between them.
struct PgrpMhead {
    struct PgrpMhead *hnext;           // the hash chain
    struct PgrpMount *first, *last;    // members, in search order; never empty
    struct PgrpMhead *work;            // a pass's worklist link
    u64               devno;
    u64               qid_path;
    int               dc;
    u32               nmembers;
    u32               visit;           // == Territory.mvisit: reached in this pass
    u32               _pad;
};
_Static_assert(sizeof(struct PgrpMhead) == 64,
               "PgrpMhead is 4 pointers + the 20 B key + 3 u32 = 64 B, the "
               "figure PGRP_MOUNT_CAP's memory cost is stated in");
_Static_assert((PGRP_MOUNT_HASH & (PGRP_MOUNT_HASH - 1)) == 0,
               "PGRP_MOUNT_HASH must be a power of two");

// Tests: mount-node allocations still to fail (territory_mount_fail_nodes_for_test).
static u32 g_mount_fail_nodes;

// Every mount node comes from here, so a test can fail one as the slab would.
static void *mount_node_alloc(struct kmem_cache *c) {
    u32 n = __atomic_load_n(&g_mount_fail_nodes, __ATOMIC_RELAXED);
    while (n > 0) {
        if (__atomic_compare_exchange_n(&g_mount_fail_nodes, &n, n - 1, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return NULL;
    }
    return kmem_cache_alloc(c, KP_ZERO);
}

u32 territory_mount_fail_nodes_for_test(u32 n) {
    return __atomic_exchange_n(&g_mount_fail_nodes, n, __ATOMIC_RELAXED);
}

static u32 mhash_of(int dc, u64 devno, u64 qid_path) {
    u64 h = (qid_path ^ ((u64)(u32)dc << 40)) + devno * 0x9E3779B97F4A7C15ull;
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 29;
    return (u32)h & (PGRP_MOUNT_HASH - 1);
}

// The point with this key, or NULL when nothing is mounted there. Caller holds
// ns_lock.
static struct PgrpMhead *mhead_find(const struct Territory *t, int dc,
                                    u64 devno, u64 qid_path) {
    for (struct PgrpMhead *h = t->mhash[mhash_of(dc, devno, qid_path)]; h;
         h = h->hnext)
        if (h->dc == dc && h->devno == devno && h->qid_path == qid_path) return h;
    return NULL;
}

static struct PgrpMhead *mhead_of(const struct Territory *t,
                                  const struct Spoor *s) {
    return mhead_find(t, s->dc, s->devno, s->qid.path);
}

// Hash a new point with no member yet; the caller links one before it drops
// ns_lock.
static void mhead_insert(struct Territory *t, struct PgrpMhead *h,
                         int dc, u64 devno, u64 qid_path) {
    h->dc       = dc;
    h->devno    = devno;
    h->qid_path = qid_path;
    h->first    = NULL;
    h->last     = NULL;
    h->work     = NULL;
    h->nmembers = 0;
    h->visit    = 0;
    u32 b = mhash_of(dc, devno, qid_path);
    h->hnext    = t->mhash[b];
    t->mhash[b] = h;
    t->npoints++;
}

// Take a point that no longer hosts a member out of the hash. Caller holds
// ns_lock.
static void mhead_remove(struct Territory *t, struct PgrpMhead *h) {
    struct PgrpMhead **pp = &t->mhash[mhash_of(h->dc, h->devno, h->qid_path)];
    while (*pp != h) {
        if (!*pp) extinction("territory: a mount point is missing from its hash chain");
        pp = &(*pp)->hnext;
    }
    *pp      = h->hnext;
    h->hnext = NULL;
    t->npoints--;
}

// Link `m` as a member of `h`. With `first` set at a point that has members
// (MBEFORE), it goes first in h's search order and just before h's old first
// member in the table; otherwise -- every other placement, and any placement
// at a point with no member -- last in both. Caller holds ns_lock.
static void mount_link(struct Territory *t, struct PgrpMhead *h,
                       struct PgrpMount *m, bool first) {
    struct PgrpMount *at = first ? h->first : NULL;
    m->head = h;
    if (at) {
        m->tprev = at->tprev;
        m->tnext = at;
        if (at->tprev) at->tprev->tnext = m;
        else           t->mfirst = m;
        at->tprev = m;
        m->prev   = NULL;
        m->next   = at;
        at->prev  = m;
        h->first  = m;
    } else {
        m->tprev = t->mlast;
        m->tnext = NULL;
        if (t->mlast) t->mlast->tnext = m;
        else          t->mfirst = m;
        t->mlast = m;
        m->prev  = h->last;
        m->next  = NULL;
        if (h->last) h->last->next = m;
        else         h->first = m;
        h->last  = m;
    }
    h->nmembers++;
    t->nmounts++;
}

// Unlink `m` from the table and from its point. The point stays hashed even
// when this empties it: the caller removes it, or links a member there again,
// before it drops ns_lock.
static void mount_unlink(struct Territory *t, struct PgrpMount *m) {
    struct PgrpMhead *h = m->head;
    if (m->tprev) m->tprev->tnext = m->tnext;
    else          t->mfirst = m->tnext;
    if (m->tnext) m->tnext->tprev = m->tprev;
    else          t->mlast = m->tprev;
    if (m->prev)  m->prev->next = m->next;
    else          h->first = m->next;
    if (m->next)  m->next->prev = m->prev;
    else          h->last = m->prev;
    m->tnext = m->tprev = m->next = m->prev = NULL;
    m->head  = NULL;
    h->nmembers--;
    t->nmounts--;
}

// Open a pass over the points: a point is reached in it once its `visit` mark
// equals the value returned. A point's mark is 0 until a pass reaches it, and a
// wrap clears every mark, so no point starts a pass looking reached. Caller
// holds ns_lock.
static u32 mvisit_next(struct Territory *t) {
    if (++t->mvisit == 0) {
        for (int b = 0; b < PGRP_MOUNT_HASH; b++)
            for (struct PgrpMhead *h = t->mhash[b]; h; h = h->hnext) h->visit = 0;
        t->mvisit = 1;
    }
    return t->mvisit;
}

// What a table edit unlinked under ns_lock, for mount_dead_release once the
// lock is dropped: entries in the order they were removed, and the points they
// left empty.
struct mount_dead {
    struct PgrpMount  *entries;
    struct PgrpMount **tail;
    struct PgrpMhead  *points;
};

static void mount_dead_init(struct mount_dead *d) {
    d->entries = NULL;
    d->tail    = &d->entries;
    d->points  = NULL;
}

static void mount_dead_entry(struct mount_dead *d, struct PgrpMount *m) {
    m->tnext = NULL;
    *d->tail = m;
    d->tail  = &m->tnext;
}

static void mount_dead_point(struct mount_dead *d, struct PgrpMhead *h) {
    h->hnext  = d->points;
    d->points = h;
}

// Release each entry as unmount() always has -- its mount-point name, then its
// source by spoor_clunk, never spoor_unref: the entry's ref may be the source's
// LAST, and the Dev's close hook must run -- then free the nodes. Never under
// ns_lock.
static void mount_dead_release(struct mount_dead *d) {
    struct PgrpMount *m = d->entries;
    while (m) {
        struct PgrpMount *next = m->tnext;
        path_unref(m->mp_path);
        spoor_clunk(m->source);
        kmem_cache_free(g_mount_cache, m);
        m = next;
    }
    struct PgrpMhead *h = d->points;
    while (h) {
        struct PgrpMhead *next = h->hnext;
        kmem_cache_free(g_mhead_cache, h);
        h = next;
    }
    mount_dead_init(d);
}

// Free nodes allocated for a copy and never linked (territory_clone's pools,
// chained through tnext and hnext).
static void mount_pools_free(struct PgrpMount *entries, struct PgrpMhead *points) {
    while (entries) {
        struct PgrpMount *next = entries->tnext;
        kmem_cache_free(g_mount_cache, entries);
        entries = next;
    }
    while (points) {
        struct PgrpMhead *next = points->hnext;
        kmem_cache_free(g_mhead_cache, points);
        points = next;
    }
}

// =============================================================================
// Territory lifecycle.
// =============================================================================

static void territory_init_fields(struct Territory *p) {
    p->magic      = PGRP_MAGIC;
    p->ref        = 1;
    p->weak       = 1;      // the strong refs' own count
    p->nbinds     = 0;
    p->nmounts    = 0;
    p->npoints    = 0;
    p->mfirst     = NULL;
    p->mlast      = NULL;
    p->mvisit     = 0;
    p->root_spoor = NULL;
    spin_lock_init(&p->dot_lock);
    spin_lock_init(&p->ns_lock);   // RW-4 SA-F1: the mount table/binds[]/root_spoor guard
    p->dot_path   = NULL;   // NULL == cwd "/" (allocated lazily on first chdir)
    // KP_ZERO already cleared binds[] and mhash[]; nothing more to do.
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
    // No KMEM_CACHE_PANIC_ON_FAIL: a mount whose entry cannot be allocated is
    // refused (ENOMEM), and a spawn whose copy cannot be is too.
    g_mount_cache = kmem_cache_create("territory_mount", sizeof(struct PgrpMount), 8, 0);
    g_mhead_cache = kmem_cache_create("territory_mhead", sizeof(struct PgrpMhead), 8, 0);
    if (!g_mount_cache || !g_mhead_cache) {
        extinction("kmem_cache_create(territory mount nodes) returned NULL");
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

// Design D (VIVARIUM 13.10.3): the namespace-level phenotype declaration.
bool territory_root_pheno(const struct Territory *territory) {
    if (!territory) return false;
    return (__atomic_load_n(&territory->flags, __ATOMIC_ACQUIRE)
            & TERRITORY_ROOT_PHENO_LINUX) != 0;
}

void territory_declare_linux(struct Territory *territory) {
    if (!territory)                    extinction("territory_declare_linux(NULL)");
    if (territory->magic != PGRP_MAGIC) extinction("territory_declare_linux on corrupted Territory");
    __atomic_fetch_or(&territory->flags, TERRITORY_ROOT_PHENO_LINUX, __ATOMIC_RELEASE);
}

struct Territory *territory_clone(struct Territory *parent) {
    if (!parent)                  extinction("territory_clone(NULL)");
    if (parent->magic != PGRP_MAGIC)
        extinction("territory_clone of corrupted Territory");
    // R5-H F92: defense-in-depth against single-bit-flip / partial-
    // corruption that leaves magic intact but corrupts the count
    // fields. The bound is fixed at compile time; validating here
    // catches a torn nbinds before the deep-copy loop reads past the
    // array (the mount counts are checked where they are read, below).
    if ((unsigned)parent->nbinds > PGRP_MAX_BINDS)
        extinction("territory_clone: parent->nbinds out of range (corrupted Territory)");

    struct Territory *child = territory_alloc();
    if (!child) return NULL;

    // The child's mount nodes are allocated before parent->ns_lock is taken
    // (ARCH 9.1), sized from the counts read under it. A peer thread that grew
    // the table in the gap sends the loop round again for the difference, and
    // the rounds end: the pools only grow, and the table only to
    // PGRP_MOUNT_CAP.
    //
    // RW-4 SA-F1: the bind/mount/root deep-copy reads parent's tables -- take
    // parent->ns_lock so a concurrent mount/unmount/pivot on a peer thread (or an
    // rfork(RFNAMEG)-sharing Proc) cannot tear the copy or free a source mid-ref.
    // spoor_ref under the lock is safe (atomic, non-sleeping). Released after the
    // root copy, before the dot_path copy (which takes the separate dot_lock).
    struct PgrpMount *epool = NULL;    // chained through tnext
    struct PgrpMhead *ppool = NULL;    // chained through hnext
    int nepool = 0, nppool = 0;
    for (;;) {
        spin_lock(&parent->ns_lock);
        int ne = parent->nmounts, np = parent->npoints;
        // R5-H F92: a torn count would size the pools from garbage.
        if ((unsigned)ne > PGRP_MOUNT_CAP || (unsigned)np > (unsigned)ne)
            extinction("territory_clone: parent's mount counts out of range (corrupted Territory)");
        if (ne <= nepool && np <= nppool) break;          // ns_lock stays held
        spin_unlock(&parent->ns_lock);
        for (; nepool < ne; nepool++) {
            struct PgrpMount *m = mount_node_alloc(g_mount_cache);
            if (!m) goto oom;
            m->tnext = epool;
            epool    = m;
        }
        for (; nppool < np; nppool++) {
            struct PgrpMhead *h = mount_node_alloc(g_mhead_cache);
            if (!h) goto oom;
            h->hnext = ppool;
            ppool    = h;
        }
    }

    // Deep-copy the bind table. Models the spec's ForkClone action's
    // bindings update: the child gets an independent function value.
    child->nbinds = parent->nbinds;
    for (int i = 0; i < parent->nbinds; i++) {
        child->binds[i] = parent->binds[i];
    }

    // Deep-copy the mount table in table order. Appending each entry to its
    // point carries every point's search order over too, since a point's
    // members appear in the table in that order. For each cloned entry,
    // spoor_ref(source) — each Territory holding an entry contributes one
    // reference per entry. Models the spec's ForkClone refcount update:
    //   refcount' = refcount + Cardinality(entries-in-parent-pointing-at-s)
    //
    // spoor_ref extincts on a NULL or corrupted source — that's a kernel
    // invariant violation (we put a corrupted Spoor into the mount table)
    // and the extinct is the correct response.
    for (const struct PgrpMount *pm = parent->mfirst; pm; pm = pm->tnext) {
        const struct PgrpMhead *ph = pm->head;
        struct PgrpMhead *ch = mhead_find(child, ph->dc, ph->devno, ph->qid_path);
        if (!ch) {
            if (!ppool) extinction("territory_clone: more mount points than npoints (corrupted Territory)");
            ch    = ppool;
            ppool = ch->hnext;
            mhead_insert(child, ch, ph->dc, ph->devno, ph->qid_path);
        }
        if (!epool) extinction("territory_clone: more mount entries than nmounts (corrupted Territory)");
        struct PgrpMount *cm = epool;
        epool       = cm->tnext;
        cm->source  = pm->source;
        spoor_ref(cm->source);
        // #66 (I-33): the cloned entry SHARES the parent's mount-point name --
        // one more Path ref per clone, dropped at this child's unmount /
        // final-release. path_ref is NULL-safe (a nameless mount stays nameless).
        cm->mp_path = pm->mp_path;
        path_ref(cm->mp_path);
        cm->flags   = pm->flags;
        mount_link(child, ch, cm, false);
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
    // Design D (VIVARIUM 13.10.3; review F2 = self-audit SA-1): the namespace-
    // level phenotype declaration is a namespace property -- copied WITH
    // root_spoor, under the same lock. This clone copies fields one by one, so
    // an omitted copy would let a container child LOOK Linux (the rfork
    // phenotype inherit at proc.c) and then revert to native on its first
    // execve, silently, for the whole descendant subtree.
    child->flags = __atomic_load_n(&parent->flags, __ATOMIC_ACQUIRE);
    spin_unlock(&parent->ns_lock);
    mount_pools_free(epool, ppool);      // what a shrinking peer left over

    // LS-4: copy the cwd snapshot (POSIX fork semantics -- the child inherits
    // the parent's cwd, independent thereafter). Take the PARENT's dot_lock so
    // a concurrent chdir on a parent thread cannot free dot_path under us; a
    // race is benign (either pre- or post-chdir cwd is a valid snapshot).
    // kmalloc UNDER the leaf lock is OK -- SLUB alloc is bounded + non-sleeping.
    // On OOM, territory_unref the child (drops the mount/root refs just taken)
    // and fail; the parent is unchanged.
    spin_lock(&parent->dot_lock);
    if (parent->dot_path) {
        u64 len = 0;
        while (parent->dot_path[len] != '\0') len++;
        char *dup = kmalloc(len + 1, 0);
        if (!dup) {
            spin_unlock(&parent->dot_lock);
            territory_unref(child);
            return NULL;
        }
        for (u64 i = 0; i <= len; i++) dup[i] = parent->dot_path[i];
        child->dot_path = dup;
    }
    spin_unlock(&parent->dot_lock);
    return child;

oom:
    // No lock is held, and the child has no entry yet.
    mount_pools_free(epool, ppool);
    territory_unref(child);
    return NULL;
}

void territory_ref(struct Territory *p) {
    if (!p) return;
    if (p->magic != PGRP_MAGIC)   extinction("territory_ref of corrupted Territory");
    int pre = __atomic_fetch_add(&p->ref, 1, __ATOMIC_RELAXED);
    if (pre <= 0)                 extinction("territory_ref of a released Territory");
}

// The last strong ref's release of the contents. Models the spec's
// "DestroyTerritory requires mounts[p] = {}" precondition: the impl satisfies
// it by spoor_clunk'ing each entry's source here, which is equivalent to a
// sequence of Unmount actions immediately preceding the destroy.
//
// If the entry loop is skipped (the BUGGY_DESTROY_LEAK class), each source's
// refcount stays bumped while the entries vanish -- the Spoor's storage is
// leaked. The spec's MountRefcountConsistency invariant catches that desync.
//
// Use spoor_clunk (Plan 9 cclose) not spoor_unref: if this is the Spoor's LAST
// holder (e.g., the user already closed any attach_9p fd; this Territory was
// the last mount-table entry for it), the Dev's close hook needs to run to
// release per-Spoor Dev state (pipe endpoints, 9P sessions, etc.).
// P5-mount-syscall closed this gap; pre-fix, spoor_unref's close-less last-drop
// would have leaked Dev state on Territory destruction.
//
// A weak holder may be rendering the table, so the table and root_spoor are
// detached under ns_lock -- the holder then reads an empty one -- and released
// outside it, where spoor_clunk may sleep. dot_path is swapped out under
// dot_lock. The entries are released from the last back, the order the
// array's release used; current Dev close hooks don't touch the Territory, so
// the direction is cosmetic.
static void territory_release_contents(struct Territory *p) {
    spin_lock(&p->ns_lock);
    struct PgrpMount *m = p->mlast;
    struct PgrpMhead *heads = NULL;
    for (int b = 0; b < PGRP_MOUNT_HASH; b++) {
        struct PgrpMhead *h = p->mhash[b];
        p->mhash[b] = NULL;
        while (h) {
            struct PgrpMhead *next = h->hnext;
            h->hnext = heads;
            heads = h;
            h = next;
        }
    }
    struct Spoor *root = p->root_spoor;
    p->mfirst     = NULL;
    p->mlast      = NULL;
    p->nmounts    = 0;
    p->npoints    = 0;
    p->root_spoor = NULL;
    spin_unlock(&p->ns_lock);

    spin_lock(&p->dot_lock);
    char *dot   = p->dot_path;
    p->dot_path = NULL;
    spin_unlock(&p->dot_lock);

    while (m) {
        struct PgrpMount *prev = m->tprev;
        // #66 (I-33): drop the entry's mount-point name ref (the matched
        // counterpart to the path_ref at mount()/clone). path_unref is
        // NULL-safe + non-sleeping (refcount + kfree, no Dev close hook).
        path_unref(m->mp_path);
        spoor_clunk(m->source);
        kmem_cache_free(g_mount_cache, m);
        m = prev;
    }
    while (heads) {
        struct PgrpMhead *next = heads->hnext;
        kmem_cache_free(g_mhead_cache, heads);
        heads = next;
    }
    // The root_spoor pivot's per-Territory ref, by spoor_clunk for the reason
    // the entries' are (P5-stratumd-stub-bringup-e2).
    if (root) spoor_clunk(root);
    // LS-4: the cwd string (NULL == "/").
    if (dot) kfree(dot);
}

void territory_unref(struct Territory *p) {
    if (!p) return;
    if (p->magic != PGRP_MAGIC)   extinction("territory_unref of corrupted Territory");
    if (p == g_kpgrp)             extinction("territory_unref attempted on kpgrp");
    int pre = __atomic_fetch_sub(&p->ref, 1, __ATOMIC_ACQ_REL);
    if (pre <= 0)                 extinction("territory_unref of zero-ref Territory");
    if (pre == 1) {
        territory_release_contents(p);
        territory_weak_unref(p);
    }
}

void territory_weak_ref(struct Territory *p) {
    if (!p) return;
    if (p->magic != PGRP_MAGIC)   extinction("territory_weak_ref of corrupted Territory");
    int pre = __atomic_fetch_add(&p->weak, 1, __ATOMIC_RELAXED);
    if (pre <= 0)                 extinction("territory_weak_ref of a freed Territory");
}

void territory_weak_unref(struct Territory *p) {
    if (!p) return;
    if (p->magic != PGRP_MAGIC)   extinction("territory_weak_unref of corrupted Territory");
    if (p == g_kpgrp)             extinction("territory_weak_unref attempted on kpgrp");
    int pre = __atomic_fetch_sub(&p->weak, 1, __ATOMIC_ACQ_REL);
    if (pre <= 0)                 extinction("territory_weak_unref of zero-ref Territory");
    if (pre == 1) {
        kmem_cache_free(g_pgrp_cache, p);
        __atomic_fetch_add(&g_pgrp_destroyed, 1u, __ATOMIC_RELAXED);
    }
}

// =============================================================================
// Per-Proc cwd ("dot") -- LS-4. See <thylacine/territory.h> + LIFE-SUPPORT.md
// LS-4 + STALK-DESIGN.md 4.3. Name-based: dot_path is an absolute path string
// with no ".", ".." or link component, NULL == "/" -- the name stalk reports
// for where a chdir LANDED (stalk_landed). cwd_join is the RESOLUTION join:
// verbatim, so "."/".."/a trailing separator reach stalk, which interprets
// them with the SAME gates an absolute path gets; I-28 containment is stalk's
// ".."-clamp at root_spoor, never this function's. Collapsing dots on the
// resolution path was #83: it popped components that were never walked, so a
// cwd-relative `nonexistent/..` opened successfully.
// =============================================================================

// cwd_join -- the RESOLUTION join (#83). Emits `dot` + '/' + `input` with the
// input copied VERBATIM: "." / ".." and any trailing separator survive into the
// path handed to stalk, which is the whole point. Resolving them here made a
// cwd-relative path bypass stalk's #79/#81/#82 gates, so `f/..`, `f/.`, `f/`
// and even `nonexistent/..` succeeded where the absolute spelling of the same
// path correctly answered ENOTDIR / ENOENT.
//
// I-28 is untouched: containment has never rested on this function. The joined
// path is resolved from root_spoor, and stalk clamps ".." at its trail floor
// (`start`) exactly as it already does for an absolute path containing ".." --
// the case ARCH's LS-4 row explicitly reasons about ("a hostile un-cleaned
// join cannot escape"). This makes cwd-relative resolution run the SAME code
// as absolute resolution rather than a parallel lexical one.
int cwd_join(const char *dot, const char *input, u64 inlen,
             char *out, u64 outcap) {
    if (!input || !out || outcap < 2) return -1;

    u64 olen = 0;
    out[0] = '\0';

    int absolute = (inlen > 0 && input[0] == '/');
    if (!absolute && dot && dot[0] == '/') {
        // The cwd is a clean absolute path by construction: territory_setdot
        // is handed either stalk_landed's name (SYS_CHDIR, a spawn's cwd
        // tail) or the boot's literal "/bin" (joey_root_kproc_at_devramfs),
        // and territory_clone copies that string.
        while (dot[olen] != '\0') {
            if (olen + 1 >= outcap) return -1;
            out[olen] = dot[olen];
            olen++;
        }
        // Root stores as "/", so drop the lone separator -- the one below is
        // the join's. (A "//" prefix would resolve identically, but the joined
        // string is also what a downstream reader sees.)
        if (olen == 1 && out[0] == '/') olen = 0;
    }

    if (!absolute && inlen > 0) {
        if (olen + 1 >= outcap) return -1;
        out[olen++] = '/';
    }

    for (u64 i = 0; i < inlen; i++) {
        if (olen + 1 >= outcap) return -1;
        out[olen++] = input[i];
    }

    // An empty input against the root cwd -- no production caller reaches this
    // (every syscall rejects path_len == 0 first), but the result must still be
    // a well-formed absolute path rather than "".
    if (olen == 0) out[olen++] = '/';
    out[olen] = '\0';
    return (int)olen;
}

int territory_join_cwd(struct Territory *p, const char *input, u64 inlen,
                       char *out, u64 outcap) {
    if (!p)                       return -1;
    if (p->magic != PGRP_MAGIC)   extinction("territory_join_cwd of corrupted Territory");
    // Read dot_path UNDER the leaf lock; the join is bounded CPU (no alloc, no
    // block), so holding dot_lock across it is safe.
    spin_lock(&p->dot_lock);
    int r = cwd_join(p->dot_path, input, inlen, out, outcap);
    spin_unlock(&p->dot_lock);
    return r;
}

int territory_getdot(struct Territory *p, char *buf, u64 cap) {
    if (!p || !buf || cap == 0)   return -1;
    if (p->magic != PGRP_MAGIC)   extinction("territory_getdot of corrupted Territory");
    spin_lock(&p->dot_lock);
    const char *src = (p->dot_path && p->dot_path[0]) ? p->dot_path : "/";
    u64 len = 0;
    while (src[len] != '\0') len++;
    if (len + 1 > cap) { spin_unlock(&p->dot_lock); return -1; }
    for (u64 i = 0; i < len; i++) buf[i] = src[i];
    buf[len] = '\0';
    spin_unlock(&p->dot_lock);
    return (int)len;
}

int territory_setdot(struct Territory *p, const char *cleaned) {
    if (!p || !cleaned)           return -1;
    if (p->magic != PGRP_MAGIC)   extinction("territory_setdot of corrupted Territory");

    // "/" is stored as the NULL sentinel -- no allocation. kmalloc the new copy
    // BEFORE taking dot_lock (the dup is private until installed).
    char *dup = NULL;
    if (!(cleaned[0] == '/' && cleaned[1] == '\0')) {
        u64 len = 0;
        while (cleaned[len] != '\0') len++;
        dup = kmalloc(len + 1, 0);
        if (!dup) return -1;
        for (u64 i = 0; i <= len; i++) dup[i] = cleaned[i];
    }

    spin_lock(&p->dot_lock);
    char *old = p->dot_path;
    p->dot_path = dup;
    spin_unlock(&p->dot_lock);

    // Free the old string OUTSIDE the lock. Safe: readers copy dot_path under
    // dot_lock and never retain the pointer past their critical section.
    if (old) kfree(old);
    return 0;
}

int territory_nbinds(struct Territory *p) {
    if (!p) return 0;
    return p->nbinds;
}

int territory_nmounts(struct Territory *p) {
    if (!p) return 0;
    return p->nmounts;
}

int territory_npoints(struct Territory *p) {
    if (!p) return 0;
    return p->npoints;
}

u64 territory_mount_entries_live(void) { return kmem_cache_live_count(g_mount_cache); }
u64 territory_mount_points_live(void)  { return kmem_cache_live_count(g_mhead_cache); }

// territory_mount_check -- the table's structural rules, each named by the
// number returned when it is the first found broken:
//   1  the table list is not a consistent doubly linked list, mfirst to mlast
//   2  nmounts is not the table's length, or the length passes PGRP_MOUNT_CAP
//   3  a point sits in the wrong hash chain, or a key is hashed twice
//   4  npoints is not the number of points hashed
//   5  a point's member list is inconsistent, empty, or not nmembers long
//   6  a member's head is not the point whose list holds it
//   7  the points' members do not add up to nmounts
//   8  a point's search order is not its members' order in the table
//   9  a covered entry carries another flag, is the second at its point, or
//      is not the point's own directory
// O(entries x points): for tests.
int territory_mount_check(struct Territory *t) {
    if (!t) return 0;
    if (t->magic != PGRP_MAGIC) extinction("territory_mount_check on corrupted Territory");

    int rule = 0;
    spin_lock(&t->ns_lock);
    int n = 0;
    const struct PgrpMount *prev = NULL;
    for (const struct PgrpMount *m = t->mfirst; m; m = m->tnext) {
        if (m->tprev != prev)             { rule = 1; goto out; }
        if (++n > PGRP_MOUNT_CAP)         { rule = 2; goto out; }
        prev = m;
    }
    if (t->mlast != prev)                 { rule = 1; goto out; }
    if (n != t->nmounts)                  { rule = 2; goto out; }

    int npts = 0, members = 0;
    for (int b = 0; b < PGRP_MOUNT_HASH; b++) {
        for (const struct PgrpMhead *h = t->mhash[b]; h; h = h->hnext) {
            if (mhash_of(h->dc, h->devno, h->qid_path) != (u32)b ||
                mhead_find(t, h->dc, h->devno, h->qid_path) != h) { rule = 3; goto out; }
            if (++npts > PGRP_MOUNT_CAP)  { rule = 4; goto out; }
            int k = 0, covered = 0;
            const struct PgrpMount *mp = NULL;
            for (const struct PgrpMount *m = h->first; m; m = m->next) {
                if (m->prev != mp)        { rule = 5; goto out; }
                if (m->head != h)         { rule = 6; goto out; }
                if ((m->flags & MCOVERED) &&
                    (m->flags != MCOVERED || ++covered > 1 || !m->source ||
                     m->source->dc != h->dc || m->source->devno != h->devno ||
                     m->source->qid.path != h->qid_path)) { rule = 9; goto out; }
                if (++k > PGRP_MOUNT_CAP) { rule = 5; goto out; }
                mp = m;
            }
            if (h->last != mp || k == 0 || (u32)k != h->nmembers) { rule = 5; goto out; }
            members += k;
        }
    }
    if (npts != t->npoints)               { rule = 4; goto out; }
    if (members != t->nmounts)            { rule = 7; goto out; }

    // With the counts agreeing, this also places every table entry on a
    // hashed point's list.
    for (int b = 0; b < PGRP_MOUNT_HASH; b++) {
        for (const struct PgrpMhead *h = t->mhash[b]; h; h = h->hnext) {
            const struct PgrpMount *want = h->first;
            for (const struct PgrpMount *m = t->mfirst; m; m = m->tnext) {
                if (m->head != h) continue;
                if (m != want)            { rule = 8; goto out; }
                want = want->next;
            }
            if (want)                     { rule = 8; goto out; }
        }
    }
out:
    spin_unlock(&t->ns_lock);
    return rule;
}

u64 territory_total_created(void)   { return __atomic_load_n(&g_pgrp_created, __ATOMIC_RELAXED); }
u64 territory_total_destroyed(void) { return __atomic_load_n(&g_pgrp_destroyed, __ATOMIC_RELAXED); }

// =============================================================================
// /proc/<pid>/ns rendering (#66). The listing is rendered from its start at every
// call, and a window of it is copied out: ns_emit copies whatever part of a run
// falls in the window and counts the rest, so a line before the window costs a
// few comparisons and no copy.
// =============================================================================

struct ns_window {
    char *buf;
    u64   off;     // the listing's first byte the window holds
    u64   cap;
    u64   pos;     // bytes of the listing rendered so far
    u64   eol;     // window bytes up to the last line end copied; 0 if none
};

static bool ns_window_full(const struct ns_window *w) {
    return w->pos >= w->off + w->cap;
}

static void ns_emit(struct ns_window *w, const char *s, u64 n) {
    u64 end = w->off + w->cap;
    u64 lo  = w->pos < w->off ? w->off - w->pos : 0;
    u64 hi  = w->pos >= end ? 0 : (end - w->pos < n ? end - w->pos : n);
    for (u64 i = lo; i < hi; i++) {
        u64 at = w->pos + i - w->off;
        w->buf[at] = s[i];
        if (s[i] == '\n') w->eol = at + 1;
    }
    w->pos += n;
}

static void ns_emit_str(struct ns_window *w, const char *s) {
    u64 n = 0;
    while (s[n] != '\0') n++;
    ns_emit(w, s, n);
}

static void ns_emit_udec(struct ns_window *w, u64 v) {
    char tmp[24], out[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    else while (v > 0 && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    ns_emit(w, out, (u64)n);
}

u64 territory_format_ns(struct Territory *p, u64 off, char *buf, u64 cap) {
    if (!p || !buf || cap == 0)   return 0;
    if (p->magic != PGRP_MAGIC)   extinction("territory_format_ns of corrupted Territory");

    struct ns_window w = { buf, off, cap, 0, 0 };
    // Read the mount table UNDER ns_lock: while held, each entry's source +
    // mp_path are ref-pinned (and source->path is ref-pinned by source + is an
    // immutable string), so every byte copied below is stable. ns_lock is a
    // near-leaf; the window writes take no further lock + cannot sleep, and the
    // render stops at the window's end, so the hold is bounded by the table, not
    // by the listing's length. The wait for ns_lock is not: this Territory's own
    // Proc can hold it for a whole shed, so a caller holding an IRQ-masking lock
    // would stall every CPU queued on that lock behind it (devproc renders after
    // dropping g_proc_table_lock).
    ASSERT_IRQS_ENABLED("territory_format_ns waits on an ns_lock its Proc may hold for a shed");
    spin_lock(&p->ns_lock);
    for (const struct PgrpMount *m = p->mfirst; m && !ns_window_full(&w); m = m->tnext) {
        ns_emit_str(&w, "mount ");

        // Mount-point name (the Plan 9 Chan.path of the dir mounted onto). NULL
        // mp_path -> "?" (a kernel-internal mountpoint with no retained name).
        if (m->mp_path && m->mp_path->len) ns_emit(&w, m->mp_path->s, m->mp_path->len);
        else                               ns_emit_str(&w, "?");
        ns_emit_str(&w, " ");

        // Source label. A 9P session's root: the file its session came over (the
        // operator's vote of 2026-09-28, Plan 9's form -- a mount line names the
        // channel, not the tree), by that file's name, else its device spec
        // ("#|", a pipe). Never on the covered entry, whose line names its own
        // directory. Otherwise the source Spoor's namespace name when it has one
        // (a mounted sub-tree); else "#<dc>" -- the Plan 9 device spec (a device
        // root has no namespace path; '#9'=9P, '#s'=srv, '#p'=proc, ...).
        struct Spoor *src = m->source;
        const struct Path *origin = NULL;
        char odc = 0;
        if (!(m->flags & MCOVERED) && dev9p_spoor_origin(src, &origin, &odc)) {
            if (origin) {
                ns_emit(&w, origin->s, origin->len);
            } else {
                char dev[3] = { '#', odc, '\0' };
                ns_emit_str(&w, dev);
            }
        } else if (src && src->path && src->path->len) {
            ns_emit(&w, src->path->s, src->path->len);
        } else {
            char dev[3];
            dev[0] = '#';
            dev[1] = src ? (char)src->dc : '?';
            dev[2] = '\0';
            ns_emit_str(&w, dev);
        }
        // #217: render MNOEXEC. A restriction that cannot be observed cannot be
        // debugged -- an operator asking "is this container's /env actually
        // noexec?" has no other way to find out, and "the code passes the flag"
        // is not an answer about a RUNNING namespace. Rendered as a suffix so
        // every existing parse of the line's first two fields is unchanged.
        if (m->flags & MNOEXEC) ns_emit_str(&w, " noexec");
        // VIVARIUM section 13: render MPHENO_LINUX for the same reason -- a
        // declaration that cannot be observed cannot be audited. An operator
        // asking "is /viv/bin actually a pheno-linux mount?" reads it here.
        if (m->flags & MPHENO_LINUX) ns_emit_str(&w, " pheno-linux");
        // The covered directory is a union member in its own right (its line
        // reads "mount <pt> <pt> covered"): shown, so an operator can see that
        // the point's own names are searched and in what order.
        if (m->flags & MCOVERED) ns_emit_str(&w, " covered");
        // LR-1 (HAUL-DESIGN 4.8): a member whose source belongs to a 9P session
        // declared remote at its attach or its /srv post. Never the covered
        // entry: nobody mounted it, and its line already names the directory.
        if (!(m->flags & MCOVERED) && dev9p_spoor_remote(m->source)) ns_emit_str(&w, " remote");
        ns_emit_str(&w, "\n");
    }
    // Bind count (binds[] are abstract path_id_t at v1.0 -- no string names to
    // render; the count keeps the file's pre-#66 contract). It comes after the
    // last mount line, so a reader that holds it has the whole list.
    int nb = p->nbinds;
    spin_unlock(&p->ns_lock);

    if (!ns_window_full(&w)) {
        ns_emit_str(&w, "binds: ");
        ns_emit_udec(&w, (u64)(nb < 0 ? 0 : nb));
        ns_emit_str(&w, "\n");
        // Design D audit F6: render the Territory-level phenotype declaration
        // (VIVARIUM 13.10.3) for the reason the per-mount " pheno-linux"
        // suffix above is rendered -- a declaration that cannot be observed
        // cannot be audited. After the binds line so every existing parse of
        // the file is unchanged. Read through the accessor (an atomic load):
        // the flag is set once, on the child's clone before its first EL0
        // instruction, and copied by clone, so it needs no ns_lock.
        if (territory_root_pheno(p)) ns_emit_str(&w, "root: pheno-linux\n");
    }

    // The window ends at its last line end (#66b audit F2: no partial line
    // where a whole one could be had), or, holding no line end, is returned
    // whole, so a read shorter than a line still advances.
    if (w.eol) return w.eol;
    if (w.pos <= off) return 0;
    return (w.pos - off < cap) ? w.pos - off : cap;
}

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

    if (src == dst)                       return -4;   // no table access; pre-lock

    // RW-4 SA-F1: the cycle check + dup scan READ binds[]/nbinds and the append
    // WRITES them -- serialize against a concurrent bind/unbind/clone under ns_lock.
    // No sleeping call inside (pure table RMW), so holding the lock is leaf-safe.
    spin_lock(&territory->ns_lock);
    if (would_create_cycle(territory, src, dst)) { spin_unlock(&territory->ns_lock); return -1; }

    for (int i = 0; i < territory->nbinds; i++) {
        if (territory->binds[i].src == src && territory->binds[i].dst == dst) {
            spin_unlock(&territory->ns_lock);
            return -2;
        }
    }

    if (territory->nbinds >= PGRP_MAX_BINDS) { spin_unlock(&territory->ns_lock); return -3; }

    territory->binds[territory->nbinds].src = src;
    territory->binds[territory->nbinds].dst = dst;
    territory->nbinds++;
    spin_unlock(&territory->ns_lock);
    return 0;
}

int unbind(struct Territory *territory, path_id_t src, path_id_t dst) {
    if (!territory)                    extinction("unbind(NULL territory)");
    if (territory->magic != PGRP_MAGIC)
        extinction("unbind on corrupted Territory");

    spin_lock(&territory->ns_lock);   // RW-4 SA-F1: serialize the binds[] RMW
    for (int i = 0; i < territory->nbinds; i++) {
        if (territory->binds[i].src == src && territory->binds[i].dst == dst) {
            territory->binds[i] = territory->binds[territory->nbinds - 1];
            territory->nbinds--;
            spin_unlock(&territory->ns_lock);
            return 0;
        }
    }
    spin_unlock(&territory->ns_lock);
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

// A mount-point / source identity (Plan 9 type+dev+qid). Used by the mount
// cycle check below.
struct mkey { int dc; u64 devno; u64 path; };
_Static_assert(sizeof(((struct mkey *)0)->devno) == sizeof(((struct Spoor *)0)->devno) &&
               sizeof(((struct PgrpMhead *)0)->devno) == sizeof(((struct Spoor *)0)->devno),
               "the mount key carries the whole devno (spoor_next_devno never wraps)");

static inline bool mkey_eq(struct mkey a, struct mkey b) {
    return a.dc == b.dc && a.devno == b.devno && a.path == b.path;
}
static inline struct mkey spoor_mkey(const struct Spoor *s) {
    struct mkey k = { s->dc, s->devno, s->qid.path };
    return k;
}

// would_create_mount_cycle(t, source, mountpoint): would adding the mount edge
// `key(mountpoint) -> key(source)` (walking onto the mount point crosses to the
// source's tree) create a cycle in `t`'s mount-identity graph? I-3 (mount points
// form a DAG, never a cycle) -- the mount-table analog of would_create_cycle for
// binds (stalk-2 audit F1: I-3 was claimed "by construction" but a self-mount or
// a two-tree oscillation could form a cycle that `cross_mounts` then resolved to
// a silently-wrong endpoint; enforce it here so the invariant actually holds).
//
// The keys reachable from `key(source)` are the least set holding it and, for
// each point whose key it holds, the key of every source mounted there; if
// `key(mountpoint)` is among them, the new edge closes a cycle (mountpoint ->
// source -> ... -> mountpoint). A pass over the points finds them: each point
// is looked up by key, marked when reached and queued on its own `work` link,
// so the pass costs the points it reaches and needs no array. The trivial
// self-mount (source identity == mountpoint identity) is the degenerate case. A
// covered entry (MCOVERED) is a self-edge -- its source IS its point -- so it
// leads back to a point already reached and needs no exemption. Caller holds
// ns_lock.
static bool would_create_mount_cycle(struct Territory *t,
                                     const struct Spoor *source,
                                     const struct Spoor *mountpoint) {
    struct mkey src = spoor_mkey(source);
    struct mkey dst = spoor_mkey(mountpoint);
    if (mkey_eq(src, dst)) return true;            // self-mount

    u32 pass = mvisit_next(t);
    struct PgrpMhead *work = mhead_find(t, src.dc, src.devno, src.path);
    if (work) {
        work->visit = pass;
        work->work  = NULL;
    }
    while (work) {
        struct PgrpMhead *h = work;
        work = h->work;
        for (const struct PgrpMount *m = h->first; m; m = m->next) {
            struct mkey k = spoor_mkey(m->source);
            if (mkey_eq(k, dst)) return true;
            struct PgrpMhead *n = mhead_find(t, k.dc, k.devno, k.path);
            if (n && n->visit != pass) {
                n->visit = pass;
                n->work  = work;
                work     = n;
            }
        }
    }
    return false;
}

int mount(struct Territory *territory, struct Spoor *source,
          struct Spoor *mountpoint, u32 flags) {
    if (!territory)                    extinction("mount(NULL territory)");
    if (territory->magic != PGRP_MAGIC) extinction("mount on corrupted Territory");

    if (!source_is_valid(source))       return -1;
    // The mount point is a transient resolved Spoor; we copy its IDENTITY
    // (dc, devno, qid.path), and retain the Spoor only as the covered member of
    // a union this mount starts (below). Reject NULL / corrupted.
    if (!mountpoint)                    return -1;
    if (mountpoint->magic != SPOOR_MAGIC) return -1;

    // ARCH 9.1: what this mount may need is allocated before ns_lock is taken,
    // so a failed allocation refuses it before any edit. The most it needs is
    // a union's start -- two entries (the member and the covered directory) at
    // a new point -- and what it does not use is freed after the lock.
    struct PgrpMount *e_new = mount_node_alloc(g_mount_cache);
    struct PgrpMount *e_cov = mount_node_alloc(g_mount_cache);
    struct PgrpMhead *h_new = mount_node_alloc(g_mhead_cache);
    if (!e_new || !e_cov || !h_new) {
        kmem_cache_free(g_mount_cache, e_new);
        kmem_cache_free(g_mount_cache, e_cov);
        kmem_cache_free(g_mhead_cache, h_new);
        return -4;
    }

    // RW-4 SA-F1: the cycle check + idempotency scan + MREPL/insert all read or
    // write the table -- serialize under ns_lock against a concurrent
    // mount/unmount/clone on a peer thread. spoor_ref is leaf-safe inside the
    // lock. An entry this mount removes (MREPL's group replace, a reposition) is
    // unlinked under the lock and released after it (mount_dead): its source's
    // Dev close hook may sleep -- never hold a spinlock across it.
    struct mount_dead dead;
    mount_dead_init(&dead);
    struct PgrpMhead *h = NULL;
    int rc;
    spin_lock(&territory->ns_lock);

    // I-3: reject a mount that would create a cycle in the mount-identity graph
    // (a self-mount, or a cross-tree oscillation). stalk-2 audit F1 -- enforce
    // the DAG here rather than relying on cross_mounts' loop bound, so a
    // cyclic mount cannot be installed + then resolve to a wrong endpoint.
    if (would_create_mount_cycle(territory, source, mountpoint)) { rc = -3; goto out; }

    h = mhead_of(territory, mountpoint);

    // Plan 9 cmount: an MBEFORE / MAFTER mount at a point with NOTHING mounted
    // there also makes the covered directory a member (MCOVERED). A point is
    // hashed only while it hosts a member, so "no point" is "nothing mounted".
    // Judged here, before the reposition below can remove this pair: a point
    // that hosted it was not fresh, and re-mounting the sole member of an MREPL
    // group must not grow a covered member (territory.tla
    // BUGGY_FRESH_AFTER_REMOVE). MREPL wins over the ordering flags (Plan 9
    // encodes the three as one field), so it never starts a union; SYS_MOUNT
    // refuses the combinations. Only a directory can be searched as a member,
    // so no union starts at a file point. SYS_MOUNT refuses an ordered mount
    // there (Plan 9's Emount, ARCH 9.6.1); this conjunct holds the line for
    // mount()'s kernel callers (territory.tla BUGGY_COVER_FILE;
    // territory_mount.covered_file_point_stays_plain).
    bool starts_union = !(flags & MREPL) && (flags & (MBEFORE | MAFTER)) &&
                        (mountpoint->qid.type & QTDIR) && !h;

    // Idempotency: (key(mountpoint), source) pair already in the table → no new
    // entry, no refcount bump. territory.tla keeps each point's members in
    // order, with their MBEFORE and MCREATE bits, but has no flagless mount,
    // so the convergence below sits beneath the model (its Reposition is the
    // MBEFORE / MAFTER arm).
    //
    // #219: the arm used to `goto out` with rc = 0 WITHOUT consulting flags, so
    // mount(..., MNOEXEC) over an already-mounted pair reported success and
    // never applied the restriction. The direction is what made it worth
    // fixing: silent success in the PERMISSIVE direction is the dangerous one
    // for a flag that carries an enforcement decision (#217's MNOEXEC), and a
    // return value satisfied by the un-restricted state teaches its caller
    // nothing.
    //
    // Converge in BOTH directions rather than refusing on a mismatch, so 0
    // always means "THIS ENTRY now says what you asked for" -- which is not the
    // same as "the restriction is gone", and must not be read as it: coverage
    // is an ANY-scan over the table (mount_noexec_covers), so a device instance
    // mounted at two points stays covered while EITHER entry carries the bit.
    // Dropping the bit here converges one entry, never a device. Converging is the
    // honest semantic here because it grants no authority the caller lacked:
    // unmount() is ungated and MREPL overwrites `flags` wholesale below, so the
    // same loosening is already two calls away for anyone who can reach this
    // one. Refusing would buy nothing and would invite the belief that a
    // mount flag is a LOCK -- it is not, deliberately (see
    // territory_mount.noexec_covers: "authority conferred by a namespace edit
    // is revoked by the inverse edit"). If a lock is ever wanted it needs a
    // locked-by-WHOM axis that Territory has no notion of today.
    //
    // mp_path is deliberately NOT re-captured (MREPL does re-capture it): I-33
    // makes Path non-load-bearing -- write-only, cosmetic to /proc/<pid>/ns --
    // and the fresh mountpoint Spoor keys to the same identity by construction,
    // so a ref-swap under ns_lock would buy no semantic difference.
    // UM-8 F6: MREPL is handled by the group-replace below (which removes this
    // pair too), so it must NOT short-circuit here -- else MREPL of an existing
    // member would converge flags and leave the OTHER members in place. For a
    // flagless re-mount (no ordering change requested) converge flags in place
    // (the #219 direction). For MBEFORE/MAFTER on an EXISTING pair, MOVE it to
    // the requested position: unlink it here and fall through to re-link it at
    // the ordering position below -- else the reorder was a silent no-op that
    // reported success (F6).
    if (!(flags & MREPL) && h) {
        for (struct PgrpMount *m = h->first; m; m = m->next) {
            if (m->source != source) continue;
            if (!(flags & (MBEFORE | MAFTER))) {
                m->flags = flags;   // in-place converge
                rc = 0;
                goto out;
            }
            // Reposition: remove now, re-insert at the ordering position below.
            // Releasing the old entry drops its ref; the insert takes a fresh
            // one (source stays alive on the caller's borrowed ref).
            mount_unlink(territory, m);
            mount_dead_entry(&dead, m);
            break;   // at most one (point, source) pair
        }
    }

    // UM (union mounts): MREPL replaces the WHOLE group -- every member at the
    // point is removed, then this one installed (the spec's MountRepl replaces
    // the sequence, not just its head). The point stays: the new member is
    // linked at it below.
    if ((flags & MREPL) && h) {
        while (h->first) {
            struct PgrpMount *m = h->first;
            mount_unlink(territory, m);
            mount_dead_entry(&dead, m);
        }
    }

    // A union's first mount installs two entries: the covered directory's and
    // the new member's. Nothing above removed an entry unless this mount's own
    // point hosted it, and then the count fell by at least the one this
    // mount adds (starts_union is false there), so this refusal never strands
    // a removal.
    if (territory->nmounts + (starts_union ? 2 : 1) > PGRP_MOUNT_CAP) { rc = -2; goto out; }

    // Take the per-entry references BEFORE linking the entries. The path
    // below is infallible after the refs, so no rollback is needed. The covered
    // entry's source is `mountpoint` itself, retained -- the one case where the
    // table keeps the point's Spoor rather than copying its key.
    spoor_ref(source);
    if (starts_union) spoor_ref(mountpoint);

    if (!h) {
        h     = h_new;
        h_new = NULL;
        mhead_insert(territory, h, mountpoint->dc, mountpoint->devno,
                     mountpoint->qid.path);
    }
    // #66 (I-33): each entry retains the mount-POINT's namespace name for
    // /proc/<pid>/ns. path_ref is NULL-safe (a kernel-internal mountpoint with
    // no retained name -> NULL mp_path -> rendered "?"). Introspection-only: the
    // entry keys on its point's (dc, devno, qid.path) identity, never on mp_path.
    struct PgrpMount *m = e_new;
    e_new      = NULL;
    m->source  = source;
    m->mp_path = mountpoint->path;
    path_ref(m->mp_path);
    m->flags   = flags;
    struct PgrpMount *cov = NULL;
    if (starts_union) {
        cov          = e_cov;
        e_cov        = NULL;
        cov->source  = mountpoint;
        cov->mp_path = mountpoint->path;
        path_ref(cov->mp_path);
        cov->flags   = MCOVERED;
    }

    // UM (union mounts): place the new member in the point's SEARCH ORDER
    // (ARCH 9.5/9.6; territory.tla MountRepl / MountBefore / MountAfter).
    // MBEFORE puts it ahead of the point's members (Plan 9 prepend; later
    // MBEFOREs go to the very front, LIFO); MAFTER, the flagless default and
    // MREPL's replacement go after them (searched last, FIFO). A union's first
    // mount finds no member, so both its entries append, in Plan 9's order:
    // <new, covered> for MBEFORE, <covered, new> for MAFTER.
    bool before = (flags & MBEFORE) != 0;
    if (cov && !before) mount_link(territory, h, cov, false);
    mount_link(territory, h, m, before);
    if (cov && before)  mount_link(territory, h, cov, false);
    rc = 0;

out:
    // Every path that empties the point above links a member at it again.
    if (h && !h->first) extinction("mount: left a mount point with no member in the hash");
    spin_unlock(&territory->ns_lock);
    mount_dead_release(&dead);
    kmem_cache_free(g_mount_cache, e_new);    // NULL when used
    kmem_cache_free(g_mount_cache, e_cov);
    kmem_cache_free(g_mhead_cache, h_new);
    return rc;
}

int unmount(struct Territory *territory, struct Spoor *mountpoint) {
    if (!territory)                    extinction("unmount(NULL territory)");
    if (territory->magic != PGRP_MAGIC) extinction("unmount on corrupted Territory");
    if (!mountpoint)                    return -1;
    if (mountpoint->magic != SPOOR_MAGIC) return -1;

    // RW-4 SA-F1: find + unlink under ns_lock against a concurrent
    // mount/unmount/clone; the removed entry is released OUTSIDE the lock (its
    // source's Dev close hook may sleep).
    struct mount_dead dead;
    mount_dead_init(&dead);
    int rc = -1;
    spin_lock(&territory->ns_lock);
    struct PgrpMhead *h = mhead_of(territory, mountpoint);
    struct PgrpMount *m = h ? h->first : NULL;
    while (m && (m->flags & MCOVERED)) m = m->next;   // never unmounted by itself
    if (m) {
        // Released with spoor_clunk (not spoor_unref) so the Dev's close hook
        // runs if this is the last holder -- the ARCH 9.6.6 lifecycle where the
        // user already closed the attach_9p fd and the mount-table was the only
        // holder keeping the 9P session alive (P5-mount-syscall fix). The FIRST
        // member in search order goes, and the others keep their order; call
        // again to drop the next union member (the contract is unchanged).
        mount_unlink(territory, m);
        mount_dead_entry(&dead, m);
        rc = 0;
        // The covered directory leaves with the last member mounted at the
        // point, so the point is a plain directory again rather than a union of
        // itself -- and a point left with no member leaves the hash.
        struct PgrpMount *cov = NULL;
        bool mounted = false;
        for (struct PgrpMount *o = h->first; o; o = o->next) {
            if (o->flags & MCOVERED) cov = o;
            else                     mounted = true;
        }
        if (cov && !mounted) {
            mount_unlink(territory, cov);
            mount_dead_entry(&dead, cov);
        }
        if (!h->first) {
            mhead_remove(territory, h);
            mount_dead_point(&dead, h);
        }
    }
    spin_unlock(&territory->ns_lock);
    mount_dead_release(&dead);
    return rc;
}

// mount_lookup -- the `stalk` cross-mount probe (Plan 9 domount). Returns a
// REF-HELD source of the FIRST member (search order) of the point whose identity
// matches `probe`'s (dc, devno, qid.path), or NULL. RW-4 SA-F1: the lookup +
// spoor_ref happen atomically under ns_lock so a concurrent unmount cannot free
// the source between the lookup and the caller's use. THE CALLER MUST
// spoor_clunk the result (stalk_cross_mounts clunks it after clone_walk_zero
// mints the crossed Spoor).
struct Spoor *mount_lookup(struct Territory *territory, struct Spoor *probe,
                           u32 *flags_out) {
    if (flags_out) *flags_out = 0;
    if (!territory)                    return NULL;
    if (territory->magic != PGRP_MAGIC) extinction("mount_lookup on corrupted Territory");
    if (!probe)                        return NULL;
    if (probe->magic != SPOOR_MAGIC)   extinction("mount_lookup probe corrupted Spoor");

    struct Spoor *src = NULL;
    spin_lock(&territory->ns_lock);
    struct PgrpMhead *h = mhead_of(territory, probe);
    if (h) {
        src = h->first->source;
        spoor_ref(src);   // transfer a ref to the caller (atomic vs unmount)
        // VIVARIUM section 13: hand back the entry's flags under the same
        // lock, so the resolver can see MPHENO_LINUX on the mount it crosses
        // without a second lookup (and without racing an unmount).
        if (flags_out) *flags_out = h->first->flags;
    }
    spin_unlock(&territory->ns_lock);
    return src;
}

// mount_member_at (UM) -- the ordered union iterator. Returns a REF-HELD source
// of the `index`-th member of the point whose identity matches `probe`, in its
// declared search order, or NULL when `index` is past the last member. Same
// atomic ns_lock discipline as mount_lookup. The resolver iterates index
// 0,1,2,... trying the next component in each member and stopping at the first
// that resolves it. THE CALLER MUST spoor_clunk the returned Spoor.
struct Spoor *mount_member_at(struct Territory *territory, struct Spoor *probe,
                              int index, u32 *flags_out) {
    if (flags_out) *flags_out = 0;
    if (!territory)                    return NULL;
    if (territory->magic != PGRP_MAGIC) extinction("mount_member_at on corrupted Territory");
    if (!probe)                        return NULL;
    if (probe->magic != SPOOR_MAGIC)   extinction("mount_member_at probe corrupted Spoor");
    if (index < 0)                     return NULL;

    struct Spoor *src = NULL;
    spin_lock(&territory->ns_lock);
    struct PgrpMhead *h = mhead_of(territory, probe);
    struct PgrpMount *m = h ? h->first : NULL;
    for (int i = 0; m && i < index; i++) m = m->next;
    if (m) {
        src = m->source;
        spoor_ref(src);   // transfer a ref to the caller (atomic vs unmount)
        if (flags_out) *flags_out = m->flags;
    }
    spin_unlock(&territory->ns_lock);
    return src;
}

// mount_members_snapshot (UM, union mounts): capture EVERY member of the union
// at `probe` in ONE ns_lock hold -- each source ref-held, its flags recorded,
// in declared search order -- when they fit in `max`; otherwise capture nothing
// and return the count, so the caller can retry with room for it. Returns the
// member count (0 if `probe` is not a mount point). THE CALLER MUST spoor_clunk
// every returned Spoor. This is the atomic form of iterating
// mount_member_at(0),(1),...: those take the lock once PER index, so a
// concurrent unmount can shift the member set between calls (UM-7 F4 -- an
// index-based re-derivation then crosses a DIFFERENT member than it
// inspected). A single-hold snapshot closes that window for any operation over
// the WHOLE union (the readdir member snapshot at open; the resolver's union
// walk).
int mount_members_snapshot(struct Territory *territory, struct Spoor *probe,
                           struct Spoor **out, u32 *flags_out, int max) {
    if (!territory)                    return 0;
    if (territory->magic != PGRP_MAGIC) extinction("mount_members_snapshot on corrupted Territory");
    if (!probe || !out || max <= 0)    return 0;
    if (probe->magic != SPOOR_MAGIC)   extinction("mount_members_snapshot probe corrupted Spoor");

    int n = 0;
    spin_lock(&territory->ns_lock);
    struct PgrpMhead *h = mhead_of(territory, probe);
    if (h) {
        n = (int)h->nmembers;
        if (n <= max) {
            int k = 0;
            for (struct PgrpMount *m = h->first; m && k < n; m = m->next, k++) {
                out[k] = m->source;
                spoor_ref(out[k]);           // transfer a ref to the caller (atomic vs unmount)
                if (flags_out) flags_out[k] = m->flags;
            }
        }
    }
    spin_unlock(&territory->ns_lock);
    return n;
}

bool mount_is_point_id(struct Territory *territory, int dc, u64 devno,
                       u64 qid_path) {
    if (!territory)                    return false;
    if (territory->magic != PGRP_MAGIC) extinction("mount_is_point_id on corrupted Territory");

    spin_lock(&territory->ns_lock);
    bool hit = mhead_find(territory, dc, devno, qid_path) != NULL;
    spin_unlock(&territory->ns_lock);
    return hit;
}

bool mount_noexec_covers(struct Territory *territory, int dc, u64 devno) {
    if (!territory)                    return false;
    if (territory->magic != PGRP_MAGIC) extinction("mount_noexec_covers on corrupted Territory");

    // A scan of every entry, not a lookup: the question is about a SOURCE's
    // device instance, and the table is keyed on mount points.
    bool noexec = false;
    spin_lock(&territory->ns_lock);
    for (const struct PgrpMount *m = territory->mfirst; m; m = m->tnext) {
        if (!(m->flags & MNOEXEC)) continue;
        // dc/devno are set at spoor_alloc and never mutate, so reading them
        // through the entry needs only the ns_lock that keeps `source` alive.
        if (!m->source) continue;
        if (m->source->dc == dc && m->source->devno == devno) {
            noexec = true;
            break;
        }
    }
    spin_unlock(&territory->ns_lock);
    return noexec;
}

// =============================================================================
// The mount-table shed (ARCH 9.6.10; #80; specs/territory_shed.tla).
// =============================================================================
//
// unmount() takes a RESOLVED mount point, so once a root swap makes a mount
// point unreachable its entry can never be named again: it cannot be removed,
// it is deep-copied into every child by territory_clone, and it holds its
// source for the life of the namespace. The shed drops those entries at the
// swap.
//
// Reachability is per DEVICE INSTANCE (dc, devno), the least set R with
//      instance(new root) in R
//      instance(mount point) in R  =>  instance(source) in R
// which is sound because the resolver only descends ('..' pops stalk's in-call
// trail and is a no-op at the base; a symlink re-anchors at root_spoor): every
// Spoor a resolution from the new root can hold is in R, so an entry keyed
// outside R can never fire for one (the spec's ShedLosesNothing). It is per
// instance, not per directory, because the kernel cannot see inside a 9P tree;
// that makes it conservative -- it may keep an entry no walk reaches, and never
// drops one a walk can. A Dev whose walk does not preserve devno
// (Dev.devno_per_walker: devenv) is matched on dc alone, conservative again.
//
// One Spoor is consulted at the base WITHOUT having been walked to: a union
// root's mount point. An open of a union directory (O_PATH or OREAD -- both
// pass the directory gate chroot / pivot_root apply) has member[0]'s identity
// and carries the point in union_snap, and stalk routes every first
// component from such a base through the entries keyed AT THE POINT
// (union_base = base->union_snap->point). The point lives in the tree the union
// was mounted in, so its instance is a second seed; without it the shed drops
// the union's own entries and every name under the new root is ENOENT.
//
// The pass marks the points whose instance it reaches and queues each on its
// own `work` link, so it needs no array and allocates nothing. Finding the
// points of an instance scans every point (the hash is keyed on the whole
// identity), so the pass costs (members of the reached points) x (points):
// about a million steps at PGRP_MOUNT_CAP, once per root swap.

// Mark reached, and queue, every point not yet reached in the device instance
// of `s` -- in its dc alone when its Dev's walk does not preserve devno.
static void shed_reach(struct Territory *t, u32 pass, const struct Spoor *s,
                       struct PgrpMhead **work) {
    bool any_devno = s->dev && s->dev->devno_per_walker;
    for (int b = 0; b < PGRP_MOUNT_HASH; b++) {
        for (struct PgrpMhead *h = t->mhash[b]; h; h = h->hnext) {
            if (h->visit == pass || h->dc != s->dc) continue;
            if (!any_devno && h->devno != s->devno) continue;
            h->visit = pass;
            h->work  = *work;
            *work    = h;
        }
    }
}

// Drop every mount entry unreachable from `root`. Called with ns_lock HELD, in
// the same hold that installed `root` as root_spoor, so a peer thread sees
// either the old root with the whole old table or the new root with the shed
// one. No allocation, no sleep. Each dropped entry, and each point it leaves
// with no member, is unlinked here and chained on `dead`, which the caller
// releases OUTSIDE the lock, as unmount() does -- a Dev close hook may sleep.
// Survivors keep their order, which IS the union search order.
static void territory_shed_unreachable_locked(struct Territory *t,
                                              const struct Spoor *root,
                                              struct mount_dead *dead) {
    u32 pass = mvisit_next(t);
    struct PgrpMhead *work = NULL;
    shed_reach(t, pass, root, &work);
    // union_snap is set once before the Spoor is published and freed with it,
    // and the caller holds a root ref, so this read needs no more than that.
    if (root->union_snap && root->union_snap->point)
        shed_reach(t, pass, root->union_snap->point, &work);
    while (work) {
        struct PgrpMhead *h = work;
        work = h->work;
        for (const struct PgrpMount *m = h->first; m; m = m->next)
            shed_reach(t, pass, m->source, &work);
    }

    struct PgrpMount *next;
    for (struct PgrpMount *m = t->mfirst; m; m = next) {
        next = m->tnext;
        struct PgrpMhead *h = m->head;
        if (h->visit == pass) continue;
        mount_unlink(t, m);
        mount_dead_entry(dead, m);
        if (!h->first) {
            mhead_remove(t, h);
            mount_dead_point(dead, h);
        }
    }
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

    // RW-4 SA-F1: the idempotency check + read-old + ref + swap run under ns_lock
    // so a concurrent FROM_ROOT reader (territory_root_ref) and a peer chroot/pivot
    // cannot observe a torn swap or free `old` mid-read. spoor_ref is leaf-safe
    // inside; the displaced root's spoor_clunk is deferred to OUTSIDE the lock (its
    // Dev close hook may sleep).
    struct Spoor *old = NULL;
    struct mount_dead shed;
    mount_dead_init(&shed);
    spin_lock(&territory->ns_lock);
    // Idempotent same-pointer: same Spoor reasserted as root -> no-op success;
    // refcount unchanged, and NO shed (no swap happened). Matches spec
    // precondition `root_spoor[p] # s`.
    if (territory->root_spoor == source) {
        spin_unlock(&territory->ns_lock);
        return 0;
    }
    old = territory->root_spoor;
    // Bump BEFORE swap. spoor_ref extincts on a corrupted source; under a clean
    // Spoor the swap is then infallible.
    spoor_ref(source);
    territory->root_spoor = source;
    // ARCH 9.6.10: end the old generation in the same hold as the swap.
    territory_shed_unreachable_locked(territory, source, &shed);
    spin_unlock(&territory->ns_lock);

    mount_dead_release(&shed);
    if (old) {
        // spoor_clunk (not spoor_unref) -- see header comment + mount() MREPL
        // precedent. If this Territory was the last holder of `old`, the Dev's
        // close hook runs here.
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
    // RW-4 SA-F1: the precondition + idempotency + read-old + ref + swap run
    // under ns_lock (so a concurrent FROM_ROOT reader / peer pivot cannot race
    // the check or free `old` mid-read); the displaced root's spoor_clunk is
    // deferred to OUTSIDE the lock (its Dev close hook may sleep).
    struct Spoor *old = NULL;
    struct mount_dead shed;
    mount_dead_init(&shed);
    spin_lock(&territory->ns_lock);
    if (!territory->root_spoor) {
        spin_unlock(&territory->ns_lock);
        return -1;
    }
    // Idempotent same-pointer: same Spoor reasserted as root -> no-op success;
    // refcount unchanged, and NO shed (no swap happened). Matches
    // territory_chroot + Plan 9 / Linux pivot-to-same.
    if (territory->root_spoor == source) {
        spin_unlock(&territory->ns_lock);
        return 0;
    }
    old = territory->root_spoor;
    // Bump BEFORE swap. spoor_ref extincts on a corrupted source; the swap is
    // then infallible.
    spoor_ref(source);
    territory->root_spoor = source;
    // ARCH 9.6.10: end the old generation in the same hold as the swap. This
    // is what returns joey's seven boot-generation entries.
    territory_shed_unreachable_locked(territory, source, &shed);
    spin_unlock(&territory->ns_lock);

    mount_dead_release(&shed);

    // spoor_clunk (NOT spoor_unref) on the displaced root: if THIS Territory was
    // the last holder, the Dev's close hook runs. Same discipline as
    // territory_chroot + mount()'s MREPL displacement. For joey's pivot the old
    // devramfs root is held by kproc as well (system-wide; ARCH-invariant for the
    // v1.0 boot path), so this clunk drops joey's per-Territory ref but does NOT
    // free the underlying tree structurally. `old` is non-NULL (precondition).
    spoor_clunk(old);
    return 0;
}

// territory_root_ref -- atomically read root_spoor + take a ref under ns_lock, so
// the read+ref cannot race a concurrent territory_pivot_root / territory_chroot
// that swaps root_spoor + clunks the displaced one to zero (RW-4 SA-F1). Returns
// a REF-HELD root Spoor (caller spoor_clunks it) or NULL if no root is set.
struct Spoor *territory_root_ref(struct Territory *territory) {
    if (!territory)                    return NULL;
    if (territory->magic != PGRP_MAGIC) extinction("territory_root_ref on corrupted Territory");
    spin_lock(&territory->ns_lock);
    struct Spoor *r = territory->root_spoor;
    if (r) spoor_ref(r);
    spin_unlock(&territory->ns_lock);
    return r;
}
