// KObj_DMA impl (P4-Ic5b1b) — kernel-allocated contiguous DMA buffers.
//
// Per <thylacine/dma_handle.h> + specs/handles.tla. Unlike KObj_MMIO
// (which tracks a PA range that's external to the kernel allocator),
// KObj_DMA wraps a buddy-allocated page chunk — the page allocator's
// per-allocation partitioning IS the claim layer for HwResourceExclusive.
// No g_dma_claims table is needed.
//
// Lifecycle:
//   - kobj_dma_create(size): SLUB-alloc the struct, alloc_pages(order, KP_ZERO)
//     for the page chunk, compute pa = page_to_pa(pages), refcount=1.
//   - kobj_dma_ref / kobj_dma_unref: atomic refcount ops.
//   - On final unref: free_pages(pages, order) + clobber magic + kfree(struct).
//
// PA stability: once set in kobj_dma_create, the pa field is read-only.
// No code path mutates it; the structural property pins
// specs/SPEC-TO-CODE.md's "PA stable across handle lifetime" commitment
// for KObj_DMA.

#include <thylacine/dma_handle.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/types.h>

#include "../arch/arm64/uart.h"
#include "../mm/phys.h"
#include "../mm/slub.h"

static u64  g_dma_created;
static u64  g_dma_live;
static bool g_dma_initialized;

u64 kobj_dma_total_created(void) {
    return __atomic_load_n(&g_dma_created, __ATOMIC_RELAXED);
}

u64 kobj_dma_live_count(void) {
    return __atomic_load_n(&g_dma_live, __ATOMIC_RELAXED);
}

// =============================================================================
// Init.
// =============================================================================

void kobj_dma_init(void) {
    // Atomic init guard mirrors kobj_mmio_init's discipline (R9 F151 close).
    // Two CPUs racing on a hypothetical future per-CPU subsystem_init would
    // both pass a plain bool check; the exchange returns the prior value
    // and only one observes FALSE.
    if (__atomic_exchange_n(&g_dma_initialized, true, __ATOMIC_ACQ_REL)) {
        extinction("kobj_dma_init called twice");
    }

    uart_puts("kobj_dma: max=");
    uart_puthex64(KOBJ_DMA_MAX_SIZE);
    uart_puts(" bytes (order ");
    // Compute log2(KOBJ_DMA_MAX_SIZE / PAGE_SIZE) for the operator-visible
    // ceiling.
    unsigned order = 0;
    size_t pages = KOBJ_DMA_MAX_SIZE / PAGE_SIZE;
    while (pages > 1) { pages >>= 1; order++; }
    uart_putdec((u64)order);
    uart_puts(" pages)\n");
}

// =============================================================================
// Helpers.
// =============================================================================

// Compute the smallest buddy order such that 2^order pages >= page_count.
// Mirror of burrow.c's order_for_pages; kept local to avoid coupling
// the two TUs through a shared header.
static unsigned order_for_pages(size_t page_count) {
    unsigned order = 0;
    size_t n = 1;
    while (n < page_count) {
        n <<= 1;
        order++;
    }
    return order;
}

// =============================================================================
// Lifecycle.
// =============================================================================

// Shared construction body. `max_size` is the per-subtype envelope
// (KOBJ_DMA_MAX_SIZE for general DMA; KOBJ_DMA_WEAVE_MAX_SIZE for a weave);
// `weave` is the G-2 kernel-minted device-passive subtype bit, set here ONCE
// and never written again (create-immutable, like `pa`).
static struct KObj_DMA *dma_create_body(size_t size, size_t max_size, bool weave) {
    if (!g_dma_initialized)               return NULL;
    if (size == 0)                        return NULL;

    // Page-align size up. Overflow-guard: size near SIZE_MAX would wrap
    // when added to PAGE_SIZE-1. Reject pathological requests rather
    // than silently producing a tiny allocation.
    if (size > SIZE_MAX - (PAGE_SIZE - 1)) return NULL;
    size_t aligned_size = (size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);

