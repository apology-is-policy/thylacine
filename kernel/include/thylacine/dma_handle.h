// KObj_DMA — kernel-allocated contiguous DMA buffer as a handle-table-managed
// kernel object (P4-Ic5b1b).
//
// Per ARCHITECTURE.md §13 (handles) + §28 I-5 (KObj_DMA non-transferable)
// + specs/handles.tla. A userspace driver process holds a KObj_DMA handle
// naming a contiguous physical-memory range allocated by the kernel for
// the driver's exclusive DMA use. The kernel guarantees:
//
//   1. Exclusivity (specs/handles.tla::HwResourceExclusive): no two
//      drivers can simultaneously hold KObj_DMA handles covering
//      overlapping PA ranges. Pinned structurally by the buddy
//      allocator — each alloc_pages call returns a fresh contiguous
//      chunk; no claim table is needed because the page allocator IS
//      the claim layer (unlike MMIO where userspace specifies a PA and
//      overlap must be rejected via g_mmio_claims).
//
//   2. Non-transferability (specs/handles.tla::HwHandlesAtOrigin): the
//      handle stays with its origin Proc; the 9P transfer path
//      structurally has no KOBJ_DMA case (see kernel/handle.h
//      KOBJ_KIND_HW_MASK + handle_transfer_via_9p switch).
//
//   3. No in-proc duplication (specs/handles.tla::NoHwDup): handle_dup
//      rejects KOBJ_DMA at runtime.
//
//   4. Capability-gated creation (specs/handles.tla::HwHandleImpliesCap):
//      SYS_DMA_CREATE checks `current_proc->caps & CAP_HW_CREATE`
//      before allocating.
//
//   5. PA stability across handle lifetime: once kobj_dma_create returns,
//      KObj_DMA.pa never changes — there is no kernel code path that
//      migrates DMA pages. The structural property (no migrator exists
//      at v1.0) is a design commitment captured in specs/SPEC-TO-CODE.md;
//      the impl-side enforcement is the absence of any pa-mutating code
//      path on a live KObj_DMA.
//
// Distinct from KObj_MMIO:
//   - PA chosen by KERNEL (via alloc_pages) — userspace cannot specify a
//     PA. This eliminates entire bug classes (PA-collision with kernel-
//     reserved ranges, IPS-out-of-bound PAs) at the syscall surface.
//   - Backing is Normal cacheable RAM (not Device memory). CPU + device
//     both access via Normal-WB PTEs; QEMU virt's VirtIO transports are
//     coherent, so no explicit cache maintenance is needed at v1.0.
//   - SLUB-allocated `pages` chunk released to buddy on free.

#ifndef THYLACINE_DMA_HANDLE_H
#define THYLACINE_DMA_HANDLE_H

#include <thylacine/types.h>

struct page;

// KOBJ_DMA_MAGIC — sentinel set at kobj_dma_create; checked at every
// public API entry. Sits at offset 0; SLUB freelist write on free
// clobbers it (use-after-free defense, mirrors kobj_mmio / burrow / irqfwd).
#define KOBJ_DMA_MAGIC 0x444D4100BADC0DEEULL

// Maximum DMA buffer size at v1.0. 1 MiB = 256 pages = buddy order 8.
// Generous for VirtIO queue tables (typical: 8-16 KiB) + indirect tables
// (typical: 8 KiB) + bounce buffers (typical: 64-128 KiB per request).
// Phase 5+ may raise this when drivers need larger contiguous chunks;
// 1 MiB is also the natural bound where the buddy allocator's contiguous-
// chunk guarantee gets thin (free-list fragmentation on long-running
// systems).
#define KOBJ_DMA_MAX_SIZE  (1ull * 1024 * 1024)

// Maximum WEAVE-subtype DMA buffer size (G-2; TAPESTRY.md §18.1). A weave is a
// framebuffer-class device-passive region (triple-buffered 1080p ≈ 25 MiB), so
// it gets its own envelope above the general-DMA 1 MiB floor. 64 MiB = order 14,
// well under the buddy's max order; the contiguous-chunk pressure is accepted at
// v1.0 (tapestryd allocates its weaves early; QEMU media carry ≥ 2 GiB).
#define KOBJ_DMA_WEAVE_MAX_SIZE  (64ull * 1024 * 1024)

