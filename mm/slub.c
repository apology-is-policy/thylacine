// SLUB-style slab allocator. Per-cache partial slab list; per-slab
// freelist embedded in the unused object memory (zero overhead per
// free object). Hot path is "pop the cache's first partial slab,
// pop an object from its freelist."
//
// At P1-E we ship single-page slabs only (slab_order = 0). Object
// sizes up to SLUB_MAX_OBJECT_SIZE = 2048 bytes go through slab;
// larger requests bypass to alloc_pages directly. This covers every
// kernel struct we'll allocate at v1.0 (Proc, Thread, Chan, VMO,
// Handle are all well under 2 KiB) and the kmalloc-{8..2048}
// general-purpose caches. kmalloc-{4096..262144} from ARCH §6.4 are
// served via alloc_pages — same API surface, different backend.
//
// Bootstrap order:
//   1. Static g_meta_cache initialized first (carries struct
//      kmem_cache instances themselves so kmem_cache_create can
//      allocate them).
//   2. Static g_kmalloc_caches[] initialized in BSS — no allocation
//      needed; one cache descriptor per power-of-two size.
//   3. After slub_init returns, kmalloc / kmem_cache_create are
//      live.
//
// Per ARCHITECTURE.md §6.4.

#include "slub.h"
#include "phys.h"

#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

// ---------------------------------------------------------------------------
// Cache table.
// ---------------------------------------------------------------------------

// Index = log2(size). Indices 0..2 unused (sizes < 8 round up to 8).
// Indices 3..11 are kmalloc-{8..2048}. Index 12+ is "above SLUB max".
#define KMALLOC_MIN_IDX     3
#define KMALLOC_MAX_IDX     11
#define KMALLOC_NUM_CACHES  (KMALLOC_MAX_IDX + 1)

static struct kmem_cache g_meta_cache;
static struct kmem_cache g_kmalloc_caches[KMALLOC_NUM_CACHES];
static struct kmem_cache *g_cache_list_head;

static const char *KMALLOC_NAMES[KMALLOC_NUM_CACHES] = {
    NULL, NULL, NULL,
    "kmalloc-8",  "kmalloc-16",  "kmalloc-32",   "kmalloc-64",
    "kmalloc-128","kmalloc-256", "kmalloc-512",  "kmalloc-1024",
    "kmalloc-2048",
};

// ---------------------------------------------------------------------------
// Doubly-linked list ops on the per-cache partial list. Sentinel head
// is itself a struct page (only next/prev fields are used).
// ---------------------------------------------------------------------------

static inline void list_init_head(struct page *head) {
    head->next = head;
    head->prev = head;
}

static inline bool list_empty(const struct page *head) {
    return head->next == head;
}

static inline void list_push_front(struct page *head, struct page *p) {
    p->next = head->next;
    p->prev = head;
    head->next->prev = p;
    head->next = p;
}

static inline void list_remove(struct page *p) {
    p->prev->next = p->next;
    p->next->prev = p->prev;
    p->next = NULL;
    p->prev = NULL;
}

// ---------------------------------------------------------------------------
// Bit utilities.
// ---------------------------------------------------------------------------

static inline u64 round_up_size(u64 v, u64 a) {
    return (v + (a - 1)) & ~(a - 1);
}

static inline int ceil_log2_u64(u64 n) {
    if (n <= 1) return 0;
    return 64 - __builtin_clzll(n - 1);
}

// ---------------------------------------------------------------------------
// Cache initialization.
//
// init_cache populates a kmem_cache descriptor in place. Called by
// slub_init for the static caches, and by kmem_cache_create for
// dynamic ones (after kmem_cache_alloc'ing the descriptor itself
// from g_meta_cache).
// ---------------------------------------------------------------------------

static void init_cache(struct kmem_cache *c, const char *name,
                       size_t size, size_t align, unsigned flags) {
    if (align < SLUB_MIN_ALIGN) align = SLUB_MIN_ALIGN;
    size_t actual = size < SLUB_MIN_OBJECT_SIZE ? SLUB_MIN_OBJECT_SIZE : size;
    actual = round_up_size(actual, align);

    c->name        = name;
    c->object_size = size;
    c->actual_size = actual;
    c->align       = align;
    c->flags       = flags;
    c->slab_order  = 0;     // P1-E: single-page slabs only
    c->objects_per_slab = (PAGE_SIZE << c->slab_order) / actual;
    list_init_head(&c->partial_list);
    c->nr_partial      = 0;
    c->alloc_count     = 0;
    c->free_count      = 0;
    c->slabs_active    = 0;
    c->slabs_drained   = 0;
    spin_lock_init(&c->lock);

    // Link onto the global list (head insertion).
    c->next_cache    = g_cache_list_head;
    g_cache_list_head = c;
}

