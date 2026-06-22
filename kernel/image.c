// Image — the qid-keyed shared-text cache (REVENANT R-3; the Plan 9 Image).
//
// docs/REVENANT.md §4.4 + ARCH §28 I-36. See <thylacine/image.h> for the design;
// this file is the implementation. The cache is a fixed table of entries, each
// holding ONE handle_count ref on a BURROW_TYPE_FILE Burrow; a lookup of a
// matching file identity returns the existing Burrow (cross-Proc text share),
// else creates + registers a fresh one.
//
// REFCOUNT MODEL (the load-bearing invariant). A cached FILE Burrow is the SOLE
// clunk-er of its backing Spoor (R-1 adopt-and-clunk), so exactly one path ever
// closes a given Spoor:
//   - MISS: burrow_create_file ADOPTS the caller's spoor; the cache keeps the
//     construction handle ref (=1); a second burrow_ref gives the caller its
//     ref (handle_count=2). The Spoor is consumed by the Burrow's eventual free.
//   - HIT:  the cached Burrow already owns ITS spoor; the caller's spoor is
//     redundant -> spoor_clunk it (outside the lock; dev->close may sleep). The
//     cached Burrow is burrow_ref'd for the caller.
//   - LOST RACE (two Procs create the same image concurrently): the loser's
//     fresh Burrow is burrow_unref'd to {0,0} -> frees -> clunks ITS adopted
//     spoor. The winner is burrow_ref'd for the caller. One clunk per spoor.
//
// EVICTION SAFETY (the SMP proof). Eviction picks only an IDLE victim
// (handle_count==1 [cache-only] && mapping_count==0 [unmapped]). Under
// g_image_lock both counts of such an entry are STABLE: to add a ref or a
// mapping a Proc must first image_lookup_or_create (which takes g_image_lock,
// burrow_ref -> handle_count>=2) THEN burrow_map — and it is locked out of
// g_image_lock while we evict. So a handle_count==1 entry has NO in-flight
// mapper, mapping_count cannot rise (only fall via an unmap that does not touch
// the cache), and the victim — once its slot is cleared under the lock — is
// reachable by no other path, so the burrow_unref OUTSIDE the lock cannot race a
// free. Lock order: g_image_lock -> v->lock (burrow_ref/unref take v->lock);
// never the reverse (the FILE free arm never re-enters the cache — the entry is
// always detached BEFORE the ref that frees is dropped).

#include <thylacine/burrow.h>
#include <thylacine/extinction.h>
#include <thylacine/image.h>
#include <thylacine/page.h>          // PAGE_SIZE
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>         // struct Spoor + spoor_clunk + SPOOR_MAGIC + qid
#include <thylacine/types.h>         // SIZE_MAX

// One cache slot. `burrow` holds the cache's single handle_count ref. The key
// scalars are sampled from the Spoor at install (== the Burrow's file_* fields).
struct image_entry {
    bool           used;
    int            dc;
    u32            devno;
    u64            qid_path;
    u32            qid_vers;
    u64            file_offset;
    size_t         size;            // page-rounded segment size (== Burrow->size)
    struct Burrow *burrow;          // cache's single handle ref
    u64            lru;             // last-use stamp (LRU victim selection)
};

// BSS-zeroed: every slot starts used=false; the lock starts unlocked
// (SPIN_LOCK_INIT == {0}); the clock + counters start 0. So no allocation is
// needed at init (mirrors kernel/pci_handle.c's g_pci_claims). Plain spin_lock:
// process-context only (exec is process context), never from IRQ — so nesting
// the process-context v->lock under it is sound.
static struct image_entry g_image[IMAGE_CACHE_MAX];
static spin_lock_t        g_image_lock = SPIN_LOCK_INIT;
static u64                g_image_clock;      // monotonic LRU source (under lock)
static bool               g_image_inited;

// Diagnostics (under the lock; a future /ctl image-stats surface + the tests).
static u64 g_image_hits;          // returned an existing cached Burrow
static u64 g_image_creates;       // registered a fresh Burrow
static u64 g_image_evictions;     // dropped a victim to make room
static u64 g_image_bypass;        // created un-cached (table full of live images)

void image_cache_init(void) {
    if (g_image_inited)
        extinction("image_cache_init called twice");
    g_image_inited = true;
}

