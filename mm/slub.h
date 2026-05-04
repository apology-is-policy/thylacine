// SLUB-style kernel object allocator (Linux's modern default since 2008).
//
// Layered on top of the buddy allocator (mm/buddy + mm/phys). Each
// kmem_cache groups slabs of identical-size objects; allocations
// pop from the cache's partial-slab freelist, frees push back. Free
// objects within a slab carry a "next" pointer in their first 8
// bytes — zero metadata overhead per free object.
//
// Public API matches ARCHITECTURE.md §6.4. Standard caches:
//   - kmalloc-{8,16,32,64,128,256,512,1024,2048} — power-of-two
//     general-purpose. kmalloc(N) finds the next-larger cache; sizes
//     above 2048 bypass to alloc_pages directly.
//   - kmem_cache_create(name, size, align, flags) for typed caches
//     (Phase 2: proc_cache, thread_cache, chan_cache, vmo_cache,
//     handle_cache).
//
// Bootstrap: a static `g_meta_cache` is initialized first so
// `struct kmem_cache` instances created via kmem_cache_create can
// be allocated; the standard kmalloc-* caches live in BSS as a
// static array (no allocation needed for them).
//
// Per ARCHITECTURE.md §6.4.

#ifndef THYLACINE_MM_SLUB_H
#define THYLACINE_MM_SLUB_H

#include <thylacine/page.h>
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

// Maximum object size for the slab path. Requests above this go
// through alloc_pages directly (the caller pays for power-of-two
// rounding via the buddy's order machinery).
#define SLUB_MAX_OBJECT_SIZE   2048

// Minimum object size = sizeof(void *) so each free object carries
// the freelist next-pointer in its body.
#define SLUB_MIN_OBJECT_SIZE   8
#define SLUB_MIN_ALIGN         8

// kmem_cache flags.
#define KMEM_CACHE_PANIC_ON_FAIL  (1u << 0)   // extinction on alloc failure
// (Future: KMEM_CACHE_ZERO, KMEM_CACHE_DEBUG_REDZONE, etc.)

struct kmem_cache {
    const char *name;
    size_t object_size;          // requested size (for diagnostics)
    size_t actual_size;          // padded for alignment + freelist link
    size_t align;
    unsigned flags;

    unsigned slab_order;         // log2(pages per slab); 0 at v1.0
    unsigned objects_per_slab;

    struct page partial_list;    // sentinel head of partial slabs
    u64 nr_partial;

    // Diagnostics
    u64 alloc_count;
    u64 free_count;
    u64 slabs_active;
    u64 slabs_drained;

    spin_lock_t lock;

    // Linked list of all caches (for /ctl/mem at Phase 2; for now,
    // diagnostic iteration only).
    struct kmem_cache *next_cache;
};

// Initialize the SLUB layer. Sets up g_meta_cache and the standard
// kmalloc-* caches. Must be called after phys_init.
void slub_init(void);

// Create a typed cache for objects of size `size`, aligned to `align`
// (rounded up to SLUB_MIN_ALIGN). `name` is informational; not copied.
// `flags` is a KMEM_CACHE_* bitmask.
//
// Returns NULL on OOM (or if size > SLUB_MAX_OBJECT_SIZE).
struct kmem_cache *kmem_cache_create(const char *name, size_t size,
                                     size_t align, unsigned flags);

// Drain the cache's partial slabs back to the buddy and release the
// cache descriptor. Caller's responsibility to ensure no live objects
// remain (we don't track per-object liveness across destroy).
void kmem_cache_destroy(struct kmem_cache *c);

// Allocate / free a single object from a cache.
void *kmem_cache_alloc(struct kmem_cache *c, unsigned flags);
void  kmem_cache_free(struct kmem_cache *c, void *obj);

// Generic kmalloc / kfree. kmalloc finds the smallest power-of-two
// cache that fits; sizes > SLUB_MAX_OBJECT_SIZE allocate via
// alloc_pages directly.
//
// kzalloc zero-initializes the returned object. kcalloc is equivalent
// to kzalloc(n * size, flags) with overflow check (returns NULL on
// overflow).
void *kmalloc(size_t n, unsigned flags);
void *kzalloc(size_t n, unsigned flags);
void *kcalloc(size_t n, size_t size, unsigned flags);
void  kfree(void *p);

// Diagnostic accessors used by the boot banner / future /ctl/mem.
u64 slub_total_alloc(void);
u64 slub_total_free(void);
u64 slub_active_slabs(void);

#endif // THYLACINE_MM_SLUB_H
