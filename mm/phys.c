// Physical allocator coordinator + DTB-driven bootstrap.
//
// Layout:
//   [mem_base, _kernel_pa_start)             — RESERVED low firmware region
//   [_kernel_pa_start, _kernel_pa_end)       — RESERVED kernel image
//   [struct_page_pa_start, struct_page_pa_end) — RESERVED struct page array
//   [..., dtb_pa_start)                       — FREE
//   [dtb_pa_start, dtb_pa_end)                — RESERVED DTB blob
//   [dtb_pa_end, mem_end)                     — FREE
//
// We sort the three reservation ranges by start address, then walk
// the gaps between them and call buddy_free_region for each gap.
//
// Per ARCHITECTURE.md §6.3.

#include "phys.h"
#include "buddy.h"
#include "magazines.h"

#include "../arch/arm64/kaslr.h"
#include "../arch/arm64/mmu.h"          // mmu_pagemap_directmap (#808)
#include "../arch/arm64/uart.h"

#include <thylacine/dtb.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/types.h>

extern volatile u64 _saved_dtb_ptr;

// Snapshot of the layout for diagnostic queries (banner).
static u64 g_total_pages;
static u64 g_initial_free_pages;
// #808: page-table pages the boot-time direct-map page-map consumed.
static u64 g_directmap_table_pages;
// Buddy zone bounds (post-cap), retained for diagnostics + the #808 sweep test.
static paddr_t g_zone_base;
static paddr_t g_zone_end;

// F3 (audit-r-memory-model): The kernel direct map (arch/arm64/mmu.c
// build_page_tables) covers PA [1 GiB, 9 GiB) via l1_directmap[1..8].
// alloc_pages's KP_ZERO loop dereferences pa_to_kva(pa) -- which past
// 9 GiB lands at an unmapped VA -> EL1 translation fault ->
// extinction. Cap zone_end at this value to ensure every PA the buddy
// can hand out is direct-map-reachable. To raise the cap, extend the
// direct map first (see arch/arm64/mmu.c build_page_tables and the
// l1_directmap definition).
//
// #808 caveat (Lazarus loose end, dormant at v1.0): the cap below is
// mem_base-RELATIVE (zone_end = mem_base + 8 GiB), but the direct map is
// ABSOLUTE: l1_directmap[1..8] = PA [1 GiB, 9 GiB). They coincide only when
// mem_base == 1 GiB (QEMU virt). If a future board reports mem_base != 1 GiB,
// this relative cap would permit PAs above the absolute 9 GiB ceiling, which
// the direct map cannot reach (the original F3 fault). mmu_pagemap_directmap's
// own gib>8 skip is already absolute-correct, so the boot pass stays safe;
// only this cap is the loose end. At Lazarus board bringup, express the cap as
// the absolute ceiling -- min(mem_base + 8 GiB, 9 GiB) -- or widen the direct
// map. (Pre-existing; surfaced by the #808 audit F2.)
#define DIRECTMAP_USABLE_RAM_MAX (8ull << 30)   /* 8 GiB */

// ---------------------------------------------------------------------------
// Layout helpers.
// ---------------------------------------------------------------------------

struct reservation { paddr_t start; paddr_t end; };

static void sort_by_start(struct reservation *r, int n) {
    // Tiny insertion sort; n elements.
    for (int i = 1; i < n; i++) {
        struct reservation key = r[i];
        int j = i - 1;
        while (j >= 0 && r[j].start > key.start) {
            r[j + 1] = r[j];
            j--;
        }
        r[j + 1] = key;
    }
}

// Check that sorted reservations don't overlap. Audit-r3 F29: on
// machines where firmware places the DTB blob inside the range we
// claim for the struct_pages array (Pi 5 with 8 GiB RAM has ~96 MiB
// of struct_pages — easy to collide), the buddy_zone_init clear pass
// would silently overwrite the DTB. Detect at boot-time before any
// damage is done.
static bool reservations_disjoint(const struct reservation *r, int n) {
    for (int i = 1; i < n; i++) {
        if (r[i].start < r[i - 1].end) return false;
    }
    return true;
}

