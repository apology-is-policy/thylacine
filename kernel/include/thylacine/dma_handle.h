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
//      the backing PAs never change — there is no kernel code path that
//      migrates DMA pages. The structural property (no migrator exists
//      at v1.0) is a design commitment captured in specs/SPEC-TO-CODE.md;
//      the impl-side enforcement is the absence of any code path that
//      mutates blk[]/nblk on a live KObj_DMA.
//
//      WEAVE-SKEIN widened "the PA" to "the PAs": a weave larger than one
//      SKEIN_BLOCK is backed by a LIST of contiguous runs (see struct
//      dma_block). Stability is unchanged — the list is create-immutable —
//      but there is no longer a single PA to name, which is why
//      SYS_DMA_MAP refuses to return one for a skein rather than
//      approximating with blk[0].pa. Approximating would corrupt silently
//      for any caller that assumed the rest of the buffer followed it.
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

#include <thylacine/page.h>    // PAGE_SIZE — SKEIN_BLOCK_PAGES is in terms of it
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

// Maximum GPU-BO-subtype DMA buffer size (Warp-2; GPU-DESIGN.md §6.1). A
// render target / texture / vertex buffer the GPU service allocates per
// client request. Same envelope as the weave (a 4K RGBA target with mips is
// ~44 MiB); larger scenes split across BOs. Runtime-allocated (unlike weaves,
// which the compositor mints early), so long-uptime buddy fragmentation is a
// real caveat for the big end of the envelope — scatter-gather backing
// (virtio ATTACH_BACKING takes an entry list; the contiguity is OUR object's
// constraint, not the device's) is the recorded follow-on if it bites.
#define KOBJ_DMA_GPU_BO_MAX_SIZE (64ull * 1024 * 1024)

// WEAVE-SKEIN (docs/WEAVE-SKEIN-DESIGN.md §3.3): the skein's block
// granularity. A weave larger than one block is backed by a LIST of blocks of
// this size rather than one span, so the largest contiguous run the buddy must
// serve falls from the whole weave (order 14 for the 48.8 MiB 2560x1664 case,
// which fails with 1889 MiB free — fragmentation, measured) to 2 MiB (order 9,
// abundant).
//
// 2 MiB is the ratified choice: 25 blocks and 0.5% waste for that weave, 32
// blocks at the full 64 MiB envelope, against the 78 mem entries the existing
// virtio-gpu REQ region holds — so no transport change is required, with real
// headroom left.
#define SKEIN_BLOCK        (2ull * 1024 * 1024)
#define SKEIN_BLOCK_PAGES  (SKEIN_BLOCK / PAGE_SIZE)

// The most blocks any KObj_DMA can hold. Bounded by the largest envelope
// (the weave's) at the block granularity, so the block array is inline and
// fixed rather than separately allocated — which removes an allocation, a
// free, and the whole double-free / dangling-array finding class from an
// I-40/I-45 surface. 32 * 24 B = 768 B per object; a handful are ever live.
#define KOBJ_DMA_MAX_BLOCKS \
    ((unsigned)(KOBJ_DMA_WEAVE_MAX_SIZE / SKEIN_BLOCK))

// The block array is fixed, so the bound that keeps a weave inside it is a
// COMPILE-TIME obligation, not a runtime hope: the envelope must not need more
// blocks than the array holds. Checked here rather than trusted, because the
// runtime guard in dma_create_body would be the last thing between a raised
// envelope and a kernel-memory write past blk[].
_Static_assert(KOBJ_DMA_WEAVE_MAX_SIZE <= (u64)KOBJ_DMA_MAX_BLOCKS * SKEIN_BLOCK,
               "a full weave must fit in KObj_DMA.blk[]");
// The buddy allocates in power-of-two page runs, so a block size that is not
// one would make order_for_pages over-allocate every block silently.
_Static_assert(SKEIN_BLOCK % PAGE_SIZE == 0 &&
               (SKEIN_BLOCK & (SKEIN_BLOCK - 1)) == 0,
               "SKEIN_BLOCK must be a power-of-two page multiple");
