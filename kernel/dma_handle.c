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
//     for each backing run, record pa = page_to_pa(pages), refcount=1.
//   - kobj_dma_ref / kobj_dma_unref: atomic refcount ops.
//   - On final unref: free every block + clobber magic + kfree(struct).
//
// WEAVE-SKEIN (docs/WEAVE-SKEIN-DESIGN.md): "the page chunk" is a LIST of
// chunks. A weave above SKEIN_BLOCK is backed by N 2 MiB runs instead of one
// power-of-two span, because a single span of a 48.8 MiB weave means an
// order-14 (64 MiB, naturally aligned) buddy allocation that fails with
// 1889 MiB free. Plain DMA and GPU BOs still take exactly one block, so their
// behaviour is byte-identical to before. Everything downstream reaches the
// backing through kobj_dma_pa_at, which handles nblk == 1 as the same case.
//
// PA stability: once set in kobj_dma_create, blk[] and nblk are read-only.
// No code path mutates them; the structural property pins
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

// WEAVE-SKEIN: free the first `n` blocks of a partially-built skein. The
// unwind path for a mid-skein alloc failure AND the tail of the normal free,
// so there is exactly one loop that returns blocks to the buddy.
static void dma_free_blocks(struct KObj_DMA *k, u32 n) {
    for (u32 i = 0; i < n; i++) {
        if (k->blk[i].pages) {
            free_pages(k->blk[i].pages, k->blk[i].order);
            k->blk[i].pages = NULL;
            k->blk[i].pa    = 0;
        }
    }
}

// The buffer distance between consecutive blocks.
//
// SKEIN_BLOCK for a real skein -- but a SINGLE-block object's one block spans
// the WHOLE buffer, and that is not a degenerate case: plain DMA is capped at
// 1 MiB, but a GPU BO is single-block BY DESIGN with a 64 MiB envelope, so
// `nblk == 1 && size > SKEIN_BLOCK` is ordinary. Dividing such an object's
// offsets by SKEIN_BLOCK yields an index past nblk and refuses every byte
// after the first 2 MiB, i.e. a client faults on most of its own buffer.
//
// Derived rather than stored: a stored stride is a second source of truth
// about the same fact, and the two can drift.
static inline u64 skein_stride(const struct KObj_DMA *k) {
    return (k->nblk == 1) ? (u64)k->size : SKEIN_BLOCK;
}

u64 kobj_dma_block_len(const struct KObj_DMA *k, u32 i) {
    if (!k || k->magic != KOBJ_DMA_MAGIC) return 0;
    if (i >= k->nblk)                     return 0;

    // The block's own allocation, and the buffer bytes it is responsible for.
    // The two differ only on the LAST block, whose allocation is rounded up to
    // its buddy order while the buffer ends mid-block. Report the smaller: a
    // segment handed to a device must not name pages past the buffer's end,
    // and the sum over all blocks must equal size (the copy-out relies on it).
    u64 alloc_bytes = (u64)PAGE_SIZE << k->blk[i].order;
    u64 covered     = (u64)i * skein_stride(k);
    if (covered >= k->size)                 return 0;   // structurally impossible
    u64 remaining   = (u64)k->size - covered;
    return alloc_bytes < remaining ? alloc_bytes : remaining;
}

u64 kobj_dma_pa_at(const struct KObj_DMA *k, u64 byte_off) {
    if (!k || k->magic != KOBJ_DMA_MAGIC) return 0;
    if (byte_off >= (u64)k->size)         return 0;

    // Uniform stride, so the block index is a division rather than a walk --
    // but the stride is a property of the OBJECT, not the constant (see
    // skein_stride: a single-block object's one block spans the whole buffer,
    // which for a GPU BO can be 64 MiB).
    u64 stride = skein_stride(k);
    u64 idx = byte_off / stride;
    u64 off = byte_off % stride;
    if (idx >= (u64)k->nblk) return 0;

    // Bound the offset against the resolved block's OWN allocation, not just
    // against k->size. For a well-formed object the two agree; checking the
    // block is what makes a malformed skein fail closed instead of handing out
    // a PA past the block's pages.
    if (off >= ((u64)PAGE_SIZE << k->blk[idx].order)) return 0;
    if (!k->blk[idx].pages)                          return 0;

    return k->blk[idx].pa + off;
}

// =============================================================================
// Lifecycle.
// =============================================================================

// The kernel-minted DMA subtype, chosen at create and immutable thereafter.
// An enum rather than bool params so a caller cannot construct the
// weave-AND-gpu_bo state (each mint sets exactly one bit, or neither).
enum dma_subtype {
    DMA_SUBTYPE_PLAIN,
    DMA_SUBTYPE_WEAVE,     // G-2: device-READ framebuffer class
    DMA_SUBTYPE_GPU_BO,    // Warp-2: device-WRITTEN GPU buffer class
};

