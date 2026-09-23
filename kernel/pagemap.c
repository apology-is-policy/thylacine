// B-1a': the pagemap -- a Burrow's charged, on-touch sparse slot table. See
// <thylacine/pagemap.h> for the contract, the two representations and the
// locking + charging rules; this file is the mechanics.

#include <thylacine/pagemap.h>

#include <thylacine/addrspace.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>

#include "../mm/phys.h"
#include "../mm/slub.h"

// =============================================================================
// Node helpers. A node is one KP_ZERO page of PAGEMAP_NODE_ENTRIES pointers;
// its occupancy (non-NULL entries) lives in its own descriptor's refcount,
// ESTABLISHED at 0 here and never inherited from the buddy.
// =============================================================================

static inline void **node_kva(struct page *pg) {
    return (void **)pa_to_kva(page_to_pa(pg));
}

static inline struct page *node_page(void *kva) {
    return pa_to_page(kva_to_pa(kva));
}

static struct page *node_alloc(bool exempt) {
    struct page *pg = alloc_user_pages(0, KP_ZERO, exempt);
    if (pg) pg->refcount = 0;
    return pg;
}

// Slot `idx`'s child index at level `level` of a `depth`-level radix (level 0
// is the root, depth-1 the leaf).
static inline size_t level_index(const struct pagemap *pm, size_t idx, u32 level) {
    u32 shift = PAGEMAP_NODE_SHIFT * (pm->depth - 1u - level);
    return (idx >> shift) & (PAGEMAP_NODE_ENTRIES - 1u);
}

// Smallest depth whose 512^depth covers `count`, or 0 if none does.
static u32 depth_for(size_t count) {
    size_t cap = PAGEMAP_NODE_ENTRIES;
    for (u32 d = 1; d <= PAGEMAP_MAX_DEPTH; d++) {
        if (count <= cap) return d;
        cap <<= PAGEMAP_NODE_SHIFT;
    }
    return 0;
}

int pagemap_init(struct pagemap *pm, size_t count) {
    if (!pm)        return -1;
    if (count == 0) return -1;
    pm->count = 0; pm->depth = 0; pm->nodes = 0; pm->resident = 0; pm->root = NULL;

    if (count <= PAGEMAP_INLINE_MAX) {
        void *leaf = kmalloc(count * sizeof(struct page *), KP_ZERO);
        if (!leaf) return -1;
        pm->root  = leaf;
        pm->count = count;
        return 0;
    }
    u32 depth = depth_for(count);
    if (depth == 0) return -1;
    pm->depth = depth;
    pm->count = count;
    return 0;
}

struct page *pagemap_get(const struct pagemap *pm, size_t idx) {
    if (!pm || idx >= pm->count) return NULL;
    if (pm->depth == 0) return ((struct page **)pm->root)[idx];
    void **node = (void **)pm->root;
    for (u32 level = 0; node && level < pm->depth - 1u; level++)
        node = (void **)node[level_index(pm, idx, level)];
    if (!node) return NULL;
    return (struct page *)node[level_index(pm, idx, pm->depth - 1u)];
}

// The number of nodes the path to `idx` lacks. Caller holds the lock.
static u32 path_missing(const struct pagemap *pm, size_t idx) {
    u32 missing = 0;
    void **node = (void **)pm->root;
    if (!node) return pm->depth;
    for (u32 level = 0; level < pm->depth - 1u; level++) {
        node = (void **)node[level_index(pm, idx, level)];
        if (!node) { missing = pm->depth - 1u - level; break; }
    }
    return missing;
}

// Walk to the leaf of `idx`, linking pool nodes into every gap. Returns the
// leaf, or NULL if the pool ran out (nothing partially linked is undone -- a
// linked node is a valid empty node, and the caller retries with more).
// Caller holds the lock. Consumed pool entries are NULLed.
static void **path_build(struct pagemap *pm, size_t idx, struct page **pool, u32 pool_n) {
    u32 used = 0;
    if (!pm->root) {
        if (used >= pool_n || !pool[used]) return NULL;
        pm->root = node_kva(pool[used]);
        pool[used++] = NULL;
        pm->nodes++;
    }
    void **node = (void **)pm->root;
    for (u32 level = 0; level < pm->depth - 1u; level++) {
        size_t ci = level_index(pm, idx, level);
        void **child = (void **)node[ci];
        if (!child) {
            if (used >= pool_n || !pool[used]) return NULL;
            child = node_kva(pool[used]);
            pool[used++] = NULL;
            node[ci] = child;
            node_page((void *)node)->refcount++;      // one more child
            pm->nodes++;
        }
        node = child;
    }
    return node;
}