// ---------------------------------------------------------------------------
// Slab page management. A "slab" at P1-E is a single 4 KiB page from
// the buddy allocator. The page's struct page carries:
//   - PG_SLAB flag.
//   - slab_cache backref.
//   - slab_freelist: pointer to first free object in the page.
//   - refcount: repurposed as inuse count (allocated objects).
// Free objects within the slab are linked via their first 8 bytes
// (each free object stores a pointer to the next free object).
// ---------------------------------------------------------------------------

// Initialize a fresh slab page's freelist. All objects are free.
static void slab_init_freelist(struct kmem_cache *c, struct page *slab) {
    paddr_t base_pa = page_to_pa(slab);
    void *prev = NULL;

    // Walk objects from last to first, threading prev pointers. The
    // resulting freelist starts at object 0 and chains forward.
    for (int i = (int)c->objects_per_slab - 1; i >= 0; i--) {
        void *obj = (void *)(uintptr_t)(base_pa + (paddr_t)i * c->actual_size);
        *(void **)obj = prev;
        prev = obj;
    }
    slab->slab_freelist = prev;     // = first object
    slab->slab_cache    = c;
    slab->flags         = PG_SLAB;
    slab->refcount      = 0;        // inuse
}

// Allocate a new slab page from the buddy and initialize it.
static struct page *slab_new(struct kmem_cache *c) {
    struct page *slab = alloc_pages(c->slab_order, 0);
    if (!slab) return NULL;
    slab_init_freelist(c, slab);
    c->slabs_active++;
    return slab;
}

// Drain an empty slab back to the buddy.
static void slab_drain(struct kmem_cache *c, struct page *slab) {
    slab->slab_freelist = NULL;
    slab->slab_cache    = NULL;
    slab->flags         = 0;
    slab->refcount      = 0;
    c->slabs_active--;
    c->slabs_drained++;
    free_pages(slab, c->slab_order);
}

// ---------------------------------------------------------------------------
// kmem_cache_alloc / kmem_cache_free.
// ---------------------------------------------------------------------------

static void *cache_alloc_locked(struct kmem_cache *c) {
    struct page *slab;

    if (!list_empty(&c->partial_list)) {
        slab = c->partial_list.next;
    } else {
        slab = slab_new(c);
        if (!slab) return NULL;
        list_push_front(&c->partial_list, slab);
        c->nr_partial++;
    }

    // Pop one object from the slab's freelist.
    void *obj = slab->slab_freelist;
    slab->slab_freelist = *(void **)obj;
    slab->refcount++;
    c->alloc_count++;

    // If the slab is now full, take it off the partial list. (Without
    // a separate "full" list, we just stop tracking it; kmem_cache_free
    // will re-add to partial when it transitions from full → has-free.)
    if (slab->refcount == c->objects_per_slab) {
        list_remove(slab);
        c->nr_partial--;
    }

    return obj;
}

void *kmem_cache_alloc(struct kmem_cache *c, unsigned flags) {
    irq_state_t s = spin_lock_irqsave(&c->lock);
    void *obj = cache_alloc_locked(c);
    spin_unlock_irqrestore(&c->lock, s);

    if (!obj) {
        if (c->flags & KMEM_CACHE_PANIC_ON_FAIL) {
            extinction("kmem_cache_alloc: out of memory");
        }
        return NULL;
    }

    if (flags & KP_ZERO) {
        u8 *q = (u8 *)obj;
        for (size_t i = 0; i < c->actual_size; i++) q[i] = 0;
    }
    return obj;
}

static void cache_free_locked(struct kmem_cache *c, void *obj, struct page *slab) {
    bool was_full = (slab->refcount == c->objects_per_slab);

    *(void **)obj = slab->slab_freelist;
    slab->slab_freelist = obj;
    slab->refcount--;
    c->free_count++;

    if (was_full) {
        // slab transitions from full → has-free: re-add to partial.
        list_push_front(&c->partial_list, slab);
        c->nr_partial++;
    } else if (slab->refcount == 0) {
        // slab fully free → drain to buddy. Remove from partial first.
        list_remove(slab);
        c->nr_partial--;
        slab_drain(c, slab);
    }
}

void kmem_cache_free(struct kmem_cache *c, void *obj) {
    if (!obj) return;
    struct page *slab = pa_to_page((paddr_t)(uintptr_t)obj);
    if (!(slab->flags & PG_SLAB) || slab->slab_cache != c) {
        extinction("kmem_cache_free: object not from this cache");
    }
    irq_state_t s = spin_lock_irqsave(&c->lock);
    cache_free_locked(c, obj, slab);
    spin_unlock_irqrestore(&c->lock, s);
}

// ---------------------------------------------------------------------------
// kmem_cache_create / kmem_cache_destroy.
// ---------------------------------------------------------------------------

