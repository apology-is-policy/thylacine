// The Larder -- the guest-side 9P FS cache. L1c: the substrate + the attr
// sub-cache. See <thylacine/larder.h> for the design + the fs_cache.tla mapping;
// docs/LARDER-DESIGN.md for the full scripture; ARCH section 28 I-38.

#include <thylacine/larder.h>
#include <thylacine/page.h>     // KP_ZERO for the lazy page-entry array
#include "../mm/slub.h"   // kmalloc / kfree for the L1e page-buffer pool



void larder_init(struct larder *l) {
    if (!l) return;
    // Self-sufficient zero (valid=false / gen=0 / heap ptrs NULL / diagnostics=0);
    // the kernel has no memset, so a byte loop, like dev9p's t_stat zero.
    for (size_t i = 0; i < sizeof(*l); i++) ((u8 *)l)[i] = 0;
    spin_lock_init(&l->lock);
}

// All three sub-caches share the O(1) index shape (the task-#25 page-cache
// pattern, generalized at the FID-LIFECYCLE re-size): a HEAP entry array
// (lazily allocated on the first install -- a non-cacheable client allocates
// none), a chained hash (pow2 buckets ~= 2*cap), a free-cursor for the fill
// phase, and CLOCK (second-chance) eviction once full. The coherence contract
// (gen guard / own-write invalidate / serve-copy-under-lock) is UNCHANGED --
// only the index + eviction mechanics differ from the L1c/L1d linear arrays.

// -- G4: the qid-scoped populate guard (larder.h "LARDER_INVAL_RING") ----------
//
// Every invalidation event bumps gen and logs the staled qid; an install skips
// iff ITS key was named in its (seq0, gen] window. Both run under l->lock.

static void inval_log_locked(struct larder *l, u64 qid_path, bool hard) {
    l->gen++;
    l->inval_qid[l->gen % LARDER_INVAL_RING]  = qid_path;
    l->inval_hard[l->gen % LARDER_INVAL_RING] = hard ? 1 : 0;
}

// Was `qid_path` staled by any event in (seq0, l->gen]? Fail-safe TRUE when the
// window exceeds the ring (evidence lost -> the pre-G4 global-skip behavior).
// Ring soundness: for n <= RING, slot s % RING (s in the window) was last
// written by seq s itself -- a later same-residue writer would exceed gen.
static bool inval_hits_locked(struct larder *l, u64 seq0, u64 qid_path) {
    u64 n = l->gen - seq0;
    if (n == 0) return false;
    if (n > LARDER_INVAL_RING) return true;
    for (u64 s = seq0 + 1; s <= l->gen; s++) {
        if (l->inval_qid[s % LARDER_INVAL_RING] == qid_path) return true;
    }
    l->inval_scope_passes++;   // the global gen would have skipped this fill
    return false;
}

// Fibonacci-hash mix of a two-part key into a pow2 bucket.
static inline u32 larder_bucket2(u32 nbuckets, u64 a, u64 b) {
    u64 h = (a + 0x9E3779B97F4A7C15ull) * 0xBF58476D1CE4E5B9ull;
    h ^= (b + 0x94D049BB133111EBull);
    h *= 0xBF58476D1CE4E5B9ull;
    return (u32)((h >> 33) & (u64)(nbuckets - 1u));
}

static inline u32 attr_bucket(u32 nbuckets, u64 qid) {
    return larder_bucket2(nbuckets, qid, 0);
}

static void attr_hash_link_locked(struct larder *l, s32 si) {
    struct larder_attr_ent *e = &l->attr[si];
    u32 b = attr_bucket(l->attr_nbuckets, e->qid_path);
    e->hnext = l->attr_hash[b];
    l->attr_hash[b] = si;
}

static void attr_hash_unlink_locked(struct larder *l, s32 si) {
    struct larder_attr_ent *e = &l->attr[si];
    u32 b = attr_bucket(l->attr_nbuckets, e->qid_path);
    s32 *pp = &l->attr_hash[b];
    while (*pp != -1) {
        if (*pp == si) { *pp = e->hnext; e->hnext = -1; return; }
        pp = &l->attr[*pp].hnext;
    }
    e->hnext = -1;
}

static bool attr_ensure_array_locked(struct larder *l) {
    if (l->attr) return true;
    u32 cap = LARDER_ATTR_ENTRIES;
    u32 nb = 1u; while (nb < cap * 2u) nb <<= 1;
    struct larder_attr_ent *arr = kmalloc((size_t)cap * sizeof(*arr), KP_ZERO);
    if (!arr) return false;
    s32 *hh = kmalloc((size_t)nb * sizeof(s32), 0);
    if (!hh) { kfree(arr); return false; }
    for (u32 i = 0; i < cap; i++) arr[i].hnext = -1;
    for (u32 i = 0; i < nb; i++)  hh[i] = -1;
    l->attr = arr; l->attr_cap = cap;
    l->attr_hash = hh; l->attr_nbuckets = nb;
    l->attr_used = 0; l->attr_hand = 0;
    return true;
}