int pagemap_install(struct pagemap *pm, spin_lock_t *lock, size_t idx,
                    struct page *pg, struct AddrSpace *as, bool exempt,
                    struct page **out_winner) {
    if (out_winner) *out_winner = NULL;
    if (!pm || !lock || !pg)  return -1;
    if (idx >= pm->count)     return -1;

    if (pm->depth == 0) {
        struct page **leaf = (struct page **)pm->root;
        spin_lock(lock);
        if (leaf[idx]) {
            if (out_winner) *out_winner = leaf[idx];
            spin_unlock(lock);
            return 1;
        }
        leaf[idx] = pg;
        pm->resident++;
        spin_unlock(lock);
        return 0;
    }

    // The pool is a PREFIX: entries [0, pool_n) are allocated nodes, consumed
    // from the front by path_build (which NULLs what it takes). At most
    // PAGEMAP_MAX_DEPTH nodes can ever be missing on one path.
    struct page *pool[PAGEMAP_MAX_DEPTH] = { NULL, NULL, NULL, NULL };
    u32 pool_n = 0;
    for (;;) {
        spin_lock(lock);
        u32 missing = path_missing(pm, idx);
        if (missing <= pool_n) {
            void **leaf = path_build(pm, idx, pool, pool_n);
            if (!leaf) extinction("pagemap_install: path_build short with a sufficient pool");
            size_t li = level_index(pm, idx, pm->depth - 1u);
            int rc;
            if (leaf[li]) {
                if (out_winner) *out_winner = (struct page *)leaf[li];
                rc = 1;
            } else {
                leaf[li] = pg;
                node_page((void *)leaf)->refcount++;
                pm->resident++;
                rc = 0;
            }
            spin_unlock(lock);
            // Leftovers: a racer built part of the path first. Return them,
            // with their charge.
            u32 left = 0;
            for (u32 i = 0; i < pool_n; i++)
                if (pool[i]) { free_pages(pool[i], 0); pool[i] = NULL; left++; }
            if (left && as) addrspace_uncharge_pages(as, left);
            return rc;
        }
        spin_unlock(lock);

        // Allocate the shortfall, charged FIRST so a cap hit allocates nothing.
        u32 need = missing - pool_n;
        if (as && !addrspace_charge_pages(as, need, exempt)) goto fail;
        u32 got = 0;
        while (got < need) {
            struct page *np = node_alloc(exempt);
            if (!np) break;
            pool[pool_n++] = np;
            got++;
        }
        if (got < need) {
            if (as) addrspace_uncharge_pages(as, need - got);   // the part never allocated
            goto fail;
        }
    }

fail:;
    u32 left = 0;
    for (u32 i = 0; i < pool_n; i++)
        if (pool[i]) { free_pages(pool[i], 0); pool[i] = NULL; left++; }
    if (left && as) addrspace_uncharge_pages(as, left);
    return -1;
}

// Unlink every node on `path` that the take of slot `idx` emptied, leaf
// upward, handing the node pages back in `freed` (capacity PAGEMAP_MAX_DEPTH)
// for the caller to free outside the lock. Caller holds the lock. Returns the
// count handed back; the accounting (pm->nodes) settles here.
static u32 unlink_emptied(struct pagemap *pm, void ***path, size_t idx,
                          struct page **freed) {
    u32 nf = 0;
    for (int lv = (int)pm->depth - 1; lv >= 0; lv--) {
        struct page *np = node_page((void *)path[lv]);
        if (np->refcount == 0) extinction("pagemap_take: node occupancy underflow");
        np->refcount--;
        if (np->refcount != 0) break;
        if (lv == 0) pm->root = NULL;
        else path[lv - 1][level_index(pm, idx, (u32)lv - 1u)] = NULL;
        if (freed) freed[nf] = np;
        nf++;
        pm->nodes--;
    }
    return nf;
}

void pagemap_take(struct pagemap *pm, spin_lock_t *lock, size_t idx,
                  struct AddrSpace *as, struct page **out_pg,
                  struct page **freed, u32 *nfreed) {
    if (out_pg) *out_pg = NULL;
    if (nfreed) *nfreed = 0;
    if (!pm || !lock || idx >= pm->count) return;

    if (pm->depth == 0) {
        struct page **leaf = (struct page **)pm->root;
        spin_lock(lock);
        struct page *pg = leaf[idx];
        if (pg) { leaf[idx] = NULL; pm->resident--; }
        spin_unlock(lock);
        if (out_pg) *out_pg = pg;
        return;
    }

    void **path[PAGEMAP_MAX_DEPTH];     // node at each level
    spin_lock(lock);
    void **node = (void **)pm->root;
    u32 level = 0;
    while (node && level < pm->depth) {
        path[level] = node;
        if (level == pm->depth - 1u) break;
        node = (void **)node[level_index(pm, idx, level)];
        level++;
    }
    if (!node) { spin_unlock(lock); return; }       // absent: nothing to take

    void **leaf = path[pm->depth - 1u];
    size_t li = level_index(pm, idx, pm->depth - 1u);
    struct page *pg = (struct page *)leaf[li];
    if (!pg) { spin_unlock(lock); return; }
    leaf[li] = NULL;
    pm->resident--;
    u32 nf = unlink_emptied(pm, path, idx, freed);
    spin_unlock(lock);
    if (nf && as) addrspace_uncharge_pages(as, nf);
    if (nfreed) *nfreed = nf;
    if (out_pg) *out_pg = pg;
}

