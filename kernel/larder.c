// The Larder -- the guest-side 9P FS cache. L1c: the substrate + the attr
// sub-cache. See <thylacine/larder.h> for the design + the fs_cache.tla mapping;
// docs/LARDER-DESIGN.md for the full scripture; ARCH section 28 I-38.

#include <thylacine/larder.h>
#include "../mm/slub.h"   // kmalloc / kfree for the L1e page-buffer pool

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

// -- L1d: the dentry sub-cache --------------------------------------------------

// Byte-exact component compare (the kernel has no memcmp; names are short + not
// NUL-terminated -- name_len defines the extent).
static bool larder_name_eq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i]) return false;
    return true;
}

// Find the dentry (parent_qid_path, name[0..name_len)), or NULL. Caller holds
// l->lock. A too-long name is never cached, so it can never match.
static struct larder_dentry_ent *dentry_find_locked(struct larder *l,
                                                    u64 parent_qid_path,
                                                    const char *name,
                                                    size_t name_len) {
    if (name_len == 0 || name_len > LARDER_DENTRY_NAME_MAX) return NULL;
    for (u32 i = 0; i < LARDER_DENTRY_ENTRIES; i++) {
        struct larder_dentry_ent *e = &l->dentry[i];
        if (e->valid && e->parent_qid_path == parent_qid_path &&
            e->name_len == (u32)name_len && larder_name_eq(e->name, name, name_len))
            return e;
    }
    return NULL;
}

bool larder_walk_serve(struct larder *l, u64 base_qid_path,
                       const char *const *names, const size_t *name_lens,
                       int nname, struct t_stat *sts,
                       int *nresolved, bool *is_miss) {
    if (!l || !names || !name_lens || !sts || !nresolved || !is_miss) return false;
    if (nname <= 0) return false;
    spin_lock(&l->lock);
    u64 cur = base_qid_path;
    for (int i = 0; i < nname; i++) {
        struct larder_dentry_ent *d = dentry_find_locked(l, cur, names[i], name_lens[i]);
        if (!d) {
            // No cached name-binding for this hop -> the RPC must resolve it.
            l->dentry_misses++;
            spin_unlock(&l->lock);
            return false;
        }
        if (d->negative) {
            // Cached miss: the walk fails at component i (name[i] absent in cur).
            // Components [0..i) already filled sts; the caller returns a partial
            // walk. The resolver re-applies the parent X-check on the served
            // prefix, so the negative serve preserves the fail-ordering.
            d->lru = ++l->lru_clock;
            l->dentry_neg_hits++;
            *nresolved = i;
            *is_miss   = true;
            spin_unlock(&l->lock);
            return true;
        }
        // Positive: the walk reply's per-component attr comes from the attr
        // sub-cache (a miss there bails to the RPC, so a served qid always has a
        // coherent attr). Copy it out UNDER the lock -- no torn serve.
        struct larder_attr_ent *a = attr_find_locked(l, d->child_qid_path);
        if (!a) {
            l->dentry_misses++;
            spin_unlock(&l->lock);
            return false;
        }
        sts[i] = a->attr;
        a->lru = ++l->lru_clock;
        d->lru = ++l->lru_clock;
        l->dentry_hits++;
        cur = d->child_qid_path;
    }
    // Full positive run.
    *nresolved = nname;
    *is_miss   = false;
    spin_unlock(&l->lock);
    return true;
}

// Choose the install slot for (parent_qid_path, name): an existing entry
// (overwrite), else a free slot, else the LRU victim. Caller holds l->lock.
// Never returns NULL.
static struct larder_dentry_ent *dentry_install_slot_locked(struct larder *l,
                                                            u64 parent_qid_path,
                                                            const char *name,
                                                            size_t name_len) {
    struct larder_dentry_ent *free_slot = NULL;
    struct larder_dentry_ent *lru_victim = NULL;
    for (u32 i = 0; i < LARDER_DENTRY_ENTRIES; i++) {
        struct larder_dentry_ent *e = &l->dentry[i];
        if (e->valid && e->parent_qid_path == parent_qid_path &&
            e->name_len == (u32)name_len && larder_name_eq(e->name, name, name_len))
            return e;                          // overwrite
        if (!e->valid) {
            if (!free_slot) free_slot = e;
        } else if (!lru_victim || e->lru < lru_victim->lru) {
            lru_victim = e;
        }
    }
    if (free_slot) return free_slot;
    l->dentry_evictions++;
    return lru_victim;                         // full: evict LRU (always non-NULL)
}

