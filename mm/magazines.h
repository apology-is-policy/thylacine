// Per-CPU magazines (Bonwick & Adams 2001, illumos kmem) layered in
// front of the buddy allocator. Each CPU has small stacks of pre-
// fetched pages at the most common allocation orders (0 = 4 KiB and
// 9 = 2 MiB). Fast path: pop / push a single page, no buddy lock.
// Slow path: refill on empty, drain on full — both batch operations
// that amortize one buddy-lock acquisition over MAGAZINE_SIZE pages.
//
// At P1-D NCPUS = 1; at P1-F (SMP) NCPUS bumps and my_cpu() reads
// MPIDR_EL1.Aff0. The interface is fixed so callers don't change.
//
// Per ARCHITECTURE.md §6.3.

#ifndef THYLACINE_MM_MAGAZINES_H
#define THYLACINE_MM_MAGAZINES_H

#include <thylacine/page.h>
#include <thylacine/types.h>

// 16 entries per magazine matches Linux's per-CPU page cache default.
#define MAGAZINE_SIZE       16

// Two magazine orders: 4 KiB (the page-allocator default) and 2 MiB
// (matches the L2 block size for kernel large mappings — useful for
// future direct-map / vmalloc allocations).
#define MAG_IDX_ORDER_4K    0
#define MAG_IDX_ORDER_2M    1
#define NUM_MAG_ORDERS      2

struct magazine {
    int count;                              // entries in use [0, MAGAZINE_SIZE]
    struct page *entries[MAGAZINE_SIZE];
};

struct percpu_data {
    struct magazine mags[NUM_MAG_ORDERS];
};

#define NCPUS               1   // P1-D; bumped at P1-F when SMP arrives.

extern struct percpu_data g_percpu[NCPUS];

// Reset all magazines to empty. Called from phys_init.
void magazines_init(void);

// Try to allocate a page via the local magazine. Returns NULL when:
//   - the requested order isn't magazine-managed (caller should fall
//     through to buddy_alloc directly), OR
//   - the magazine was empty and the buddy refill failed (OOM).
struct page *mag_alloc(unsigned order);

// Try to free a page via the local magazine. Returns true when freed
// (caller's job is done); false when order isn't magazine-managed
// (caller should fall through to buddy_free).
bool mag_free(struct page *p, unsigned order);

// Drain ALL per-CPU magazines back to the buddy. Used by the boot
// smoke test for clean accounting; in production this is also useful
// for memory pressure responses (Phase 2+).
void magazines_drain_all(void);

#endif // THYLACINE_MM_MAGAZINES_H