struct KObj_DMA {
    u64           magic;       // KOBJ_DMA_MAGIC
    u64           pa;          // physical address (page-aligned)
    size_t        size;        // requested bytes (page-aligned, > 0,
                               //   <= KOBJ_DMA_MAX_SIZE, or the weave bound)
    struct page  *pages;       // alloc_pages chunk (kept for free_pages)
    unsigned      order;       // buddy order for free_pages
    int           ref;         // refcount; starts at 1 from kobj_dma_create
    // G-2 (TAPESTRY.md §18.1 / §18.12 R2-F1): the KERNEL-MINTED device-passive
    // weave subtype bit. Set ONLY by kobj_dma_create_weave (SYS_DMA_CREATE_WEAVE),
    // create-immutable — no code path writes it on a live KObj_DMA (the same
    // structural discipline as `pa`). The cross-Proc share gate
    // (burrow_share_into + the SYS_WEFT_SHARE admission) admits a DMA Burrow
    // ONLY when this bit is set, so a device-command region (virtqueue,
    // descriptor table — allocated via plain SYS_DMA_CREATE) is structurally
    // unshareable exactly as MMIO is. The bit conveys no hardware authority of
    // its own: a weave is pinned Normal-WB RAM the device only DMA-reads
    // (pixels); what it changes is share-ADMISSIBILITY, not device reach.
    bool          weave;
};

// Bring up the DMA-handle subsystem. Atomic init guard extincts on
// re-call (mirrors kobj_mmio_init). Must be called after phys_init
// (buddy allocator must be live).
void kobj_dma_init(void);

// Allocate a fresh KObj_DMA backed by `size` bytes of contiguous
// physical memory. Backing pages are alloc_pages(order, KP_ZERO) where
// order = smallest such that 2^order pages >= size/PAGE_SIZE. Size is
// rounded up to a multiple of PAGE_SIZE.
//
// Returns NULL on:
//   - size == 0.
//   - size > KOBJ_DMA_MAX_SIZE (after page-alignment rounding).
//   - SLUB OOM for the struct.
//   - alloc_pages OOM for the page chunk.
//
// On success, the caller owns the returned reference (refcount=1);
// balance with kobj_dma_unref / kobj_dma_destroy. The PA range is
// pinned (held by struct KObj_DMA's reference) until the last unref
// drops the refcount to zero.
struct KObj_DMA *kobj_dma_create(size_t size);

// G-2: mint a WEAVE-subtype KObj_DMA (TAPESTRY.md §18.1; the SYS_DMA_CREATE_WEAVE
// body). Identical to kobj_dma_create except: the size envelope is
// KOBJ_DMA_WEAVE_MAX_SIZE (framebuffer-class), and the returned object carries
// the create-immutable `weave` bit that admits it into the cross-Proc share
// gate. Same NULL cases as kobj_dma_create (plus size > the weave bound).
struct KObj_DMA *kobj_dma_create_weave(size_t size);

// Refcount ops. Mirror kobj_mmio_ref / kobj_irq_ref.
void kobj_dma_ref(struct KObj_DMA *k);

// Decrement ref. If zero: free_pages(k->pages, k->order) + clobber
// magic + kfree(k). After the unref that drops ref to 0, `k` is INVALID.
//
// NULL-safe.
void kobj_dma_unref(struct KObj_DMA *k);

// Convenience: drop the caller's reference + ensure the PA range is
// released. Equivalent to kobj_dma_unref when the caller holds the
// only reference.
void kobj_dma_destroy(struct KObj_DMA *k);

// Diagnostic: cumulative create counter + currently-live count.
u64 kobj_dma_total_created(void);
u64 kobj_dma_live_count(void);

#endif  // THYLACINE_DMA_HANDLE_H
