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

// Public allocation API per ARCHITECTURE.md §6.3.
struct page *alloc_pages(unsigned order, unsigned flags);
void free_pages(struct page *p, unsigned order);
struct page *alloc_pages_node(int node, unsigned order, unsigned flags);

void *kpage_alloc(unsigned flags);   // single 4 KiB page; returns PA-as-void*
void  kpage_free(void *p);

#endif // THYLACINE_MM_PHYS_H