static inline paddr_t round_up(paddr_t v, paddr_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

static inline paddr_t round_down(paddr_t v, paddr_t a) {
    return v & ~(a - 1);
}

// ---------------------------------------------------------------------------
// phys_init.
// ---------------------------------------------------------------------------

bool phys_init(void) {
    if (!dtb_is_ready()) {
        extinction("phys_init: DTB not ready");
    }

    // 1. Discover RAM.
    u64 mem_base, mem_size;
    if (!dtb_get_memory(&mem_base, &mem_size)) {
        extinction("phys_init: DTB has no /memory node");
    }
    paddr_t zone_base    = (paddr_t)mem_base;
    paddr_t zone_end_dtb = (paddr_t)(mem_base + mem_size);

    // F3: cap zone_end at the direct map's usable upper bound. Without
    // the cap, alloc_pages can return a PA the direct map can't reach
    // and the KP_ZERO loop takes an unhandled EL1 translation fault.
    paddr_t zone_end_cap = (paddr_t)(mem_base + DIRECTMAP_USABLE_RAM_MAX);
    paddr_t zone_end     = zone_end_dtb < zone_end_cap ? zone_end_dtb
                                                       : zone_end_cap;
    if (zone_end < zone_end_dtb) {
        uart_puts("  phys_init: capping RAM at 0x");
        uart_puthex64(zone_end);
        uart_puts(" (direct map reaches 8 GiB; DTB reported 0x");
        uart_puthex64(zone_end_dtb);
        uart_puts(" -- extend l1_directmap to raise the cap)\n");
    }

    // 2. Kernel image PA range — captured by kaslr.c during kaslr_init
    //    while still running at PA. After the long-branch into TTBR1,
    //    PC-relative adrp+add gives high VAs, so we read the cached
    //    values rather than re-deriving here.
    paddr_t kern_pa_start = (paddr_t)kaslr_kernel_pa_start();
    paddr_t kern_pa_end   = round_up((paddr_t)kaslr_kernel_pa_end(), PAGE_SIZE);

    // 3. struct page array placement: just past the kernel image,
    //    page-aligned. Sized for the full zone.
    paddr_t struct_pages_pa_start = kern_pa_end;
    u64 num_pages_total = (zone_end - zone_base) >> PAGE_SHIFT;
    u64 struct_pages_bytes = round_up(num_pages_total * sizeof(struct page),
                                      PAGE_SIZE);
    paddr_t struct_pages_pa_end = struct_pages_pa_start + struct_pages_bytes;

    // 4. DTB blob reservation: [dtb_pa, dtb_pa + dtb_size), page-aligned.
    paddr_t dtb_pa_start = round_down((paddr_t)_saved_dtb_ptr, PAGE_SIZE);
    paddr_t dtb_pa_end   = round_up((paddr_t)_saved_dtb_ptr +
                                    (paddr_t)dtb_get_total_size(),
                                    PAGE_SIZE);

    // 4b. Initrd reservation (P4-E). When QEMU's `-initrd` is in play,
    //     the bootloader records [linux,initrd-start, linux,initrd-end)
    //     in /chosen. The cpio blob lives in this range and devramfs
    //     reads from it after dev_init; if the buddy isn't told to
    //     reserve it, those pages free + get reused, clobbering the
    //     blob bytes that g_ramfs_files[].name + .data point at.
    //     The bug surfaces deterministically once the kernel image grows
    //     enough (e.g., UBSan build) to push buddy allocations past the
    //     low-zone threshold.
    paddr_t initrd_pa_start = 0, initrd_pa_end = 0;
    bool    have_initrd = false;
    {
        u64 s, e;
        if (dtb_get_chosen_initrd(&s, &e)) {
            initrd_pa_start = round_down((paddr_t)s, PAGE_SIZE);
            initrd_pa_end   = round_up((paddr_t)e, PAGE_SIZE);
            have_initrd = true;
        }
    }

    // Sanity: every reservation must lie within the zone.
    if (kern_pa_start < zone_base || kern_pa_end > zone_end ||
        struct_pages_pa_end > zone_end ||
        dtb_pa_start < zone_base || dtb_pa_end > zone_end ||
        (have_initrd && (initrd_pa_start < zone_base ||
                         initrd_pa_end > zone_end))) {
        extinction("phys_init: reservation outside DTB-discovered RAM");
    }

    // F34 (audit-r3): make the low-firmware reservation EXPLICIT in
    // the array rather than threading it through cursor manipulation.
    // F29 (audit-r3): verify no overlap among the reservations.
    // P4-E: append initrd reservation if present.
    struct reservation res[5] = {
        { zone_base,             kern_pa_start },        // low firmware
        { kern_pa_start,         kern_pa_end },          // kernel image
        { struct_pages_pa_start, struct_pages_pa_end },  // struct page array
        { dtb_pa_start,          dtb_pa_end },           // DTB blob
        { 0, 0 },                                        // initrd (filled below)
    };
    int res_count = 4;
    if (have_initrd) {
        res[4].start = initrd_pa_start;
        res[4].end   = initrd_pa_end;
        res_count = 5;
    }
    sort_by_start(res, res_count);
    if (!reservations_disjoint(res, res_count)) {
        extinction("phys_init: reservation overlap "
                   "(DTB collides with kernel/struct_pages/initrd?)");
    }

    // 5. Initialize the buddy zone. struct_pages lives at PA
    //    `struct_pages_pa_start` in physical RAM. Pre-P3-Bda we
    //    addressed it via TTBR0 identity (PA-as-VA); P3-Bda retires
    //    TTBR0 identity, so we now use the kernel direct map (TTBR1
    //    high half at 0xFFFF_0000_*) — pa_to_kva(pa) = pa | base. The
    //    direct-map alias of the struct_pages region is RW + XN at
    //    runtime; reads/writes through it walk via TTBR1's
    //    l1_directmap → l2_directmap_kernel (for the kernel-image GiB)
    //    or l1_directmap[gib] 1 GiB block (for other GiBs).
    struct page *struct_pages =
        (struct page *)pa_to_kva(struct_pages_pa_start);
    buddy_zone_init(&g_zone0, zone_base, zone_end, struct_pages);

    // 6. Free regions = the gaps between sorted reservations.
    //    [zone_base, kern_pa_start) (low firmware) is the first entry
    //    in res[]; we treat it as fully reserved.
    paddr_t cursor = round_up(zone_base, PAGE_SIZE);

    for (int i = 0; i < res_count; i++) {
        if (cursor < res[i].start) {
            buddy_free_region(&g_zone0, cursor, res[i].start);
        }
        if (cursor < res[i].end) cursor = res[i].end;
    }
    if (cursor < zone_end) {
        buddy_free_region(&g_zone0, cursor, zone_end);
    }

    // 7. Initialize per-CPU magazines.
    magazines_init();

    g_total_pages        = num_pages_total;
    g_initial_free_pages = g_zone0.total_free_pages;
    g_zone_base          = zone_base;
    g_zone_end           = zone_end;

    // 8. #808: page-map the buddy direct map to L3 granularity now. We are
    //    single-CPU and IRQ-masked (boot_main has not yet run `msr daifclr,
    //    #2`), the buddy is live (it allocates the L2/L3 tables), and the
    //    first thread_create is still ahead -- the exact window where the
    //    block->table demotes are safe by construction. Afterwards the
    //    runtime kstack-guard path (mmu_set_no_access_range) only flips
    //    already-present L3 leaves, so it never does a break-before-make:
    //    the #806 same-CPU IRQ-during-BBM race AND its cross-CPU sibling are
    //    eliminated for the buddy zone. The table cost is the free-page
    //    delta (boot-banner diagnostic via phys_directmap_table_pages).
    u64 free_before_pagemap = g_zone0.total_free_pages;
    mmu_pagemap_directmap(zone_base, zone_end);
    g_directmap_table_pages = free_before_pagemap - g_zone0.total_free_pages;

    return true;
}

// ---------------------------------------------------------------------------
// Diagnostic accessors.
// ---------------------------------------------------------------------------

u64 phys_total_pages(void) {
    return g_total_pages;
}

u64 phys_free_pages(void) {
    return g_zone0.total_free_pages;
}

u64 phys_reserved_pages(void) {
    return g_total_pages - g_initial_free_pages;
}

u64 phys_directmap_table_pages(void) {
    return g_directmap_table_pages;
}

void phys_zone_bounds(paddr_t *base, paddr_t *end) {
    if (base) *base = g_zone_base;
    if (end)  *end  = g_zone_end;
}

// ---------------------------------------------------------------------------
// Public alloc / free API.
// ---------------------------------------------------------------------------

struct page *alloc_pages(unsigned order, unsigned flags) {
    struct page *p = mag_alloc(order);
    if (!p) {
        // Magazine miss (or order isn't magazine-managed). Buddy direct.
        p = buddy_alloc(&g_zone0, order);
    }
    if (!p) return NULL;

    if (flags & KP_ZERO) {
        // P3-Bb: zero the allocated region via the kernel direct map.
        // Pre-P3-Bb this used TTBR0 identity (PA-as-VA); now we use
        // the high-VA direct map at KERNEL_DIRECT_MAP_BASE so the
        // zeroing loop is unaffected by future TTBR0 changes.
        u64 *q = pa_to_kva(page_to_pa(p));
        u64 n  = (1ull << order) << PAGE_SHIFT;
        for (u64 i = 0; i < n / 8; i++) q[i] = 0;

        // F5 (audit-r-memory-model): drain the zeroing stores into the
        // inner-shareable domain before returning the PA to the caller.
        // At single-CPU v1.0 this is benign (store-to-load forwarding
        // serves any same-CPU reload), but at Phase 5+ SMP a second
        // CPU mapping the same PA via a different VA (e.g., the user
        // page-fault handler installs a PTE on another CPU and the
        // user faults in to it) could see pre-zero garbage without
        // this barrier. Trivial cost (single dsb ish per allocation);
        // defense-in-depth.
        __asm__ __volatile__("dsb ish" ::: "memory");
    }
    return p;
}

void free_pages(struct page *p, unsigned order) {
    if (!p) return;
    if (mag_free(p, order)) return;
    buddy_free(&g_zone0, p, order);
}

struct page *alloc_pages_node(int node, unsigned order, unsigned flags) {
    (void)node;     // single zone at v1.0
    return alloc_pages(order, flags);
}

// kpage_alloc returns a void * that's a kernel direct-map VA (P3-Bb).
// Pre-P3-Bb this returned the cast load PA via TTBR0 identity; the
// direct map at 0xFFFF_0000_* now provides a stable high-VA pointer
// independent of TTBR0 contents.
void *kpage_alloc(unsigned flags) {
    struct page *p = alloc_pages(0, flags);
    if (!p) return NULL;
    return pa_to_kva(page_to_pa(p));
}

void kpage_free(void *p) {
    if (!p) return;
    // P3-Bb: convert direct-map KVA to PA for page-frame database lookup.
    paddr_t pa = kva_to_pa(p);
    free_pages(pa_to_page(pa), 0);
}