    if (aligned_size > max_size)           return NULL;

    size_t page_count = aligned_size / PAGE_SIZE;
    unsigned order = order_for_pages(page_count);

    // Allocate the struct first. If page alloc fails below we kfree this
    // before returning NULL.
    struct KObj_DMA *k = kmalloc(sizeof(*k), KP_ZERO);
    if (!k) return NULL;

    // Allocate the page chunk. KP_ZERO so the driver sees zeroed memory
    // (matches the security expectation that DMA-reachable pages don't
    // carry residual data from prior users — defense against driver-bug
    // info-leak through descriptor padding etc.). For a weave the zeroing
    // additionally guarantees a client's first map never sees another
    // surface's stale pixels.
    struct page *pages = alloc_pages(order, KP_ZERO);
    if (!pages) {
        kfree(k);
        return NULL;
    }

    k->magic = KOBJ_DMA_MAGIC;
    k->pa    = page_to_pa(pages);
    k->size  = aligned_size;
    k->pages = pages;
    k->order = order;
    k->ref   = 1;
    k->weave = weave;

    __atomic_fetch_add(&g_dma_created, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_dma_live,    1u, __ATOMIC_RELAXED);
    return k;
}

struct KObj_DMA *kobj_dma_create(size_t size) {
    return dma_create_body(size, KOBJ_DMA_MAX_SIZE, false);
}

struct KObj_DMA *kobj_dma_create_weave(size_t size) {
    return dma_create_body(size, KOBJ_DMA_WEAVE_MAX_SIZE, true);
}

void kobj_dma_ref(struct KObj_DMA *k) {
    if (!k)                              extinction("kobj_dma_ref(NULL)");
    if (k->magic != KOBJ_DMA_MAGIC)      extinction("kobj_dma_ref of corrupted KObj_DMA");

    // Atomic ref bump (mirrors kobj_mmio_ref's R9 F148 close). The
    // returned old value catches "ref was already 0" — extinct after
    // the fact rather than silently allow a resurrected object.
    int old = __atomic_fetch_add(&k->ref, 1, __ATOMIC_RELAXED);
    if (old <= 0) {
        extinction("kobj_dma_ref of zero-ref KObj_DMA (already freed?)");
    }
}

static void kobj_dma_free_internal(struct KObj_DMA *k) {
    if (k->magic != KOBJ_DMA_MAGIC)
        extinction("kobj_dma_free_internal of corrupted KObj_DMA");
    if (k->ref != 0)
        extinction("kobj_dma_free_internal with ref > 0");
    if (!k->pages)
        extinction("kobj_dma_free_internal with NULL pages (double-free?)");

    free_pages(k->pages, k->order);
    k->pages = NULL;

    // Defensive: clobber magic before kfree so any stale-pointer
    // dereference between free and SLUB-list-write extincts on the
    // magic check.
    k->magic = 0;

    kfree(k);
    __atomic_fetch_sub(&g_dma_live, 1u, __ATOMIC_RELAXED);
}

void kobj_dma_unref(struct KObj_DMA *k) {
    if (!k) return;
    if (k->magic != KOBJ_DMA_MAGIC)
        extinction("kobj_dma_unref of corrupted KObj_DMA");

    // Atomic ref decrement with ACQ_REL ordering (R9 F148 discipline).
    // The release on the dec ensures prior accesses to *k happen before
    // the dec is observed; the acquire ensures the post-dec
    // free_internal sees the final state coherently. Only the caller
    // that observed old==1 (dec was 1→0) calls free_internal.
    int old = __atomic_fetch_sub(&k->ref, 1, __ATOMIC_ACQ_REL);
    if (old <= 0) {
        extinction("kobj_dma_unref of zero-ref KObj_DMA (double-free?)");
    }
    if (old == 1) {
        kobj_dma_free_internal(k);
    }
}

void kobj_dma_destroy(struct KObj_DMA *k) {
    kobj_dma_unref(k);
}