// Shared construction body. `max_size` is the per-subtype envelope
// (KOBJ_DMA_MAX_SIZE general; KOBJ_DMA_WEAVE_MAX_SIZE / KOBJ_DMA_GPU_BO_
// MAX_SIZE per subtype); the subtype bit is set here ONCE and never written
// again (create-immutable, like `pa`).
static struct KObj_DMA *dma_create_body(size_t size, size_t max_size,
                                        enum dma_subtype subtype) {
    if (!g_dma_initialized)               return NULL;
    if (size == 0)                        return NULL;

    // Page-align size up. Overflow-guard: size near SIZE_MAX would wrap
    // when added to PAGE_SIZE-1. Reject pathological requests rather
    // than silently producing a tiny allocation.
    if (size > SIZE_MAX - (PAGE_SIZE - 1)) return NULL;
    size_t aligned_size = (size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);

    if (aligned_size > max_size)           return NULL;

    size_t page_count = aligned_size / PAGE_SIZE;

    // WEAVE-SKEIN: how many contiguous runs back this object.
    //
    // Only the WEAVE subtype scatters, and only above one block. Plain DMA
    // stays single-block DELIBERATELY -- a virtqueue descriptor table must be
    // contiguous because the device walks it by address with no length list to
    // consult -- which keeps virtio-net/blk and every ring allocation entirely
    // out of this change's blast radius. GPU BOs likewise stay single-block at
    // this chunk's ratified scope.
    //
    // A weave that fits in one block also takes the single-block path, so a
    // small weave is not rounded up to a full SKEIN_BLOCK (the kernel test
    // suite mints 2-page weaves; padding those to 2 MiB would be a 256x waste
    // for no gain).
    u32 nblk = 1;
    if (subtype == DMA_SUBTYPE_WEAVE && aligned_size > SKEIN_BLOCK) {
        nblk = (u32)((aligned_size + SKEIN_BLOCK - 1) / SKEIN_BLOCK);
    }
    // Envelope-derived, so unreachable for any admitted size -- asserted
    // rather than assumed, because the array is fixed and an overrun here
    // would be a kernel-memory write.
    if (nblk > KOBJ_DMA_MAX_BLOCKS) return NULL;

    // Allocate the struct first. If page alloc fails below we kfree this
    // before returning NULL.
    struct KObj_DMA *k = kmalloc(sizeof(*k), KP_ZERO);
    if (!k) return NULL;

    // Allocate the backing runs. KP_ZERO so the driver sees zeroed memory
    // (matches the security expectation that DMA-reachable pages don't
    // carry residual data from prior users — defense against driver-bug
    // info-leak through descriptor padding etc.). For a weave the zeroing
    // additionally guarantees a client's first map never sees another
    // surface's stale pixels.
    //
    // Every block but the last spans exactly SKEIN_BLOCK_PAGES -- the uniform
    // stride kobj_dma_pa_at divides by. The last is sized to its OWN order
    // rather than padded to a full block: the fault-path formula is unaffected
    // (the tail is last, so no later block's index depends on its length) and
    // the per-block order is stored anyway for free_pages, so the saving is
    // free. For the 2560x1664 weave that is 24 x 2 MiB + 1 MiB = 49 MiB
    // against 48.75 MiB requested -- 0.5% waste, where one span would have
    // demanded 64 MiB contiguous and failed.
    for (u32 i = 0; i < nblk; i++) {
        size_t done  = (size_t)i * (size_t)SKEIN_BLOCK_PAGES;
        size_t want  = (nblk == 1) ? page_count
                                   : (page_count - done < (size_t)SKEIN_BLOCK_PAGES
                                          ? page_count - done
                                          : (size_t)SKEIN_BLOCK_PAGES);
        unsigned order = order_for_pages(want);

        struct page *pages = alloc_pages(order, KP_ZERO);
        if (!pages) {
            // Unwind the blocks already taken. Without this a mid-skein OOM
            // would strand up to 62 MiB in the buddy for the object's life --
            // and the object is never returned, so nothing would ever free it.
            dma_free_blocks(k, i);
            kfree(k);
            return NULL;
        }
        k->blk[i].pages = pages;
        k->blk[i].order = order;
        k->blk[i].pa    = page_to_pa(pages);
    }

    k->magic  = KOBJ_DMA_MAGIC;
    k->size   = aligned_size;
    k->nblk   = nblk;
    k->ref    = 1;
    k->weave  = (subtype == DMA_SUBTYPE_WEAVE);
    k->gpu_bo = (subtype == DMA_SUBTYPE_GPU_BO);

    __atomic_fetch_add(&g_dma_created, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_dma_live,    1u, __ATOMIC_RELAXED);
    return k;
}

struct KObj_DMA *kobj_dma_create(size_t size) {
    return dma_create_body(size, KOBJ_DMA_MAX_SIZE, DMA_SUBTYPE_PLAIN);
}

struct KObj_DMA *kobj_dma_create_weave(size_t size) {
    return dma_create_body(size, KOBJ_DMA_WEAVE_MAX_SIZE, DMA_SUBTYPE_WEAVE);
}

struct KObj_DMA *kobj_dma_create_gpu_bo(size_t size) {
    return dma_create_body(size, KOBJ_DMA_GPU_BO_MAX_SIZE, DMA_SUBTYPE_GPU_BO);
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
    // The skein's block 0 always exists (nblk >= 1 on every constructed
    // object), so a NULL there is the same double-free tell it always was.
    if (k->nblk == 0 || !k->blk[0].pages)
        extinction("kobj_dma_free_internal with NULL pages (double-free?)");

    dma_free_blocks(k, k->nblk);

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
