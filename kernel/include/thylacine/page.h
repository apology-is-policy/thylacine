// Page descriptor and physical-allocator common types.
//
// Per ARCHITECTURE.md §6.3. The struct page array is one entry per
// physical 4 KiB frame across the managed zone. Allocated by phys_init
// at boot from contiguous RAM just past the kernel image; sized
// dynamically based on DTB-discovered RAM. Linked-list pointers in
// struct page back the buddy allocator's per-order free lists; the
// magazine layer (mm/magazines.c) layers on top of buddy without
// touching struct page directly.

#ifndef THYLACINE_PAGE_H
#define THYLACINE_PAGE_H

#include <thylacine/types.h>

// 4 KiB granule (matches MMU TG0 / TG1 = 4K).
#define PAGE_SHIFT  12
#define PAGE_SIZE   (1ull << PAGE_SHIFT)
#define PAGE_MASK   (PAGE_SIZE - 1)

// Maximum buddy order. 2^MAX_ORDER * PAGE_SIZE = largest single
// allocation. ARCH §6.3 documents orders 0..18 (4 KiB to 1 GiB).
#define MAX_ORDER   18

// Forward declaration so struct page can carry a backref. Defined in
// mm/slub.h.
struct kmem_cache;

// Page descriptor. One per physical 4 KiB frame in the managed zone.
//
// Size: 48 bytes — chosen to carry the doubly-linked-list pointers
// (O(1) free-list manipulation) plus the two SLUB-only fields
// (slab_freelist + slab_cache). For 2 GiB of RAM at 4 KiB granule
// (524288 pages), the struct page array is 24 MiB — placed by
// phys_init just past the kernel image at the start of free RAM.
//
// The slab-only fields are valid when (flags & PG_SLAB); for a buddy
// free or generic kernel allocation page they're zero. We accept the
// 50% size growth from P1-D (16 MiB → 24 MiB) over the alternative
// of overlaying with a union — clarity of access and absence of
// "is this field meaningful?" footguns matter more than ~8 MiB of
// BSS at v1.0.
struct page {
    struct page *next;          // free list next (NULL if not on a list)
    struct page *prev;          // free list prev
    u32 order;                  // current order if PG_FREE; slab order if PG_SLAB
    u32 flags;                  // PG_*
    u32 refcount;               // VMO refcount placeholder; slab: inuse count
    u32 _pad;                   // 4-byte alignment slack
    void *slab_freelist;        // SLUB: head of free objects in this slab
    struct kmem_cache *slab_cache; // SLUB: cache backref (NULL when not a slab)
};

// Pin struct page size at compile time (P1-I audit F35). The struct
// page array scales with RAM (24 MiB at 2 GiB / 96 MiB at 8 GiB);
// adding a field silently grows the BSS reservation. Catch the drift
// at build time so a future field addition is intentional.
_Static_assert(sizeof(struct page) == 48,
               "struct page must be 48 bytes (changes scale the per-RAM BSS); "
               "if intentional update phys.c reservation math AND this assert");

// struct page flags.
#define PG_FREE       (1u << 0) // page is currently on a buddy free list
#define PG_RESERVED   (1u << 1) // page is reserved (kernel image, DTB blob,
                                // struct-page array itself, low firmware)
#define PG_KERNEL     (1u << 2) // page is allocated to the kernel
#define PG_SLAB       (1u << 3) // page is a SLUB slab (slab_freelist + slab_cache valid)

// Allocation flags (caller-passed to alloc_pages / kpage_alloc).
//
// Per ARCHITECTURE.md §6.3. v1.0 supports KP_ZERO meaningfully; the
// other flags are accepted but no-op until later phases:
//   - KP_DMA wants a < 4 GiB PA; v1.0 has a single zone (already low
//     PA) so the flag is satisfied unconditionally.
//   - KP_NOWAIT is implicit at v1.0 (no scheduler).
//   - KP_COMPLETE (don't return until magazine refill) is unused.
#define KP_ZERO       (1u << 0)
#define KP_DMA        (1u << 1)
#define KP_NOWAIT     (1u << 2)
#define KP_COMPLETE   (1u << 3)

// PFN <-> struct page conversion. Backed by globals set during
// buddy_zone_init. Inline so hot allocator paths don't pay function-
// call overhead.
extern struct page *_g_struct_pages;
extern paddr_t _g_zone_base_pfn;

static inline struct page *pfn_to_page(paddr_t pfn) {
    return &_g_struct_pages[pfn - _g_zone_base_pfn];
}

static inline paddr_t page_to_pfn(const struct page *p) {
    return _g_zone_base_pfn + (paddr_t)(p - _g_struct_pages);
}

static inline paddr_t page_to_pa(const struct page *p) {
    return page_to_pfn(p) << PAGE_SHIFT;
}

static inline struct page *pa_to_page(paddr_t pa) {
    return pfn_to_page(pa >> PAGE_SHIFT);
}

#endif // THYLACINE_PAGE_H