static u64 g_pagemap_walk_steps;

u64 pagemap_walk_steps(void) {
    return __atomic_load_n(&g_pagemap_walk_steps, __ATOMIC_RELAXED);
}

// The first slot under entry `e` of the level-`level` node on the path to
// `idx`: idx's prefix above that level, then `e`, then zeros.
static inline size_t entry_first_slot(const struct pagemap *pm, size_t idx,
                                      u32 level, size_t e) {
    u32 shift = PAGEMAP_NODE_SHIFT * (pm->depth - 1u - level);
    size_t prefix = (idx >> shift) & ~(size_t)(PAGEMAP_NODE_ENTRIES - 1u);
    return (prefix | e) << shift;
}

size_t pagemap_take_next(struct pagemap *pm, spin_lock_t *lock, size_t from,
                         size_t hi, struct AddrSpace *as, struct page **out_pg,
                         struct page **freed, u32 *nfreed) {
    if (out_pg) *out_pg = NULL;
    if (nfreed) *nfreed = 0;
    if (!pm || !lock || pm->count == 0) return hi;
    if (hi > pm->count) hi = pm->count;
    if (from >= hi) return hi;

    if (pm->depth == 0) {
        struct page **leaf = (struct page **)pm->root;
        size_t took = hi;
        struct page *pg = NULL;
        spin_lock(lock);
        for (size_t i = from; i < hi; i++) {
            if (!leaf[i]) continue;
            pg = leaf[i]; leaf[i] = NULL; pm->resident--; took = i;
            break;
        }
        spin_unlock(lock);
        if (out_pg) *out_pg = pg;
        return took;
    }

    // Descend from the root along `idx`; at each node scan forward from idx's
    // own entry for the first present child. An exhausted node resumes at the
    // next entry of the nearest ancestor that has one (never re-scanning an
    // entry already passed, so nothing below `from` is ever reached). A present
    // entry advances `idx` to the first slot beneath it; the leaf's is the slot
    // taken, unlinked upward exactly as pagemap_take does.
    void **path[PAGEMAP_MAX_DEPTH];
    u64 steps = 0;
    size_t idx = from, took = hi;
    struct page *pg = NULL;
    u32 nf = 0;
    spin_lock(lock);
    if (pm->root) {
        path[0] = (void **)pm->root;
        u32 level = 0;
        for (;;) {
            void **node = path[level];
            size_t own = level_index(pm, idx, level);
            size_t e = own;
            while (e < PAGEMAP_NODE_ENTRIES && !node[e]) { e++; steps++; }
            if (e == PAGEMAP_NODE_ENTRIES) {
                size_t ne;
                do {
                    if (level == 0) goto done;
                    level--;
                    ne = level_index(pm, idx, level) + 1u;
                } while (ne == PAGEMAP_NODE_ENTRIES);
                idx = entry_first_slot(pm, idx, level, ne);
                if (idx >= hi) goto done;
                continue;
            }
            steps++;
            if (e != own) idx = entry_first_slot(pm, idx, level, e);
            if (idx >= hi) goto done;
            if (level == pm->depth - 1u) {
                pg = (struct page *)node[e];
                node[e] = NULL;
                pm->resident--;
                nf = unlink_emptied(pm, path, idx, freed);
                took = idx;
                goto done;
            }
            path[level + 1u] = (void **)node[e];
            level++;
        }
    }
done:
    spin_unlock(lock);
    __atomic_add_fetch(&g_pagemap_walk_steps, steps, __ATOMIC_RELAXED);
    if (nf && as) addrspace_uncharge_pages(as, nf);
    if (nfreed) *nfreed = nf;
    if (out_pg) *out_pg = pg;
    return took;
}

bool pagemap_swap(struct pagemap *pm, spin_lock_t *lock, size_t idx,
                  struct page *expect, struct page *replacement) {
    if (!pm || !lock || !expect || !replacement) return false;
    if (idx >= pm->count)                         return false;
    bool ok = false;
    spin_lock(lock);
    if (pm->depth == 0) {
        struct page **leaf = (struct page **)pm->root;
        if (leaf[idx] == expect) { leaf[idx] = replacement; ok = true; }
    } else {
        void **node = (void **)pm->root;
        for (u32 level = 0; node && level < pm->depth - 1u; level++)
            node = (void **)node[level_index(pm, idx, level)];
        if (node) {
            size_t li = level_index(pm, idx, pm->depth - 1u);
            if ((struct page *)node[li] == expect) { node[li] = replacement; ok = true; }
        }
    }
    spin_unlock(lock);
    return ok;
}

