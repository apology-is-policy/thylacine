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

#include <thylacine/dtb.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/types.h>

extern volatile u64 _saved_dtb_ptr;

// Snapshot of the layout for diagnostic queries (banner).
static u64 g_total_pages;
static u64 g_initial_free_pages;

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
    paddr_t zone_base = (paddr_t)mem_base;
    paddr_t zone_end  = (paddr_t)(mem_base + mem_size);

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

    // Sanity: every reservation must lie within the zone.
    if (kern_pa_start < zone_base || kern_pa_end > zone_end ||
        struct_pages_pa_end > zone_end ||
        dtb_pa_start < zone_base || dtb_pa_end > zone_end) {
        extinction("phys_init: reservation outside DTB-discovered RAM");
    }

    // F34 (audit-r3): make the low-firmware reservation EXPLICIT in
    // the array rather than threading it through cursor manipulation.
    // F29 (audit-r3): verify no overlap among the four reservations
    // — on Pi 5 with 8 GiB RAM the struct_pages array is ~96 MiB and
    // can collide with the DTB blob the firmware placed; collision
    // would cause buddy_zone_init's clear pass to overwrite the DTB.
    struct reservation res[4] = {
        { zone_base,             kern_pa_start },        // low firmware
        { kern_pa_start,         kern_pa_end },          // kernel image
        { struct_pages_pa_start, struct_pages_pa_end },  // struct page array
        { dtb_pa_start,          dtb_pa_end },           // DTB blob
    };
    sort_by_start(res, 4);
    if (!reservations_disjoint(res, 4)) {
        extinction("phys_init: reservation overlap "
                   "(DTB collides with kernel/struct_pages?)");
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

    for (int i = 0; i < 4; i++) {
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