struct kmem_cache *kmem_cache_create(const char *name, size_t size,
                                     size_t align, unsigned flags) {
    if (size > SLUB_MAX_OBJECT_SIZE) return NULL;

    struct kmem_cache *c = kmem_cache_alloc(&g_meta_cache, KP_ZERO);
    if (!c) return NULL;
    init_cache(c, name, size, align, flags);
    return c;
}

void kmem_cache_destroy(struct kmem_cache *c) {
    if (!c) return;

    // Drain all partial slabs.
    while (!list_empty(&c->partial_list)) {
        struct page *slab = c->partial_list.next;
        list_remove(slab);
        c->nr_partial--;
        slab_drain(c, slab);
    }

    // Unlink from the global cache list.
    if (g_cache_list_head == c) {
        g_cache_list_head = c->next_cache;
    } else {
        for (struct kmem_cache *p = g_cache_list_head; p; p = p->next_cache) {
            if (p->next_cache == c) {
                p->next_cache = c->next_cache;
                break;
            }
        }
    }

    // Return the cache descriptor itself.
    kmem_cache_free(&g_meta_cache, c);
}

// ---------------------------------------------------------------------------
// kmalloc / kfree / kzalloc / kcalloc.
// ---------------------------------------------------------------------------

// Pick the smallest kmalloc-N cache that fits `n`. Returns the cache
// pointer, or NULL if `n` exceeds SLUB_MAX_OBJECT_SIZE.
static struct kmem_cache *kmalloc_cache_for(size_t n) {
    if (n > SLUB_MAX_OBJECT_SIZE) return NULL;
    int idx = ceil_log2_u64((u64)n);
    if (idx < KMALLOC_MIN_IDX) idx = KMALLOC_MIN_IDX;
    if (idx > KMALLOC_MAX_IDX) return NULL;
    return &g_kmalloc_caches[idx];
}

void *kmalloc(size_t n, unsigned flags) {
    if (n == 0) return NULL;

    struct kmem_cache *c = kmalloc_cache_for(n);
    if (c) {
        return kmem_cache_alloc(c, flags);
    }

    // Large request: bypass slab and use alloc_pages directly. The
    // page itself records the order (set by alloc_pages); kfree
    // reads it back.
    size_t pages = (n + PAGE_SIZE - 1) >> PAGE_SHIFT;
    unsigned order = (unsigned)ceil_log2_u64((u64)pages);
    struct page *p = alloc_pages(order, flags);
    if (!p) return NULL;
    return (void *)(uintptr_t)page_to_pa(p);
}

void *kzalloc(size_t n, unsigned flags) {
    return kmalloc(n, flags | KP_ZERO);
}

void *kcalloc(size_t n, size_t size, unsigned flags) {
    if (n != 0 && size > (~(size_t)0) / n) return NULL;     // overflow check
    return kzalloc(n * size, flags);
}

void kfree(void *p) {
    if (!p) return;
    struct page *page = pa_to_page((paddr_t)(uintptr_t)p);
    if (page->flags & PG_SLAB) {
        kmem_cache_free(page->slab_cache, p);
    } else {
        // Large allocation: call free_pages with the order recorded
        // on the head page.
        free_pages(page, page->order);
    }
}

// ---------------------------------------------------------------------------
// Initialization.
// ---------------------------------------------------------------------------

void slub_init(void) {
    g_cache_list_head = NULL;

    // 1. Meta cache for struct kmem_cache. Must be initialized first
    //    so kmem_cache_create can allocate descriptors from it.
    init_cache(&g_meta_cache, "kmem_cache",
               sizeof(struct kmem_cache),
               _Alignof(struct kmem_cache), 0);

    // 2. Standard kmalloc-* caches. Static descriptors in BSS; no
    //    allocation needed.
    for (int i = KMALLOC_MIN_IDX; i <= KMALLOC_MAX_IDX; i++) {
        size_t size = (size_t)1 << i;
        init_cache(&g_kmalloc_caches[i], KMALLOC_NAMES[i],
                   size, SLUB_MIN_ALIGN, 0);
    }
}

// ---------------------------------------------------------------------------
// Diagnostics.
// ---------------------------------------------------------------------------

u64 slub_total_alloc(void) {
    u64 total = 0;
    for (struct kmem_cache *c = g_cache_list_head; c; c = c->next_cache) {
        total += c->alloc_count;
    }
    return total;
}

u64 slub_total_free(void) {
    u64 total = 0;
    for (struct kmem_cache *c = g_cache_list_head; c; c = c->next_cache) {
        total += c->free_count;
    }
    return total;
}

u64 slub_active_slabs(void) {
    u64 total = 0;
    for (struct kmem_cache *c = g_cache_list_head; c; c = c->next_cache) {
        total += c->slabs_active;
    }
    return total;
}