// Page-round a length the same way burrow_create_file does, so the cache key's
// `size` matches the Burrow's `size` exactly.
static size_t page_round(size_t length) {
    return ((length + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
}

// Key match against an input (spoor, file_offset, page-rounded size). Read under
// g_image_lock.
static bool key_match(const struct image_entry *e, const struct Spoor *s,
                      u64 file_offset, size_t size) {
    return e->used &&
           e->dc          == s->dc &&
           e->devno       == s->devno &&
           e->qid_path    == s->qid.path &&
           e->qid_vers    == s->qid.vers &&
           e->file_offset == file_offset &&
           e->size        == size;
}

// Pick a slot to install a new entry: a free slot if any, else the LRU IDLE
// victim (handle_count==1 && mapping_count==0 — see the eviction-safety proof in
// the file header), else -1 (every entry is in active use -> caller bypasses the
// cache). Called under g_image_lock.
static int find_install_slot(void) {
    for (int i = 0; i < IMAGE_CACHE_MAX; i++)
        if (!g_image[i].used)
            return i;

    int victim = -1;
    u64 best = (u64)-1;
    for (int i = 0; i < IMAGE_CACHE_MAX; i++) {
        struct Burrow *b = g_image[i].burrow;
        // Stable under the lock for a handle_count==1 entry (no in-flight mapper
        // can exist — it would hold handle_count>=2): a true idle image.
        if (burrow_handle_count(b) == 1 && burrow_mapping_count(b) == 0) {
            if (g_image[i].lru < best) {
                best = g_image[i].lru;
                victim = i;
            }
        }
    }
    return victim;
}

struct Burrow *image_lookup_or_create(struct Spoor *spoor, u64 file_offset, size_t length) {
    if (!spoor)
        return NULL;
    if (spoor->magic != SPOOR_MAGIC)
        extinction("image_lookup_or_create: spoor has bad magic (UAF?)");
    // length==0 / overflow are caller bugs -> NULL WITHOUT consuming the spoor
    // (the caller still owns it), mirroring burrow_create_file's NULL-on-bad-arg.
    if (length == 0)
        return NULL;
    if (length > SIZE_MAX - (PAGE_SIZE - 1))
        return NULL;

    size_t size = page_round(length);

    // --- pass 1: search the cache ---
    spin_lock(&g_image_lock);
    for (int i = 0; i < IMAGE_CACHE_MAX; i++) {
        if (key_match(&g_image[i], spoor, file_offset, size)) {
            struct Burrow *cached = g_image[i].burrow;
            burrow_ref(cached);              // the caller's handle ref
            g_image[i].lru = ++g_image_clock;
            g_image_hits++;
            spin_unlock(&g_image_lock);
            // The caller's spoor is redundant (the cached Burrow owns its own).
            // Consume it OUTSIDE the lock — spoor_clunk -> dev->close may sleep.
            spoor_clunk(spoor);
            return cached;
        }
    }
    spin_unlock(&g_image_lock);

    // --- create OUTSIDE the lock (burrow_create_file may sleep; it ADOPTS spoor) ---
    struct Burrow *fresh = burrow_create_file(spoor, file_offset, length);
    if (!fresh)
        return NULL;                         // no ref taken; caller still owns spoor

    // --- pass 2: re-search (the create race) + install ---
    spin_lock(&g_image_lock);
    for (int i = 0; i < IMAGE_CACHE_MAX; i++) {
        if (key_match(&g_image[i], spoor, file_offset, size)) {
            // Lost the race: a concurrent exec registered the same image first.
            struct Burrow *winner = g_image[i].burrow;
            burrow_ref(winner);              // the caller's handle ref
            g_image[i].lru = ++g_image_clock;
            g_image_hits++;
            spin_unlock(&g_image_lock);
            // Discard our surplus Burrow: the last unref frees it + clunks the
            // spoor it adopted (so the spoor is consumed exactly once).
            burrow_unref(fresh);
            return winner;
        }
    }

    int slot = find_install_slot();
    if (slot < 0) {
        // Cache full of live images: BYPASS. Return `fresh` as-is — handle_count
        // is 1 (== the caller's ref; NO cache ref); it lives on its mapping and
        // frees at unmap. Fail-safe degrade, never an exec failure.
        g_image_bypass++;
        spin_unlock(&g_image_lock);
        return fresh;
    }

    // Install. The cache KEEPS the construction ref (=1); bump for the caller (=2).
    struct Burrow *victim = NULL;
    if (g_image[slot].used) {
        victim = g_image[slot].burrow;       // detached here; unref'd outside the lock
        g_image_evictions++;
    }
    burrow_ref(fresh);                        // the caller's handle ref (cache keeps the original)
    g_image[slot].used        = true;
    g_image[slot].dc          = spoor->dc;
    g_image[slot].devno       = spoor->devno;
    g_image[slot].qid_path    = spoor->qid.path;
    g_image[slot].qid_vers    = spoor->qid.vers;
    g_image[slot].file_offset = file_offset;
    g_image[slot].size        = size;
    g_image[slot].burrow      = fresh;
    g_image[slot].lru         = ++g_image_clock;
    g_image_creates++;
    spin_unlock(&g_image_lock);

    if (victim)
        burrow_unref(victim);                 // drop the cache's ref: {1,0} -> {0,0} -> free + clunk spoor
    return fresh;
}

#ifdef KERNEL_TESTS
int image_cache_live_count_for_test(void) {
    spin_lock(&g_image_lock);
    int n = 0;
    for (int i = 0; i < IMAGE_CACHE_MAX; i++)
        if (g_image[i].used)
            n++;
    spin_unlock(&g_image_lock);
    return n;
}

int image_cache_evict_idle_for_test(void) {
    struct Burrow *victims[IMAGE_CACHE_MAX];
    int nv = 0;
    spin_lock(&g_image_lock);
    for (int i = 0; i < IMAGE_CACHE_MAX; i++) {
        if (!g_image[i].used)
            continue;
        struct Burrow *b = g_image[i].burrow;
        if (burrow_handle_count(b) == 1 && burrow_mapping_count(b) == 0) {
            victims[nv++] = b;
            g_image[i].used   = false;
            g_image[i].burrow = NULL;
        }
    }
    spin_unlock(&g_image_lock);
    // Outside the lock: each victim was the cache's last ref ({1,0}) on an entry
    // reachable by no other path (just detached) -> unref frees + clunks spoor.
    for (int i = 0; i < nv; i++)
        burrow_unref(victims[i]);
    return nv;
}

u64 image_cache_hits_for_test(void)      { return g_image_hits; }
u64 image_cache_creates_for_test(void)   { return g_image_creates; }
u64 image_cache_evictions_for_test(void) { return g_image_evictions; }
#endif
