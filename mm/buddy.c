// Buddy allocator — Knuth (1973).
//
// One zone per NUMA node (single zone at v1.0). Power-of-two free
// lists at orders 0..MAX_ORDER (4 KiB to 1 GiB). Doubly-linked free
// lists with embedded next/prev pointers in struct page (so removing
// an arbitrary buddy during merge is O(1)).
//
// Buddy math: for a block at PFN `p` of order `k`, its buddy lives at
// PFN `p ^ (1 << k)`. The "left" buddy of a pair is the one with the
// lower PFN (i.e., bit k of PFN clear). Merging combines two buddies
// at order k into a single block at order k+1, anchored on the left
// buddy's PFN.
//
// Layered behind per-CPU magazines (mm/magazines.c) for hot-path
// lock-freedom on the common orders. The buddy lock is acquired only
// on magazine refill / drain plus rare large allocations.
//
// Per ARCHITECTURE.md §6.3.

#include "buddy.h"

#include <thylacine/page.h>
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

// Single zone for v1.0.
struct buddy_zone g_zone0;

// PFN <-> struct page array globals (declared in page.h). Set by
// buddy_zone_init.
struct page *_g_struct_pages;
paddr_t _g_zone_base_pfn;

// ---------------------------------------------------------------------------
// Doubly-linked list ops on the per-order free list. The sentinel head
// is itself a struct page (only next/prev fields are used). On an
// empty list, head->next == head->prev == head.
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

static inline struct page *list_pop_front(struct page *head) {
    if (head->next == head) return NULL;
    struct page *p = head->next;
    list_remove(p);
    return p;
}

// ---------------------------------------------------------------------------
// Buddy math.
// ---------------------------------------------------------------------------

static inline paddr_t buddy_pfn_of(paddr_t pfn, unsigned order) {
    return pfn ^ (1ull << order);
}

static inline bool pfn_in_zone(const struct buddy_zone *zone, paddr_t pfn) {
    paddr_t pa = pfn << PAGE_SHIFT;
    return pa >= zone->base_pa && pa < zone->end_pa;
}

// Largest order N such that (1 << N) <= n. Returns -1 for n == 0.
static inline int floor_log2_u64(u64 n) {
    if (n == 0) return -1;
    return 63 - __builtin_clzll(n);
}

// Maximum alignment of a PFN expressed as the largest order it's
// aligned to. PFN 0 is "infinitely aligned" — we cap at MAX_ORDER.
static inline int pfn_max_align(paddr_t pfn) {
    if (pfn == 0) return MAX_ORDER;
    int ctz = __builtin_ctzll(pfn);
    return ctz > MAX_ORDER ? MAX_ORDER : ctz;
}

// ---------------------------------------------------------------------------
// Zone init: clears struct page array, initializes free lists empty.
// ---------------------------------------------------------------------------

void buddy_zone_init(struct buddy_zone *zone, paddr_t base_pa, paddr_t end_pa,
                     struct page *struct_pages) {
    zone->base_pa     = base_pa;
    zone->end_pa      = end_pa;
    zone->num_pages   = (end_pa - base_pa) >> PAGE_SHIFT;
    spin_lock_init(&zone->lock);

    _g_struct_pages   = struct_pages;
    _g_zone_base_pfn  = base_pa >> PAGE_SHIFT;

    // Mark every page PG_RESERVED initially. phys_init's
    // buddy_free_region calls flip the to-be-free pages.
    for (u64 i = 0; i < zone->num_pages; i++) {
        struct_pages[i].next     = NULL;
        struct_pages[i].prev     = NULL;
        struct_pages[i].order    = 0;
        struct_pages[i].flags    = PG_RESERVED;
        struct_pages[i].refcount = 0;
        struct_pages[i]._pad     = 0;
    }

    // Initialize per-order free lists empty. Sentinel heads point at
    // themselves.
    for (int k = 0; k <= MAX_ORDER; k++) {
        list_init_head(&zone->free_lists[k]);
        zone->free_pages_per_order[k] = 0;
    }
    zone->total_free_pages = 0;
}

// ---------------------------------------------------------------------------
// Bulk free: chop a contiguous range into largest-possible power-of-2
// chunks and put each on the appropriate free list.
//
// Greedy algorithm: from the current PFN, the largest valid order is
// min(alignment_of(pfn), floor_log2(end_pfn - pfn), MAX_ORDER). Free
// at that order; advance.
// ---------------------------------------------------------------------------

