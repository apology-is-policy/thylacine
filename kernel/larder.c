// The Larder -- the guest-side 9P FS cache. L1c: the substrate + the attr
// sub-cache. See <thylacine/larder.h> for the design + the fs_cache.tla mapping;
// docs/LARDER-DESIGN.md for the full scripture; ARCH section 28 I-38.

#include <thylacine/larder.h>

void larder_init(struct larder *l) {
    if (!l) return;
    // Self-sufficient zero (valid=false / gen=0 / lru_clock=0 / diagnostics=0);
    // the kernel has no memset, so a byte loop, like dev9p's t_stat zero.
    for (size_t i = 0; i < sizeof(*l); i++) ((u8 *)l)[i] = 0;
    spin_lock_init(&l->lock);
}

// Find the slot holding `qid_path` (valid), or NULL. Caller holds l->lock.
static struct larder_attr_ent *attr_find_locked(struct larder *l, u64 qid_path) {
    for (u32 i = 0; i < LARDER_ATTR_ENTRIES; i++) {
        struct larder_attr_ent *e = &l->attr[i];
        if (e->valid && e->qid_path == qid_path)
            return e;
    }
    return NULL;
}

bool larder_attr_serve(struct larder *l, u64 qid_path,
                       struct t_stat *out, u64 *seq0_out) {
    if (!l || !out) return false;
    spin_lock(&l->lock);
    struct larder_attr_ent *e = attr_find_locked(l, qid_path);
    if (e) {
        // Read: copy the whole entry out UNDER the lock (a concurrent invalidate
        // cannot tear it). Bump LRU.
        *out = e->attr;
        e->lru = ++l->lru_clock;
        l->attr_hits++;
        spin_unlock(&l->lock);
        return true;
    }
    // Miss: hand back the gen snapshot for the caller's install guard (captured
    // under the same lock as the failed lookup -- a single critical section).
    if (seq0_out) *seq0_out = l->gen;
    l->attr_misses++;
    spin_unlock(&l->lock);
    return false;
}

u64 larder_gen_snapshot(struct larder *l) {
    if (!l) return 0;
    spin_lock(&l->lock);
    u64 g = l->gen;
    spin_unlock(&l->lock);
    return g;
}

// Choose the install slot for `qid_path`: an existing entry (overwrite), else a
// free slot, else the LRU victim. Caller holds l->lock. Never returns NULL.
static struct larder_attr_ent *attr_install_slot_locked(struct larder *l,
                                                        u64 qid_path) {
    struct larder_attr_ent *free_slot = NULL;
    struct larder_attr_ent *lru_victim = NULL;
    for (u32 i = 0; i < LARDER_ATTR_ENTRIES; i++) {
        struct larder_attr_ent *e = &l->attr[i];
        if (e->valid && e->qid_path == qid_path)
            return e;                          // overwrite (revalidate-by-overwrite)
        if (!e->valid) {
            if (!free_slot) free_slot = e;     // remember the first free slot
        } else if (!lru_victim || e->lru < lru_victim->lru) {
            lru_victim = e;                    // track the least-recently-used
        }
    }
    if (free_slot) return free_slot;
    l->attr_evictions++;
    return lru_victim;                         // full: evict LRU (always non-NULL here)
}

void larder_attr_install(struct larder *l, u64 seq0, u64 qid_path,
                         u32 cvers, const struct t_stat *attr) {
    if (!l || !attr) return;
    spin_lock(&l->lock);
    // The gen guard: an invalidate raced the RPC since the seq0 snapshot -> the
    // fetched value may be stale relative to that invalidate; skip the install
    // (the resurrection close, larder.h note (2)).
    if (l->gen != seq0) {
        l->attr_install_skips++;
        spin_unlock(&l->lock);
        return;
    }
    struct larder_attr_ent *e = attr_install_slot_locked(l, qid_path);
    e->qid_path = qid_path;
    e->cvers    = cvers;
    e->valid    = true;
    e->lru      = ++l->lru_clock;
    e->attr     = *attr;
    l->attr_installs++;
    spin_unlock(&l->lock);
}

void larder_attr_invalidate(struct larder *l, u64 qid_path) {
    if (!l) return;
    spin_lock(&l->lock);
    // Bump gen ALWAYS (the file was mutated even if it was not cached -- a
    // concurrent populate that fetched the pre-mutation value must be skipped).
    l->gen++;
    struct larder_attr_ent *e = attr_find_locked(l, qid_path);
    if (e) {
        e->valid = false;
        l->attr_invalidations++;
    }
    spin_unlock(&l->lock);
}
