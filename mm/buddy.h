// Buddy allocator — Knuth (1973). Per-zone power-of-two free lists,
// one zone per NUMA node (single zone at v1.0). Layered behind the
// per-CPU magazine cache (mm/magazines.c) so the hot path on common
// orders (0 = 4 KiB, 9 = 2 MiB) is a per-CPU stack pop, not a buddy
// lock acquisition.
//
// Per ARCHITECTURE.md §6.3.

#ifndef THYLACINE_MM_BUDDY_H
#define THYLACINE_MM_BUDDY_H

#include <thylacine/page.h>
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

// One buddy zone covers a contiguous PA range [base_pa, end_pa).
// Members are read-mostly post-init; the spinlock protects the free
// lists during alloc / free.
struct buddy_zone {
    paddr_t base_pa;
    paddr_t end_pa;
    u64 num_pages;

    // Sentinel-headed doubly-linked free lists, one per order. The
    // sentinel is itself a struct page (only its next/prev fields
    // are used). list_empty(head) == (head->next == head).
    struct page free_lists[MAX_ORDER + 1];
    u64 free_pages_per_order[MAX_ORDER + 1];
    u64 total_free_pages;

    spin_lock_t lock;
};

// The single zone for v1.0. Phase 2+ may add more (multi-NUMA / DMA
// zones) but the API takes a zone explicitly so the wiring is ready.
extern struct buddy_zone g_zone0;

// Initialize a zone covering [base_pa, end_pa). The caller must have
// allocated `struct_pages` (one entry per 4 KiB frame) externally —
// phys_init does this from the start of free RAM.
//
// Marks all pages PG_RESERVED. Caller follows with one or more
// buddy_free_region() calls to populate the free lists with the
// known-free PA ranges.
void buddy_zone_init(struct buddy_zone *zone, paddr_t base_pa, paddr_t end_pa,
                     struct page *struct_pages);

// Free a contiguous PA range [start_pa, end_pa) into the zone. The
// implementation chops the range into the largest possible power-of-2
// aligned chunks (alignment limited by the start address; size limited
// by remaining range and MAX_ORDER).
//
// Caller's responsibility to ensure no PG_RESERVED pages live in the
// range (i.e., this is a "definitely free" range).
void buddy_free_region(struct buddy_zone *zone, paddr_t start_pa, paddr_t end_pa);

// Allocate a 2^order page block. Splits a larger block if no exact
// match is on the free list. Returns NULL when no block of the
// requested order or larger is available.
//
// On success: the returned page has PG_KERNEL set, refcount = 1, and
// is no longer on any free list.
struct page *buddy_alloc(struct buddy_zone *zone, unsigned order);

// Free a 2^order page block back to the zone. Iteratively merges with
// the buddy at order, order+1, ... up to MAX_ORDER as long as the
// buddy is also PG_FREE at the same order.
//
// After return, the page (or its merged ancestor) has PG_FREE set and
// is on the appropriate free list.
void buddy_free(struct buddy_zone *zone, struct page *p, unsigned order);

#endif // THYLACINE_MM_BUDDY_H