// =============================================================================
// The clone.
// =============================================================================

u32 pagemap_pool_alloc(struct page **pool, u32 n, bool exempt) {
    u32 got = 0;
    for (; got < n; got++) {
        pool[got] = node_alloc(exempt);
        if (!pool[got]) break;
    }
    return got;
}

void pagemap_pool_free(struct page **pool, u32 n) {
    for (u32 i = 0; i < n; i++)
        if (pool[i]) { free_pages(pool[i], 0); pool[i] = NULL; }
}

// Take the next pool node, or NULL when the pool is exhausted.
static void **pool_take(struct page **pool, u32 pool_n) {
    for (u32 i = 0; i < pool_n; i++) {
        if (!pool[i]) continue;
        void **nd = node_kva(pool[i]);
        pool[i] = NULL;
        return nd;
    }
    return NULL;
}

// Mirror `src` (a node at `level`) into a fresh pool node LINKED at `*slot`
// BEFORE it is filled, so that a pool that runs short deeper down leaves a
// tree pagemap_destroy can still reach whole -- every page whose on_page ran is
// in a linked node. Returns 0, or -1 on a short pool.
static int mirror_node(struct pagemap *dst, const void **src, u32 level, void **slot,
                       struct page **pool, u32 pool_n,
                       void (*on_page)(struct page *pg, void *ctx), void *ctx) {
    void **nd = pool_take(pool, pool_n);
    if (!nd) return -1;
    *slot = nd;
    dst->nodes++;
    struct page *np = node_page((void *)nd);
    bool leaf = (level == dst->depth - 1u);
    for (size_t e = 0; e < PAGEMAP_NODE_ENTRIES; e++) {
        if (!src[e]) continue;
        if (leaf) {
            struct page *pg = (struct page *)src[e];
            if (on_page) on_page(pg, ctx);
            nd[e] = pg;
            np->refcount++;
            dst->resident++;
        } else {
            // The child links itself into nd[e] before filling; its parent's
            // occupancy counts it as soon as it is linked.
            np->refcount++;
            if (mirror_node(dst, (const void **)src[e], level + 1u, &nd[e],
                            pool, pool_n, on_page, ctx) != 0) {
                if (!nd[e]) np->refcount--;      // the link never happened
                return -1;
            }
        }
    }
    return 0;
}

int pagemap_mirror(struct pagemap *dst, const struct pagemap *src,
                   struct page **pool, u32 pool_n,
                   void (*on_page)(struct page *pg, void *ctx), void *ctx) {
    if (!dst || !src)                       return -1;
    if (dst->count != src->count)           return -1;
    if (dst->depth != src->depth)           return -1;
    if (dst->resident != 0 || dst->nodes)   return -1;   // must be empty

    if (src->depth == 0) {
        struct page **s = (struct page **)src->root;
        struct page **d = (struct page **)dst->root;
        for (size_t i = 0; i < src->count; i++) {
            if (!s[i]) continue;
            if (on_page) on_page(s[i], ctx);
            d[i] = s[i];
            dst->resident++;
        }
        return 0;
    }
    if (!src->root) return 0;               // nothing resident: an empty mirror
    return mirror_node(dst, (const void **)src->root, 0, &dst->root,
                       pool, pool_n, on_page, ctx);
}

// =============================================================================
// Teardown.
// =============================================================================

static void destroy_node(struct pagemap *pm, void **node, u32 level,
                         void (*put)(struct page *pg, void *ctx), void *ctx) {
    bool leaf = (level == pm->depth - 1u);
    for (size_t e = 0; e < PAGEMAP_NODE_ENTRIES; e++) {
        if (!node[e]) continue;
        if (leaf) { if (put) put((struct page *)node[e], ctx); }
        else      destroy_node(pm, (void **)node[e], level + 1u, put, ctx);
        node[e] = NULL;
    }
    free_pages(node_page((void *)node), 0);
}

void pagemap_destroy(struct pagemap *pm, void (*put)(struct page *pg, void *ctx),
                     void *ctx) {
    if (!pm || pm->count == 0) return;
    if (pm->depth == 0) {
        struct page **leaf = (struct page **)pm->root;
        for (size_t i = 0; i < pm->count; i++)
            if (leaf[i]) { if (put) put(leaf[i], ctx); leaf[i] = NULL; }
        kfree(leaf);
    } else if (pm->root) {
        destroy_node(pm, (void **)pm->root, 0, put, ctx);
    }
    pm->root = NULL; pm->count = 0; pm->depth = 0; pm->nodes = 0; pm->resident = 0;
}