// Find the VALID slot holding `qid_path`, or NULL. Caller holds l->lock.
static struct larder_attr_ent *attr_find_locked(struct larder *l, u64 qid_path) {
    if (!l->attr) return NULL;
    u32 b = attr_bucket(l->attr_nbuckets, qid_path);
    for (s32 si = l->attr_hash[b]; si != -1; si = l->attr[si].hnext) {
        struct larder_attr_ent *e = &l->attr[si];
        if (e->valid && e->qid_path == qid_path)
            return e;
    }
    return NULL;
}

static s32 attr_clock_evict_locked(struct larder *l) {
    for (u32 spin = 0; spin < 2u * l->attr_cap; spin++) {
        u32 h = l->attr_hand;
        l->attr_hand = (h + 1u) % l->attr_cap;
        struct larder_attr_ent *e = &l->attr[h];
        if (!e->valid) return (s32)h;
        if (e->ref) { e->ref = false; continue; }
        return (s32)h;
    }
    return (s32)l->attr_hand;
}

bool larder_attr_serve(struct larder *l, u64 qid_path,
                       struct t_stat *out, u64 *seq0_out) {
    if (!l || !out) return false;
    spin_lock(&l->lock);
    struct larder_attr_ent *e = attr_find_locked(l, qid_path);
    // G3: a perm_only entry misses here -- the stat consumers read size/times,
    // exactly the fields the downgrade staled. The getattr this miss triggers
    // re-installs full (the upgrade).
    if (e && !e->perm_only) {
        // Read: copy the whole entry out UNDER the lock (a concurrent invalidate
        // cannot tear it). CLOCK: touched.
        *out = e->attr;
        e->ref = true;
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

bool larder_attr_fresh_size(struct larder *l, u64 qid_path, u32 cvers,
                            u64 *size_out) {
    if (!l || !size_out) return false;
    spin_lock(&l->lock);
    struct larder_attr_ent *e = attr_find_locked(l, qid_path);
    // Freshness gate: cvers must match the reading fid's open-time qid.vers
    // (the page-serve rule -- larder.h). A stale/newer entry is a miss; a
    // perm_only entry too (its size is what the G3 downgrade staled).
    if (e && !e->perm_only && e->cvers == cvers) {
        *size_out = e->attr.size;
        e->ref = true;              // CLOCK: touched
        spin_unlock(&l->lock);
        return true;
    }
    spin_unlock(&l->lock);
    return false;
}

// Choose the install slot for `qid_path`: an existing entry (overwrite), else
// the free-cursor, else the CLOCK victim (unlinked from its old bucket, linked
// under the new key). Caller holds l->lock; the array exists. Never NULL.
static struct larder_attr_ent *attr_install_slot_locked(struct larder *l,
                                                        u64 qid_path) {
    struct larder_attr_ent *e = attr_find_locked(l, qid_path);
    if (e) return e;                           // overwrite (revalidate-by-overwrite)
    s32 si;
    if (l->attr_used < l->attr_cap) {
        si = (s32)l->attr_used++;              // fresh slot (KP_ZERO'd, hnext = -1)
    } else {
        si = attr_clock_evict_locked(l);
        if (l->attr[si].valid) l->attr_evictions++;
        attr_hash_unlink_locked(l, si);
    }
    struct larder_attr_ent *ne = &l->attr[si];
    ne->valid = false;
    ne->qid_path = qid_path;
    attr_hash_link_locked(l, si);
    return ne;
}

void larder_attr_install(struct larder *l, u64 seq0, u64 qid_path,
                         u32 cvers, const struct t_stat *attr) {
    if (!l || !attr) return;
    spin_lock(&l->lock);
    // The gen guard, G4 qid-scoped: skip only if an invalidation event named
    // THIS qid since the seq0 snapshot -> the fetched value may be stale
    // relative to that event (the resurrection close, larder.h note (2)). An
    // unrelated qid's event no longer discards this fill.
    if (inval_hits_locked(l, seq0, qid_path)) {
        l->attr_install_skips++;
        spin_unlock(&l->lock);
        return;
    }
    // Lazy heap array (first install; OOM skips -- best-effort, I-38 never
    // depends on a fill; re-attempted next install, the task-#25 F1 posture).
    if (!attr_ensure_array_locked(l)) {
        spin_unlock(&l->lock);
        return;
    }
    struct larder_attr_ent *e = attr_install_slot_locked(l, qid_path);
    e->cvers     = cvers;
    e->valid     = true;
    e->ref       = true;
    e->perm_only = false;      // a full fresh record upgrades a G3 downgrade
    e->attr      = *attr;
    l->attr_installs++;
    spin_unlock(&l->lock);
}

void larder_attr_invalidate(struct larder *l, u64 qid_path) {
    if (!l) return;
    spin_lock(&l->lock);
    // Log the event ALWAYS (the file was mutated even if it was not cached -- a
    // concurrent populate of THIS qid that fetched the pre-mutation value must
    // be skipped).
    inval_log_locked(l, qid_path, /*hard=*/true);
    struct larder_attr_ent *e = attr_find_locked(l, qid_path);
    if (e) {
        attr_hash_unlink_locked(l, (s32)(e - l->attr));   // drop from its bucket
        e->valid = false;
        l->attr_invalidations++;
    }
    spin_unlock(&l->lock);
}

void larder_attr_downgrade(struct larder *l, u64 qid_path) {
    if (!l) return;
    spin_lock(&l->lock);
    // An invalidation to the gen guard (a concurrent FULL populate of this dir
    // that observed pre-mutation times must be skipped) -- only the entry's
    // perm-servable core (mode/uid/gid + the immutable qid identity) survives,
    // and only the resolver's intermediate-hop X-check may read it (larder.h).
    inval_log_locked(l, qid_path, /*hard=*/false);
    struct larder_attr_ent *e = attr_find_locked(l, qid_path);
    if (e && !e->perm_only) {
        e->perm_only = true;
        l->attr_downgrades++;
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

// The dentry key is (parent_qid_path, name bytes): fold the name through FNV-1a
// and mix with the parent for the bucket; the chain walk full-compares.
static u64 dentry_name_hash(const char *name, size_t n) {
    u64 h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= (u8)name[i]; h *= 0x100000001b3ull; }
    return h;
}

static void dentry_hash_link_locked(struct larder *l, s32 si) {
    struct larder_dentry_ent *e = &l->dentry[si];
    u32 b = larder_bucket2(l->dentry_nbuckets, e->parent_qid_path,
                           dentry_name_hash(e->name, e->name_len));
    e->hnext = l->dentry_hash[b];
    l->dentry_hash[b] = si;
}

static void dentry_hash_unlink_locked(struct larder *l, s32 si) {
    struct larder_dentry_ent *e = &l->dentry[si];
    u32 b = larder_bucket2(l->dentry_nbuckets, e->parent_qid_path,
                           dentry_name_hash(e->name, e->name_len));
    s32 *pp = &l->dentry_hash[b];
    while (*pp != -1) {
        if (*pp == si) { *pp = e->hnext; e->hnext = -1; return; }
        pp = &l->dentry[*pp].hnext;
    }
    e->hnext = -1;
}

static bool dentry_ensure_array_locked(struct larder *l) {
    if (l->dentry) return true;
    u32 cap = LARDER_DENTRY_ENTRIES;
    u32 nb = 1u; while (nb < cap * 2u) nb <<= 1;
    struct larder_dentry_ent *arr = kmalloc((size_t)cap * sizeof(*arr), KP_ZERO);
    if (!arr) return false;
    s32 *hh = kmalloc((size_t)nb * sizeof(s32), 0);
    if (!hh) { kfree(arr); return false; }
    for (u32 i = 0; i < cap; i++) arr[i].hnext = -1;
    for (u32 i = 0; i < nb; i++)  hh[i] = -1;
    l->dentry = arr; l->dentry_cap = cap;
    l->dentry_hash = hh; l->dentry_nbuckets = nb;
    l->dentry_used = 0; l->dentry_hand = 0;
    return true;
}

// Find the dentry (parent_qid_path, name[0..name_len)), or NULL. Caller holds
// l->lock. A too-long name is never cached, so it can never match.
static struct larder_dentry_ent *dentry_find_locked(struct larder *l,
                                                    u64 parent_qid_path,
                                                    const char *name,
                                                    size_t name_len) {
    if (name_len == 0 || name_len > LARDER_DENTRY_NAME_MAX) return NULL;
    if (!l->dentry) return NULL;
    u32 b = larder_bucket2(l->dentry_nbuckets, parent_qid_path,
                           dentry_name_hash(name, name_len));
    for (s32 si = l->dentry_hash[b]; si != -1; si = l->dentry[si].hnext) {
        struct larder_dentry_ent *e = &l->dentry[si];
        if (e->valid && e->parent_qid_path == parent_qid_path &&
            e->name_len == (u32)name_len && larder_name_eq(e->name, name, name_len))
            return e;
    }
    return NULL;
}

static s32 dentry_clock_evict_locked(struct larder *l) {
    for (u32 spin = 0; spin < 2u * l->dentry_cap; spin++) {
        u32 h = l->dentry_hand;
        l->dentry_hand = (h + 1u) % l->dentry_cap;
        struct larder_dentry_ent *e = &l->dentry[h];
        if (!e->valid) return (s32)h;
        if (e->ref) { e->ref = false; continue; }
        return (s32)h;
    }
    return (s32)l->dentry_hand;
}

bool larder_walk_serve(struct larder *l, u64 base_qid_path,
                       const char *const *names, const size_t *name_lens,
                       int nname, struct t_stat *sts,
                       int *nresolved, bool *is_miss, bool *leaf_perm_only) {
    if (!l || !names || !name_lens || !sts || !nresolved || !is_miss) return false;
    if (nname <= 0) return false;
    if (leaf_perm_only) *leaf_perm_only = false;
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
            d->ref = true;
            l->dentry_neg_hits++;
            *nresolved = i;
            *is_miss   = true;
            spin_unlock(&l->lock);
            return true;
        }
        // Positive: the walk reply's per-component attr comes from the attr
        // sub-cache (a miss there bails to the RPC, so a served qid always has a
        // coherent attr). Copy it out UNDER the lock -- no torn serve.
        // G3: a perm_only attr serves an INTERMEDIATE hop -- the resolver reads
        // only mode/uid/gid (the X-check) + the immutable qid fields there --
        // but NOT the final hop of the run: the LEAF record feeds STALK_STAT's
        // returned stat, the carried-attrs chain, and the cached-open
        // size/cvers gates, all of which read the fields the downgrade staled.
        // G2: with a non-NULL leaf_perm_only out, a perm_only LEAF serves and
        // is REPORTED instead of bailed -- only the bind-form dir-fid consume
        // (which reads mode/uid/gid + qid, all fresh) may accept; every other
        // caller treats the flag as a miss.
        struct larder_attr_ent *a = attr_find_locked(l, d->child_qid_path);
        if (!a || (a->perm_only && i == nname - 1 && !leaf_perm_only)) {
            l->dentry_misses++;
            spin_unlock(&l->lock);
            return false;
        }
        if (a->perm_only && i == nname - 1)
            *leaf_perm_only = true;
        sts[i] = a->attr;
        a->ref = true;
        d->ref = true;
        l->dentry_hits++;
        cur = d->child_qid_path;
    }
    // Full positive run.
    *nresolved = nname;
    *is_miss   = false;
    spin_unlock(&l->lock);
    return true;
}

bool larder_dentry_lookup(struct larder *l, u64 parent_qid_path,
                          const char *name, size_t name_len, u64 *child_out) {
    if (!l || !name || !child_out) return false;
    spin_lock(&l->lock);
    struct larder_dentry_ent *d =
        dentry_find_locked(l, parent_qid_path, name, name_len);
    if (!d || d->negative) {
        spin_unlock(&l->lock);
        return false;
    }
    *child_out = d->child_qid_path;
    spin_unlock(&l->lock);
    return true;
}

// HARD-event-only form of inval_hits_locked (the donate gate; see larder.h).
static bool inval_hard_hits_locked(struct larder *l, u64 seq0, u64 qid_path) {
    u64 n = l->gen - seq0;
    if (n == 0) return false;
    if (n > LARDER_INVAL_RING) return true;
    for (u64 s = seq0 + 1; s <= l->gen; s++) {
        u64 slot = s % LARDER_INVAL_RING;
        if (l->inval_hard[slot] && l->inval_qid[slot] == qid_path) return true;
    }
    return false;
}

bool larder_qid_staled_since(struct larder *l, u64 seq0, u64 qid_path) {
    if (!l) return true;   // no larder -> no evidence -> fail-safe stale
    spin_lock(&l->lock);
    bool staled = inval_hard_hits_locked(l, seq0, qid_path);
    spin_unlock(&l->lock);
    return staled;
}

// Choose the install slot for (parent_qid_path, name): an existing entry
// (overwrite), else the free-cursor, else the CLOCK victim (unlinked from its
// old bucket; the caller sets the key fields BEFORE we re-link). Caller holds
// l->lock; the array exists. Never returns NULL.
static struct larder_dentry_ent *dentry_install_slot_locked(struct larder *l,
                                                            u64 parent_qid_path,
                                                            const char *name,
                                                            size_t name_len) {
    struct larder_dentry_ent *e =
        dentry_find_locked(l, parent_qid_path, name, name_len);
    if (e) return e;                           // overwrite (same key/bucket kept)
    s32 si;
    if (l->dentry_used < l->dentry_cap) {
        si = (s32)l->dentry_used++;            // fresh slot (KP_ZERO'd, hnext = -1)
    } else {
        si = dentry_clock_evict_locked(l);
        if (l->dentry[si].valid) l->dentry_evictions++;
        dentry_hash_unlink_locked(l, si);      // drop the OLD (parent,name) key
    }
    struct larder_dentry_ent *ne = &l->dentry[si];
    ne->valid = false;
    ne->parent_qid_path = parent_qid_path;
    ne->name_len = (u32)name_len;
    for (size_t i = 0; i < name_len; i++) ne->name[i] = name[i];
    dentry_hash_link_locked(l, si);            // link under the NEW key
    return ne;
}

void larder_dentry_install(struct larder *l, u64 seq0, u64 parent_qid_path,
                           const char *name, size_t name_len,
                           u64 child_qid_path, bool negative) {
    if (!l || !name) return;
    if (name_len == 0 || name_len > LARDER_DENTRY_NAME_MAX) return;  // not cacheable
    spin_lock(&l->lock);
    // The gen guard (same as the attr install), G4-scoped to the PARENT qid:
    // every event that stales a (parent, name) binding -- a create/rename/
    // unlink's dentry invalidate, and the parent-attr invalidate/downgrade the
    // same mutation issues -- logs the parent, so a raced install of this
    // parent's chain skips while an unrelated file's event no longer does.
    if (inval_hits_locked(l, seq0, parent_qid_path)) {
        l->dentry_install_skips++;
        spin_unlock(&l->lock);
        return;
    }
    // Lazy heap array (first install; OOM skips -- best-effort).
    if (!dentry_ensure_array_locked(l)) {
        spin_unlock(&l->lock);
        return;
    }
    struct larder_dentry_ent *e =
        dentry_install_slot_locked(l, parent_qid_path, name, name_len);
    // Key fields (parent/name) were set by the slot chooser before linking.
    e->child_qid_path  = negative ? 0 : child_qid_path;
    e->valid           = true;
    e->negative        = negative;
    e->ref             = true;
    l->dentry_installs++;
    spin_unlock(&l->lock);
}

void larder_dentry_invalidate_name(struct larder *l, u64 parent_qid_path,
                                   const char *name, size_t name_len) {
    if (!l) return;
    spin_lock(&l->lock);
    // Log the event ALWAYS, keyed by the PARENT (a concurrent walk populate of
    // this parent's chain that snapshotted gen before this mutation must be
    // skipped by the install guard -- even when the mutated name was not
    // itself cached; the same resurrection-close as the attr path).
    inval_log_locked(l, parent_qid_path, /*hard=*/false);
    // A single-name dir mutation (create/rename/unlink of exactly `name`) stales
    // ONLY the (parent, name) binding: the negative dentry a create fills, the
    // positive one an unlink empties, or a rename endpoint. Siblings under the
    // same parent are untouched -- creating "foo" does not change whether "bar"
    // exists (dentries are per-(parent,name) existence, populated from walks, not
    // dir listings), so a whole-parent drop was an over-broad superset that
    // forced every sibling to re-walk (the cold-band wga thrash this closes).
    // Name-specific matches fs_cache.tla's per-token OwnWrite(f), and it is O(1)
    // via the same (parent,name) hash the serve uses -- retiring the whole-parent
    // O(dentry_cap) scan and its task-#30 secondary-index seam in one move.
    if (l->dentry && name && name_len && name_len <= LARDER_DENTRY_NAME_MAX) {
        struct larder_dentry_ent *e =
            dentry_find_locked(l, parent_qid_path, name, name_len);
        if (e) {
            dentry_hash_unlink_locked(l, (s32)(e - l->dentry));
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

// The L1e page cache is O(1)-per-op at scale (task #25): a chained hash keyed
// (qid_path, page_index) gives O(1) find; a free-cursor fills fresh slots O(1);
// a CLOCK (second-chance) hand evicts O(1) amortized once full. Only invalidate is
// O(page_cap) (an own-write is rare on the read-dominated hot path). All state is
// under l->lock. The coherence contract (gen-guard / cvers / own-write invalidate /
// serve-copy-under-lock) is UNCHANGED from the O(N) L1e -- only the index changes.

static inline u32 page_bucket(u32 nbuckets, u64 qid, u64 idx) {
    // Fibonacci-hash mix of the 128-bit key into a pow2 bucket. nbuckets is pow2.
    u64 h = (qid + 0x9E3779B97F4A7C15ull) * 0xBF58476D1CE4E5B9ull;
    h ^= (idx + 0x94D049BB133111EBull);
    h *= 0xBF58476D1CE4E5B9ull;
    return (u32)((h >> 33) & (u64)(nbuckets - 1u));
}

// Link slot `si` into its (already-set) key's bucket. Caller holds l->lock.
static void page_hash_link_locked(struct larder *l, s32 si) {
    struct larder_page_ent *e = &l->page[si];
    u32 b = page_bucket(l->page_nbuckets, e->qid_path, e->page_index);
    e->hnext = l->page_hash[b];
    l->page_hash[b] = si;
}

// Remove slot `si` from its key's bucket (walk-unlink). Idempotent-safe: a slot not
// in its bucket leaves hnext = -1. Caller holds l->lock.
static void page_hash_unlink_locked(struct larder *l, s32 si) {
    struct larder_page_ent *e = &l->page[si];
    u32 b = page_bucket(l->page_nbuckets, e->qid_path, e->page_index);
    s32 *pp = &l->page_hash[b];
    while (*pp != -1) {
        if (*pp == si) { *pp = e->hnext; e->hnext = -1; return; }
        pp = &l->page[*pp].hnext;
    }
    e->hnext = -1;
}

// The SECONDARY index (F3): buckets keyed by qid_path ALONE, so every page of one file
// shares a single qbucket -> larder_page_invalidate walks O(pages-of-file), not O(cap).
// A slot is in page_qhash IFF it is in page_hash (the two are linked/unlinked together).
static inline u32 page_qbucket(u32 nbuckets, u64 qid_path) {
    return larder_bucket2(nbuckets, qid_path, 0);
}

static void page_qhash_link_locked(struct larder *l, s32 si) {
    struct larder_page_ent *e = &l->page[si];
    u32 b = page_qbucket(l->page_qnbuckets, e->qid_path);
    e->qnext = l->page_qhash[b];
    l->page_qhash[b] = si;
}

static void page_qhash_unlink_locked(struct larder *l, s32 si) {
    struct larder_page_ent *e = &l->page[si];
    u32 b = page_qbucket(l->page_qnbuckets, e->qid_path);
    s32 *pp = &l->page_qhash[b];
    while (*pp != -1) {
        if (*pp == si) { *pp = e->qnext; e->qnext = -1; return; }
        pp = &l->page[*pp].qnext;
    }
    e->qnext = -1;
}

// Lazily allocate the page-entry array + hash on the first install (a cacheable
// client only) -- page[] is heap so a non-cacheable client (netd) carries neither.
// Returns true if ready, false on OOM (the caller skips the install -- best-effort;
// I-38 never depends on a fill).
static bool page_ensure_array_locked(struct larder *l) {
    if (l->page) return true;
    u32 cap = LARDER_PAGE_ENTRIES;
    u32 nb = 1u; while (nb < cap * 2u) nb <<= 1;    // pow2 >= 2*cap (~50% load)
    struct larder_page_ent *arr = kmalloc((size_t)cap * sizeof(*arr), KP_ZERO);
    if (!arr) return false;
    s32 *hh = kmalloc((size_t)nb * sizeof(s32), 0);
    if (!hh) { kfree(arr); return false; }
    s32 *qh = kmalloc((size_t)nb * sizeof(s32), 0);   // the F3 qid_path-only index
    if (!qh) { kfree(hh); kfree(arr); return false; }
    for (u32 i = 0; i < cap; i++) { arr[i].hnext = -1; arr[i].qnext = -1; }
    for (u32 i = 0; i < nb; i++)  { hh[i] = -1; qh[i] = -1; }   // empty buckets
    l->page = arr; l->page_cap = cap;
    l->page_hash = hh; l->page_nbuckets = nb;
    l->page_qhash = qh; l->page_qnbuckets = nb;
    l->page_used = 0; l->page_hand = 0;
    return true;
}

// O(1) chained find of the VALID slot for (qid_path, page_index), or NULL.
static struct larder_page_ent *page_find_locked(struct larder *l, u64 qid_path,
                                                u64 page_index) {
    if (!l->page) return NULL;
    u32 b = page_bucket(l->page_nbuckets, qid_path, page_index);
    for (s32 si = l->page_hash[b]; si != -1; si = l->page[si].hnext) {
        struct larder_page_ent *e = &l->page[si];
        if (e->valid && e->qid_path == qid_path && e->page_index == page_index)
            return e;
    }
    return NULL;
}

// CLOCK second-chance: advance the hand clearing ref bits; the first ref==0 (or an
// already-free slot) is the victim slot index. Terminates within 2*cap. l->lock held.
static s32 page_clock_evict_locked(struct larder *l) {
    for (u32 spin = 0; spin < 2u * l->page_cap; spin++) {
        u32 h = l->page_hand;
        l->page_hand = (h + 1u) % l->page_cap;
        struct larder_page_ent *e = &l->page[h];
        if (!e->valid) return (s32)h;      // a freed (invalidated) slot -- take it
        if (e->ref) { e->ref = false; continue; }
        return (s32)h;                      // ref == 0: the victim
    }
    return (s32)l->page_hand;               // fallback (all ref-set race; take current)
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
    if (e && (e->cvers == want_cvers || e->own) && page_off < e->valid_len) {
        u32 avail = e->valid_len - page_off;
        u32 chunk = (want < avail) ? want : avail;
        larder_pagecopy(out, e->page + page_off, chunk);
        e->ref = true;                 // CLOCK: touched -> second chance on evict
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
    struct larder_page_ent *e = page_find_locked(l, qid_path, page_index);
    if (e) return e;                           // overwrite (revalidate; key/bucket kept)
    s32 si;
    if (l->page_used < l->page_cap) {
        si = (s32)l->page_used++;              // fresh slot (KP_ZERO'd, hnext = -1)
    } else {
        si = page_clock_evict_locked(l);       // full: CLOCK-pick a victim to reuse
        struct larder_page_ent *v = &l->page[si];
        if (v->valid) { l->page_evictions++; }
        page_hash_unlink_locked(l, si);        // drop the victim's OLD key from both
        page_qhash_unlink_locked(l, si);       //   indexes (kept in lockstep)
    }
    struct larder_page_ent *s = &l->page[si];
    s->valid = false;                          // not servable until the caller copies content
    s->qid_path = qid_path;
    s->page_index = page_index;
    page_hash_link_locked(l, si);              // link under the NEW key (both indexes)
    page_qhash_link_locked(l, si);
    return s;
}

void larder_page_install(struct larder *l, u64 seq0, u64 qid_path, u64 page_index,
                         u32 cvers, const u8 *data, u32 len) {
    if (!l || !data) return;
    if (len > LARDER_PAGE_SIZE) len = LARDER_PAGE_SIZE;
    spin_lock(&l->lock);
    // The gen guard (same as attr/dentry), G4-scoped to the file qid: every
    // event that stales this file's content (write-through/flush/OTRUNC page
    // invalidates + the attr invalidate the same mutation issues) logs this
    // qid; an unrelated file's event no longer discards the fill (the
    // measured 726-886 lost installs per S3 window under the global gen).
    if (inval_hits_locked(l, seq0, qid_path)) {
        l->page_install_skips++;
        spin_unlock(&l->lock);
        return;
    }
    // Lazily allocate the heap page-entry array (cacheable client, first install).
    // An OOM here skips the install -- best-effort, like the buffer alloc below.
    if (!page_ensure_array_locked(l)) {
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
            // OOM on a not-yet-valid slot (fresh/reused): unlink it from BOTH indexes
            // (kept in lockstep) so a later install of the same key does not double-link
            // a bucket. Its key is already set; it stays !valid + unlinked (a free slot
            // the CLOCK reuses).
            page_hash_unlink_locked(l, (s32)(e - l->page));
            page_qhash_unlink_locked(l, (s32)(e - l->page));
            spin_unlock(&l->lock);
            return;
        }
    }
    // key (qid_path/page_index) is already set by page_install_slot_locked.
    larder_pagecopy(e->page, data, len);
    e->cvers      = cvers;
    e->valid_len  = len;
    e->valid      = true;
    e->ref        = true;          // CLOCK: freshly installed -> a second chance
    e->own        = false;         // a read-populate observed SERVER content: the
                                   // own bypass upgrades to the normal cvers gate
    l->page_installs++;
    spin_unlock(&l->lock);
}

void larder_page_install_own(struct larder *l, u64 qid_path, u64 page_index,
                             u32 page_off, const u8 *data, u32 len) {
    if (!l || !data || len == 0) return;
    if (page_off >= LARDER_PAGE_SIZE) return;
    if (len > LARDER_PAGE_SIZE - page_off) len = LARDER_PAGE_SIZE - page_off;
    spin_lock(&l->lock);
    if (!page_ensure_array_locked(l)) {
        spin_unlock(&l->lock);
        return;
    }
    if (page_off != 0) {
        // Append-chain continuation ONLY: an existing OWN page ending exactly
        // at our start. A non-own boundary page is left as-is (it serves its
        // prefix; extending it would mix cvers-gated and own content). A gap
        // or overlap is skipped (appends are monotonic; anything else is a
        // shape this API refuses -- the wire serves those reads).
        struct larder_page_ent *e = page_find_locked(l, qid_path, page_index);
        if (e && e->valid && e->own && e->valid_len == page_off) {
            larder_pagecopy(e->page + page_off, data, len);
            e->valid_len = page_off + len;
            e->ref = true;
            l->page_own_installs++;
        }
        spin_unlock(&l->lock);
        return;
    }
    struct larder_page_ent *e = page_install_slot_locked(l, qid_path, page_index);
    if (!e->page) {
        e->page = kmalloc(LARDER_PAGE_SIZE, 0);
        if (!e->page) {
            page_hash_unlink_locked(l, (s32)(e - l->page));
            page_qhash_unlink_locked(l, (s32)(e - l->page));
            spin_unlock(&l->lock);
            return;
        }
    }
    larder_pagecopy(e->page, data, len);
    e->cvers     = 0;
    e->valid_len = len;
    e->valid     = true;
    e->ref       = true;
    e->own       = true;
    l->page_own_installs++;
    spin_unlock(&l->lock);
}

void larder_page_invalidate(struct larder *l, u64 qid_path) {
    if (!l) return;
    spin_lock(&l->lock);
    // Log the event ALWAYS (a concurrent populate of THIS file that read the
    // pre-write content must be skipped -- the same guard as the attr/dentry
    // paths), even if no page array is allocated yet (a non-cacheable /
    // never-installed client).
    inval_log_locked(l, qid_path, /*hard=*/false);
    if (l->page) {
        // Walk ONLY this file's qbucket (every page of qid_path chains here), splicing
        // each matching valid page out of BOTH indexes -- O(pages-of-file + qbucket
        // collisions), never O(page_cap). A !valid same-qid_path slot (a fresh install
        // mid-copy) is left linked: the gen bump above makes its install's seq0 guard
        // skip, and CLOCK reclaims it later (unlinking it from both). A collision from
        // another file (qid_path mismatch) is skipped, staying linked.
        u32 qb = page_qbucket(l->page_qnbuckets, qid_path);
        s32 *pp = &l->page_qhash[qb];
        while (*pp != -1) {
            s32 si = *pp;
            struct larder_page_ent *e = &l->page[si];
            if (e->qid_path == qid_path && e->valid) {
                page_hash_unlink_locked(l, si);   // drop from the (path,index) bucket
                *pp = e->qnext;                   // splice out of the qbucket
                e->qnext = -1;
                e->valid = false;                 // buffer retained for reuse
                l->page_invalidations++;
            } else {
                pp = &e->qnext;                   // keep walking the qbucket
            }
        }
    }
    spin_unlock(&l->lock);
}

void larder_page_invalidate_range(struct larder *l, u64 qid_path,
                                  u64 first_idx, u64 last_idx) {
    if (!l) return;
    spin_lock(&l->lock);
    // Same event discipline as the whole-file form: a concurrent read-populate
    // of THIS file that observed pre-write content anywhere must be guarded
    // out (the range narrows the DROP set, not the guard -- an in-flight read
    // may span the written range).
    inval_log_locked(l, qid_path, /*hard=*/false);
    if (l->page) {
        // The same qbucket walk, dropping only pages whose index lies in
        // [first_idx, last_idx]. Out-of-range pages stay linked AND valid --
        // the G1b range-scoped write-through discipline (larder.h).
        u32 qb = page_qbucket(l->page_qnbuckets, qid_path);
        s32 *pp = &l->page_qhash[qb];
        while (*pp != -1) {
            s32 si = *pp;
            struct larder_page_ent *e = &l->page[si];
            if (e->qid_path == qid_path && e->valid &&
                e->page_index >= first_idx && e->page_index <= last_idx) {
                page_hash_unlink_locked(l, si);
                *pp = e->qnext;
                e->qnext = -1;
                e->valid = false;
                l->page_invalidations++;
            } else {
                pp = &e->qnext;
            }
        }
    }
    spin_unlock(&l->lock);
}

// FID-LIFECYCLE cached-open (docs/FID-LIFECYCLE-DESIGN.md section 3.3): the two
// PURE READERS the fidless open consults. Neither mutates cache state beyond
// the CLOCK ref bit (a coverage consult is a use), so the L1f-audited mutation
// surface (install / evict / invalidate / destroy) is untouched.
//
// Coverage rule: pages [0, ceil(size/PAGE)) of qid_path must ALL be resident at
// `cvers`, each holding content up to min(PAGE, size - i*PAGE). A mid-file page
// whose valid_len is short of the boundary (an msize-clamped populate -- CF-3
// client_max_read_count -- NOT an EOF) FAILS coverage; serving around it would
// be a hole. size == 0 is trivially covered (the empty-file 1-RT open).
static bool pages_cover_locked(struct larder *l, u64 qid_path, u32 cvers,
                               u64 size, u8 *out) {
    u64 npages = (size + LARDER_PAGE_SIZE - 1u) / LARDER_PAGE_SIZE;
    for (u64 i = 0; i < npages; i++) {
        struct larder_page_ent *e = page_find_locked(l, qid_path, i);
        if (!e || (e->cvers != cvers && !e->own)) return false;
        u64 need64 = size - i * LARDER_PAGE_SIZE;
        u32 need   = (need64 >= LARDER_PAGE_SIZE) ? LARDER_PAGE_SIZE : (u32)need64;
        if (e->valid_len < need) return false;
        e->ref = true;
        if (out)
            larder_pagecopy(out + i * LARDER_PAGE_SIZE, e->page, need);
    }
    return true;
}

bool larder_pages_cover(struct larder *l, u64 qid_path, u32 cvers, u64 size) {
    if (!l) return false;
    spin_lock(&l->lock);
    bool ok = pages_cover_locked(l, qid_path, cvers, size, NULL);
    spin_unlock(&l->lock);
    return ok;
}

bool larder_pages_snapshot(struct larder *l, u64 qid_path, u32 cvers,
                           u64 size, u8 *out, u64 seq0) {
    if (!l) return false;
    if (size > 0 && !out) return false;
    spin_lock(&l->lock);
    // The GEN WITNESS (B1-audit F1): the two-party invalidate-vs-snapshot
    // serialization below is not enough on its own -- a THIRD actor holding
    // a fid opened before a concurrent own-write can re-populate the just-
    // invalidated pages with POST-write bytes tagged the OLD cvers (the
    // populate tags with the reading fid's open-time qid.vers), re-satisfying
    // coverage at `cvers` and minting a torn snapshot (post-write bytes at
    // the pre-write size/attr -- a view no fresh RPC could return, an I-38
    // NoWrongRead violation). `seq0` is the caller's gen capture from BEFORE
    // its coverage decision (the cached-open hint / the pre-RPC refill).
    // G4-scoped to THIS file: the hole is an own-write to qid_path (whose
    // invalidate logs qid_path); another file's event cannot stale this
    // file's pages or re-satisfy its coverage, so it no longer fails the
    // snapshot (the global form killed cached-opens under unrelated write
    // churn). Fail-safe on ring overflow, as everywhere.
    if (inval_hits_locked(l, seq0, qid_path)) {
        l->co_misses++;
        spin_unlock(&l->lock);
        return false;
    }
    // ONE lock hold over the whole verify-and-copy: a concurrent own-write
    // invalidate either precedes this (its unlink fails page_find -> coverage
    // FALSE; the gen witness above catches the repopulated-stale case) or
    // follows the entire copy -- the snapshot is atomically the open-time
    // content at `cvers` (the fs_cache.tla Open linearization).
    bool ok = pages_cover_locked(l, qid_path, cvers, size, out);
    if (ok) l->co_snapshots++; else l->co_misses++;
    spin_unlock(&l->lock);
    return ok;
}

void larder_destroy(struct larder *l) {
    if (!l) return;
    spin_lock(&l->lock);
    if (l->attr) {
        kfree(l->attr);
        if (l->attr_hash) kfree(l->attr_hash);
        l->attr = NULL; l->attr_hash = NULL;
        l->attr_cap = 0; l->attr_nbuckets = 0;
        l->attr_used = 0; l->attr_hand = 0;
    }
    if (l->dentry) {
        kfree(l->dentry);
        if (l->dentry_hash) kfree(l->dentry_hash);
        l->dentry = NULL; l->dentry_hash = NULL;
        l->dentry_cap = 0; l->dentry_nbuckets = 0;
        l->dentry_used = 0; l->dentry_hand = 0;
    }
    if (l->page) {
        for (u32 i = 0; i < l->page_cap; i++) {
            if (l->page[i].page) kfree(l->page[i].page);
        }
        kfree(l->page);          // the heap page-entry array itself (lazy-allocated)
        if (l->page_hash) kfree(l->page_hash);
        if (l->page_qhash) kfree(l->page_qhash);
        l->page = NULL;
        l->page_hash = NULL;
        l->page_qhash = NULL;
        l->page_cap = 0;
        l->page_nbuckets = 0;
        l->page_qnbuckets = 0;
        l->page_used = 0;
        l->page_hand = 0;
    }
    spin_unlock(&l->lock);
}