void larder_dentry_install(struct larder *l, u64 seq0, u64 parent_qid_path,
                           const char *name, size_t name_len,
                           u64 child_qid_path, bool negative) {
    if (!l || !name) return;
    if (name_len == 0 || name_len > LARDER_DENTRY_NAME_MAX) return;  // not cacheable
    spin_lock(&l->lock);
    // The gen guard (same as the attr install): an invalidate raced the RPC since
    // the seq0 snapshot -> skip (the pre-mutation listing may be stale).
    if (l->gen != seq0) {
        l->dentry_install_skips++;
        spin_unlock(&l->lock);
        return;
    }
    struct larder_dentry_ent *e =
        dentry_install_slot_locked(l, parent_qid_path, name, name_len);
    e->parent_qid_path = parent_qid_path;
    e->child_qid_path  = negative ? 0 : child_qid_path;
    e->name_len        = (u32)name_len;
    e->valid           = true;
    e->negative        = negative;
    e->lru             = ++l->lru_clock;
    for (size_t i = 0; i < name_len; i++) e->name[i] = name[i];
    l->dentry_installs++;
    spin_unlock(&l->lock);
}

void larder_dentry_invalidate_parent(struct larder *l, u64 parent_qid_path) {
    if (!l) return;
    spin_lock(&l->lock);
    // Bump gen ALWAYS (a concurrent walk populate that fetched the pre-mutation
    // dirent listing must be skipped -- the same guard as the attr path).
    l->gen++;
    for (u32 i = 0; i < LARDER_DENTRY_ENTRIES; i++) {
        struct larder_dentry_ent *e = &l->dentry[i];
        if (e->valid && e->parent_qid_path == parent_qid_path) {
            e->valid = false;
            l->dentry_invalidations++;
        }
    }
    spin_unlock(&l->lock);
}

// -- L1e: the page sub-cache ----------------------------------------------------

// Copy `n` bytes (no freestanding memcpy -- the loom_bufcopy precedent, widened to
// 8-byte words for the up-to-4 KiB page copy). Word-copy only when both ends are
// 8-aligned (the page-aligned read fast path -- Go's .a reads); byte-copy otherwise
// (unaligned: UBSan-clean, correctness over speed). Both ends are kernel memory.
static void larder_pagecopy(u8 *dst, const u8 *src, u32 n) {
    if ((((u64)dst | (u64)src) & 7u) == 0) {
        u32 i = 0;
        for (; i + 8 <= n; i += 8) *(u64 *)(dst + i) = *(const u64 *)(src + i);
        for (; i < n; i++) dst[i] = src[i];
    } else {
        for (u32 i = 0; i < n; i++) dst[i] = src[i];
    }
}

// Find the page slot (qid_path, page_index) (valid), or NULL. Caller holds l->lock.
static struct larder_page_ent *page_find_locked(struct larder *l, u64 qid_path,
                                                u64 page_index) {
    for (u32 i = 0; i < LARDER_PAGE_ENTRIES; i++) {
        struct larder_page_ent *e = &l->page[i];
        if (e->valid && e->qid_path == qid_path && e->page_index == page_index)
            return e;
    }
    return NULL;
}

u32 larder_page_serve(struct larder *l, u64 qid_path, u64 page_index,
                      u32 page_off, u32 want, u32 want_cvers,
                      u8 *out, u64 *seq0_out) {
    if (!l || !out || want == 0) return 0;
    spin_lock(&l->lock);
    struct larder_page_ent *e = page_find_locked(l, qid_path, page_index);
    // Serve only a fresh (cvers-matching), in-range slot. A cvers mismatch is a
    // cross-open external write (close-to-open); page_off >= valid_len is past the
    // page's known content (the caller's next read, at valid_len, refetches -- so
    // a partial page needs no EOF determination). Copy [0, chunk) out UNDER the
    // lock so a concurrent invalidate/evict cannot free `page` mid-copy.
    if (e && e->cvers == want_cvers && page_off < e->valid_len) {
        u32 avail = e->valid_len - page_off;
        u32 chunk = (want < avail) ? want : avail;
        larder_pagecopy(out, e->page + page_off, chunk);
        e->lru = ++l->lru_clock;
        l->page_hits++;
        spin_unlock(&l->lock);
        return chunk;
    }
    // Miss: hand back the gen snapshot for the caller's install guard (a stale slot
    // is left in place -- the subsequent install for the same key overwrites it).
    if (seq0_out) *seq0_out = l->gen;
    l->page_misses++;
    spin_unlock(&l->lock);
    return 0;
}