// Plain DMA never scatters, and its envelope is what makes that safe: one
// block always covers it. If KOBJ_DMA_MAX_SIZE ever rose past SKEIN_BLOCK the
// single-block stride would still be correct (skein_stride derives it from
// size), but the claim "plain DMA is small so contiguity is cheap" would not.
_Static_assert(KOBJ_DMA_MAX_SIZE <= SKEIN_BLOCK,
               "plain DMA is single-block; its envelope must stay within one");

// One physically-contiguous run of a KObj_DMA's backing. A plain DMA object
// has exactly one (nblk == 1); a skein has N, VA-contiguous when mapped but
// physically scattered.
struct dma_block {
    u64           pa;          // block base PA (page-aligned)
    struct page  *pages;       // the alloc_pages chunk, for free_pages
    unsigned      order;       // buddy order of THIS block, for free_pages
};

struct KObj_DMA {
    u64           magic;       // KOBJ_DMA_MAGIC
    size_t        size;        // requested bytes (page-aligned, > 0,
                               //   <= KOBJ_DMA_MAX_SIZE, or the weave bound)
    // The skein. blk[0..nblk) are the backing runs, in ascending buffer order:
    // buffer page i lives in block i / SKEIN_BLOCK_PAGES at page offset
    // i % SKEIN_BLOCK_PAGES. That stride is UNIFORM by construction — every
    // block but the last spans exactly SKEIN_BLOCK_PAGES — which is what keeps
    // page resolution a division rather than a search. The tail block is sized
    // to its own order (see kobj_dma_pa_at). Create-immutable, like `size`:
    // nothing rewrites a live object's backing.
    struct dma_block blk[KOBJ_DMA_MAX_BLOCKS];
    u32           nblk;        // 1 for plain DMA / a small weave; N for a skein
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
    // Warp-2 (GPU-DESIGN.md §6.1): the KERNEL-MINTED GPU-BO subtype bit — the
    // second share-admissible kind, with a DIFFERENT safety argument than the
    // weave's. A weave is device-READ only (pixels out); a GPU BO is
    // device-WRITTEN (a render target, a readback destination), which breaks
    // the weave's "device-passive" argument. The GPU-BO argument is §2.1's:
    // what the GPU may write is bounded by the GPU's own address translation,
    // which only the trusted device owner programs — the client's cacheable
    // RW mapping still conveys zero hardware authority. Set ONLY by
    // kobj_dma_create_gpu_bo (SYS_DMA_CREATE_GPU_BO); create-immutable;
    // mutually exclusive with `weave` by construction (each mint sets one).
    bool          gpu_bo;
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

// Warp-2 (GPU-DESIGN.md §6.1): mint a GPU-BO-subtype KObj_DMA (the
// SYS_DMA_CREATE_GPU_BO body). Identical to kobj_dma_create except: the size
// envelope is KOBJ_DMA_GPU_BO_MAX_SIZE, and the returned object carries the
// create-immutable `gpu_bo` bit — the second share-admissible kind (see the
// struct field for its distinct device-WRITTEN safety argument). Same NULL
// cases as kobj_dma_create (plus size > the GPU-BO bound).
struct KObj_DMA *kobj_dma_create_gpu_bo(size_t size);

// WEAVE-SKEIN: resolve a byte offset within the object's buffer to the
// backing PA, walking the skein. THE one place buffer-offset -> PA is decided,
// so the demand-fault arm and the segment copy-out cannot drift apart.
//
// `byte_off` need not be page-aligned; the returned PA carries the same
// in-page offset. Returns 0 (never a valid backing PA — page 0 is not
// buddy-allocated) when k is NULL/corrupted, byte_off is out of range, or the
// resolved block is absent, so every caller's guard is one `== 0` test.
//
// The tail block may be SHORTER than SKEIN_BLOCK (it is sized to its own
// buddy order rather than padded), so this bounds the offset against BOTH the
// object's size and the resolved block's own length — an offset inside the
// object's last block but past that block's allocation is out of range, which
// cannot happen for a well-formed object and is refused rather than trusted.
u64 kobj_dma_pa_at(const struct KObj_DMA *k, u64 byte_off);

// WEAVE-SKEIN: length in bytes of skein block `i` (its own buddy order's page
// count, clamped to the object's remaining size). Returns 0 for a NULL /
// corrupted object or an out-of-range index. The sum over 0..nblk is the
// object's size, which the segment copy-out relies on.
u64 kobj_dma_block_len(const struct KObj_DMA *k, u32 i);

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