static void zone_free_chunk(struct buddy_zone *zone, paddr_t pfn, int order) {
    struct page *p = pfn_to_page(pfn);
    p->order    = (u32)order;
    p->flags    = PG_FREE;
    p->refcount = 0;
    list_push_front(&zone->free_lists[order], p);
    zone->free_pages_per_order[order]++;
    zone->total_free_pages += (1ull << order);
}

void buddy_free_region(struct buddy_zone *zone, paddr_t start_pa, paddr_t end_pa) {
    if (start_pa >= end_pa) return;

    irq_state_t s = spin_lock_irqsave(&zone->lock);

    paddr_t pfn     = start_pa >> PAGE_SHIFT;
    paddr_t end_pfn = end_pa   >> PAGE_SHIFT;

    while (pfn < end_pfn) {
        int max_align = pfn_max_align(pfn);
        int max_size  = floor_log2_u64(end_pfn - pfn);
        if (max_size  > MAX_ORDER) max_size = MAX_ORDER;
        int order = max_align < max_size ? max_align : max_size;
        if (order < 0) order = 0;

        zone_free_chunk(zone, pfn, order);
        pfn += 1ull << order;
    }

    spin_unlock_irqrestore(&zone->lock, s);
}

// ---------------------------------------------------------------------------
// Allocate. Caller holds no lock; we acquire/release zone->lock here.
// ---------------------------------------------------------------------------

static struct page *alloc_locked(struct buddy_zone *zone, unsigned order) {
    if (order > MAX_ORDER) return NULL;

    // Find the smallest order >= request that has a free block.
    unsigned src_order = order;
    while (src_order <= MAX_ORDER && list_empty(&zone->free_lists[src_order])) {
        src_order++;
    }
    if (src_order > MAX_ORDER) return NULL;       // OOM

    struct page *p = list_pop_front(&zone->free_lists[src_order]);
    zone->free_pages_per_order[src_order]--;
    zone->total_free_pages -= (1ull << src_order);

    // Split down: while src_order > order, halve the block and put
    // the right buddy back on the lower-order free list. The left
    // buddy (= our `p`) stays.
    while (src_order > order) {
        src_order--;
        paddr_t pfn   = page_to_pfn(p);
        paddr_t bpfn  = pfn | (1ull << src_order);    // right buddy
        struct page *bp = pfn_to_page(bpfn);
        bp->order    = src_order;
        bp->flags    = PG_FREE;
        bp->refcount = 0;
        list_push_front(&zone->free_lists[src_order], bp);
        zone->free_pages_per_order[src_order]++;
        zone->total_free_pages += (1ull << src_order);
    }

    p->order    = order;
    p->flags    = PG_KERNEL;
    p->refcount = 1;
    p->next     = NULL;
    p->prev     = NULL;
    return p;
}

struct page *buddy_alloc(struct buddy_zone *zone, unsigned order) {
    irq_state_t s = spin_lock_irqsave(&zone->lock);
    struct page *p = alloc_locked(zone, order);
    spin_unlock_irqrestore(&zone->lock, s);
    return p;
}

// ---------------------------------------------------------------------------
// Free. Iteratively merge with buddy if buddy is free at the same order.
// ---------------------------------------------------------------------------

static void free_locked(struct buddy_zone *zone, struct page *p, unsigned order) {
    paddr_t pfn = page_to_pfn(p);

    while (order < MAX_ORDER) {
        paddr_t bpfn = buddy_pfn_of(pfn, order);
        if (!pfn_in_zone(zone, bpfn)) break;
        struct page *bp = pfn_to_page(bpfn);
        if (!(bp->flags & PG_FREE) || bp->order != order) break;

        // Buddy is free at the same order — merge.
        list_remove(bp);
        zone->free_pages_per_order[order]--;
        zone->total_free_pages -= (1ull << order);
        bp->flags = 0;
        bp->order = 0;

        // Anchor on the left (lower-PFN) buddy.
        if (bpfn < pfn) {
            pfn = bpfn;
            p   = bp;
        }
        order++;
    }

    p->order    = order;
    p->flags    = PG_FREE;
    p->refcount = 0;
    list_push_front(&zone->free_lists[order], p);
    zone->free_pages_per_order[order]++;
    zone->total_free_pages += (1ull << order);
}

void buddy_free(struct buddy_zone *zone, struct page *p, unsigned order) {
    irq_state_t s = spin_lock_irqsave(&zone->lock);
    free_locked(zone, p, order);
    spin_unlock_irqrestore(&zone->lock, s);
}