// Choose the install slot for (qid_path, page_index): an existing entry
// (overwrite), else a free slot, else the LRU victim (its retained buffer is
// reused). Caller holds l->lock. Never returns NULL.
static struct larder_page_ent *page_install_slot_locked(struct larder *l,
                                                        u64 qid_path,
                                                        u64 page_index) {
    struct larder_page_ent *free_slot = NULL;
    struct larder_page_ent *lru_victim = NULL;
    for (u32 i = 0; i < LARDER_PAGE_ENTRIES; i++) {
        struct larder_page_ent *e = &l->page[i];
        if (e->valid && e->qid_path == qid_path && e->page_index == page_index)
            return e;                          // overwrite (revalidate-by-overwrite)
        if (!e->valid) {
            if (!free_slot) free_slot = e;     // prefer a free slot (may hold a reusable buffer)
        } else if (!lru_victim || e->lru < lru_victim->lru) {
            lru_victim = e;
        }
    }
    if (free_slot) return free_slot;
    l->page_evictions++;
    return lru_victim;                         // full: evict LRU (always non-NULL)
}

void larder_page_install(struct larder *l, u64 seq0, u64 qid_path, u64 page_index,
                         u32 cvers, const u8 *data, u32 len) {
    if (!l || !data) return;
    if (len > LARDER_PAGE_SIZE) len = LARDER_PAGE_SIZE;
    spin_lock(&l->lock);
    // The gen guard (same as attr/dentry): an invalidate raced the read since the
    // seq0 snapshot -> the read may have seen the pre-mutation content; skip the
    // install (the populate-after-invalidate resurrection close, larder.h note).
    if (l->gen != seq0) {
        l->page_install_skips++;
        spin_unlock(&l->lock);
        return;
    }
    struct larder_page_ent *e = page_install_slot_locked(l, qid_path, page_index);
    // Lazily allocate the slot's 4 KiB buffer (retained + reused across evictions).
    // kmalloc is non-blocking (buddy zone->lock, a leaf below l->lock -- the pinned
    // larder -> buddy order), so it is safe under the spinlock. A failure skips the
    // install (best-effort; the RPC already served the bytes -- I-38 never depends
    // on a fill). Only a cacheable client reaches here, so netd allocates no pages.
    if (!e->page) {
        e->page = kmalloc(LARDER_PAGE_SIZE, 0);
        if (!e->page) {
            spin_unlock(&l->lock);
            return;
        }
    }
    larder_pagecopy(e->page, data, len);
    e->qid_path   = qid_path;
    e->page_index = page_index;
    e->cvers      = cvers;
    e->valid_len  = len;
    e->valid      = true;
    e->lru        = ++l->lru_clock;
    l->page_installs++;
    spin_unlock(&l->lock);
}

void larder_page_invalidate(struct larder *l, u64 qid_path) {
    if (!l) return;
    spin_lock(&l->lock);
    // Bump gen ALWAYS (a concurrent populate that read the pre-write content must
    // be skipped -- the same guard as the attr/dentry paths).
    l->gen++;
    for (u32 i = 0; i < LARDER_PAGE_ENTRIES; i++) {
        struct larder_page_ent *e = &l->page[i];
        if (e->valid && e->qid_path == qid_path) {
            e->valid = false;   // buffer retained for reuse
            l->page_invalidations++;
        }
    }
    spin_unlock(&l->lock);
}

void larder_destroy(struct larder *l) {
    if (!l) return;
    spin_lock(&l->lock);
    for (u32 i = 0; i < LARDER_PAGE_ENTRIES; i++) {
        if (l->page[i].page) {
            kfree(l->page[i].page);
            l->page[i].page  = NULL;
            l->page[i].valid = false;
        }
    }
    spin_unlock(&l->lock);
}
