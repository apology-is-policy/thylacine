// Physical allocator coordinator. Wires the buddy zone and per-CPU
// magazines together, performs DTB-driven bootstrap, and exposes the
// public allocation API per ARCHITECTURE.md §6.3.
//
// At P1-D, kpage_alloc returns a void* that's a cast load PA — TTBR0
// identity-maps the low 4 GiB so the kernel can deference it directly.
// Phase 2 will introduce the kernel direct map at 0xFFFF_0000_*; the
// API stays the same, but kpage_alloc will return a high-VA pointer.

#ifndef THYLACINE_MM_PHYS_H
#define THYLACINE_MM_PHYS_H

#include <thylacine/page.h>
#include <thylacine/types.h>

// Bring up the physical allocator. Reads the DTB-discovered RAM
// range, computes reservations (low-firmware area, kernel image,
// struct page array, DTB blob), initializes the single zone, and
// pushes the free regions onto the buddy. Then initializes
// magazines.
//
// Returns true on success; false if the DTB isn't ready or something
// in the layout doesn't fit.
bool phys_init(void);

// Diagnostic accessors used by the boot banner.
u64 phys_total_pages(void);     // total pages in the zone
u64 phys_free_pages(void);      // currently free pages (across all orders)
u64 phys_reserved_pages(void);  // total - free, computed at init

// #808: page table pages consumed by the boot-time direct-map page-map
// (mmu_pagemap_directmap, driven from phys_init). Boot-banner diagnostic.
u64 phys_directmap_table_pages(void);

// Buddy zone bounds [base, end) after the direct-map 8 GiB-reach cap.
// NULL args are skipped. Used by the #808 page-map sweep test.
void phys_zone_bounds(paddr_t *base, paddr_t *end);

// =============================================================================
// B-1a' (ARCH 6.5 "Capacity, and the I-32 default"): the user pool.
// =============================================================================
//
// The memory bar: a program is never refused memory while free memory exists.
// So the I-32 default budget is the whole machine but a reserve -- RAM minus
// the TCB's working set (the Larder page cache, DMA, the slabs; NOT the Image
// cache, which is pool memory) -- and that same figure is every Proc's hard
// maximum. A per-Proc
// budget alone cannot keep the bar's other half (the TCB keeps running under a
// memory bomb): N Procs each within the default would together take the
// reserve. The POOL is the machine-wide bound, and it is PHYSICAL: it counts
// the pages allocated for users -- data pages, pagemap nodes, page tables --
// charged here at allocation (alloc_user_pages) and returned at the ONE place
// a page can leave (free_pages), so a page that lives is counted and a page
// that is freed is not, whoever frees it and under whatever counter it was
// held. A NON-exempt allocation is refused when it would take the pool past
// RAM minus the reserve; an exempt one (the TCB) is counted but never refused,
// which is what the reserve is for. There is no OOM victim selection: a
// refused allocation fails the syscall or the fault (proc_fault_terminate) of
// the Proc that asked, never a bystander.
//
// The per-address-space count (AddrSpace.page_count) keeps the HOLDER reading
// -- every space that maps a page counts it, so a COW-shared page is charged
// to both sharers and a fork is bounded by the cap -- while the pool counts
// the page once, as the buddy does. That is the Linux memcg shape, and it is
// what keeps a fork of a large process from being refused while free memory
// exists: the clone costs the pool only its node mirror (audit F5).
//
// The reserve is max(CAPACITY_RESERVE_MIN_PAGES, total / 8), clamped to half
// the machine so a tiny guest still boots with a usable pool. 256 MiB is the
// TCB's measured ceiling with headroom: the Larder caches at most 128 MiB
// (LARDER_PAGE_ENTRIES), the DMA envelope 64 MiB, the slabs, kernel stacks and
// the L0 root (one per thread / address space -- the thread and child axes),
// DMA buffers (the warden's allowance, I-34), the direct map's tables. The
// Image cache is NOT reserve memory: a cached file's pages are user pages,
// charged to the pool like every other, and they are what the pool reclaims
// under pressure -- an idle image (cached, mapped by no one) is stripped of
// its pages before a user allocation is refused, the way Plan 9's imagereclaim
// runs when the page pool runs low and Linux's page cache gives way to
// anonymous demand (capacity_set_reclaim, image_cache_reclaim). The counters
// are u32 pages (16 TiB), like page_count.
#define CAPACITY_RESERVE_MIN_PAGES  65536u    // 256 MiB at 4 KiB

void capacity_init(void);           // after phys_init, before the first Proc
u32  capacity_pool_pages(void);     // RAM minus the reserve: the default budget and the hard max
u32  capacity_pool_charged(void);   // user pages allocated and not yet freed, exempt included
u32  capacity_reserve_pages(void);  // the TCB reserve

// A user allocation: charged to the pool (refused past it unless `exempt`),
// then allocated like alloc_pages, then tagged PG_USER so free_pages returns
// the charge. NULL, with nothing charged, when the pool or the buddy refuses.
struct page *alloc_user_pages(unsigned order, unsigned flags, bool exempt);

// The reclaim step under pressure (B-1a' audit F8): when a NON-exempt
// allocation would take the pool past its bound, alloc_user_pages calls the
// registered function with the pages it needs and retries the charge if it
// freed any; it gives up (NULL) when nothing is registered or nothing was
// reclaimable. The Image cache registers image_cache_reclaim at init. Runs in
// the allocating context, under whatever that holds (as->lock for a fault or
// an attach): the function must take no lock an allocator's caller can hold,
// and must allocate nothing.
typedef u32 (*capacity_reclaim_fn)(u32 npages);
void capacity_set_reclaim(capacity_reclaim_fn fn);

// Tests only: hold `npages` of the pool without allocating (an exempt charge,
// never refused), and give them back.
void capacity_pool_park_for_test(u32 npages);
void capacity_pool_unpark_for_test(u32 npages);
// The runner releases whatever a test left parked, after every test, and
// reddens a test that passed while leaking it; answers the pages released.
u32 capacity_pool_unpark_all_for_test(void);

// Public allocation API per ARCHITECTURE.md §6.3.
struct page *alloc_pages(unsigned order, unsigned flags);
void free_pages(struct page *p, unsigned order);
struct page *alloc_pages_node(int node, unsigned order, unsigned flags);

void *kpage_alloc(unsigned flags);   // single 4 KiB page; returns PA-as-void*
void  kpage_free(void *p);

#endif // THYLACINE_MM_PHYS_H
