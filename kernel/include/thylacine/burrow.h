// Virtual Memory Object (BURROW) — kernel object representing a memory
// region, independent of any address space (P2-Fd / P3-Db).
//
// Per ARCHITECTURE.md §19 + specs/burrow.tla. A BURROW holds:
//   - size (page-aligned, rounded up at create)
//   - backing type (BURROW_TYPE_ANON at v1.0; PHYS at Phase 3; FILE post-v1.0)
//   - handle_count + mapping_count (the dual-refcount lifecycle)
//   - the backing pages (alloc_pages chunk; freed when both counts reach 0)
//
// State invariant pinned by specs/burrow.tla::NoUseAfterFree (TLC-checked):
//
//   pages alive iff (handle_count > 0 OR mapping_count > 0)
//
// API surface (P3-Db):
//   - burrow_create_anon / burrow_ref / burrow_unref — handle-side lifecycle.
//   - burrow_map(Proc, Burrow, vaddr, length, prot) — install a VMA in a Proc's
//     address space. Calls vma_alloc + vma_insert; mapping_count++ on
//     success. Returns 0 on success, negative errno on failure.
//   - burrow_unmap(Proc, vaddr, length) — remove the matching VMA from a
//     Proc; mapping_count--. At v1.0 the (vaddr, length) must match the
//     VMA exactly (no partial unmap; that's post-v1.0).
//   - burrow_acquire_mapping / burrow_release_mapping — the bare refcount-only
//     ops (renamed from old burrow_map/burrow_unmap). Internal to the vma
//     layer; not for general use. Public so test_burrow.c can exercise the
//     refcount lifecycle in isolation per specs/burrow.tla.
//
// At v1.0 P3-Db:
//   - BURROW_TYPE_ANON only. Eager allocation via alloc_pages(order, KP_ZERO)
//     where order = ceil_log2(page_count). Size rounded up to page_count
//     * PAGE_SIZE. Wasted memory possible for non-power-of-two page_count
//     (acceptable for tests; production-grade per-page allocation deferred).
//   - burrow_map creates a VMA via vma_alloc + vma_insert. PTE installation
//     is deferred to demand-paging via the user-mode fault path (P3-Dc);
//     burrow_map only installs the VMA.
//   - Single-CPU lifecycle. Phase 5+ adds atomic ops for handle_count +
//     mapping_count when SMP syscalls go live.
//
// Phase 3+ refinement (P3-Dc):
//   - arch_fault_handle's user-mode dispatch path → vma_lookup → page
//     allocate → PTE install in the per-Proc TTBR0 tree.
//
// Phase 3+ extensions:
//   - BURROW_TYPE_PHYS for DMA buffers (CMA-allocated; pinned).
//   - burrow_create_physical(paddr, size) for driver pre-existing-memory.
//
// Post-v1.0:
//   - BURROW_TYPE_FILE for Stratum page cache integration.
//   - Partial unmap (burrow_unmap with sub-VMA range; splits the VMA).

#ifndef THYLACINE_VMO_H
#define THYLACINE_VMO_H

#include <thylacine/types.h>

struct Proc;

// VMO_MAGIC — sentinel set at burrow_create_anon; checked at burrow_ref /
// burrow_unref / burrow_acquire_mapping / burrow_release_mapping. Sits at offset
// 0 so SLUB's freelist write on kmem_cache_free clobbers it; subsequent
// operation on a freed BURROW sees magic != VMO_MAGIC and extincts with a
// clear UAF diagnostic.
#define VMO_MAGIC 0x564D4F00BADC0DE5ULL    // 'BURROW\0' || 0xBADC0DE5

enum burrow_type {
    BURROW_TYPE_INVALID = 0,
    BURROW_TYPE_ANON    = 1,
    // P4-Ic1: BURROW_TYPE_MMIO — backing is a fixed physical-memory range
    // owned by a KObj_MMIO (the spec-pinned PA-exclusivity claim ticket
    // from P4-Ib). The Burrow holds a reference to the underlying
    // KObj_MMIO; `pages` is NULL (no alloc_pages backing); `pa` carries
    // the device PA. burrow_unref of a MMIO Burrow skips free_pages and
    // calls kobj_mmio_unref(kobj_mmio) to release the held reference.
    BURROW_TYPE_MMIO    = 2,
    // P4-Ic5b1b: BURROW_TYPE_DMA — backing is a kernel-allocated contiguous
    // page chunk owned by a KObj_DMA (PA-stable claim ticket from
    // P4-Ic5b1b). The Burrow holds a reference to the underlying KObj_DMA;
    // `pages` is NULL on the Burrow (the page chunk lives on the KObj_DMA
    // itself, not the Burrow); `pa` carries the buddy-chosen PA. Distinct
    // from BURROW_TYPE_ANON because:
    //   - The backing is owned by a separately-refcounted KObj_DMA (so
    //     handle_close on the user's KObj_DMA handle and burrow_unmap on
    //     the user's VMA can race; either path drops a ref on KObj_DMA
    //     and the last one frees the pages).
    //   - The PTE attrs at userland_demand_page time are Normal cacheable
    //     (matches BURROW_TYPE_ANON; distinct from BURROW_TYPE_MMIO which
    //     uses Device-nGnRnE).
    // burrow_unref of a DMA Burrow skips free_pages and calls
    // kobj_dma_unref(kobj_dma) to release the held reference.
    BURROW_TYPE_DMA     = 3,
    // Post-v1.0: BURROW_TYPE_FILE  (Stratum page cache)
};

struct page;
struct KObj_MMIO;
struct KObj_DMA;

struct Burrow {
    u64            magic;          // VMO_MAGIC; clobbered by SLUB on free
    enum burrow_type  type;
    size_t         size;           // rounded-up to page_count * PAGE_SIZE
    size_t         page_count;
    int            handle_count;   // open handles to this BURROW
    int            mapping_count;  // open mappings (vma's)
    struct page   *pages;          // alloc_pages chunk; NULL after free; NULL for MMIO
    unsigned       order;          // for free_pages; unused for MMIO
    // P4-Ic1 / P4-Ic5b1b: hw-backed-Burrow fields. For BURROW_TYPE_ANON
    // these are zero. For BURROW_TYPE_MMIO: kobj_mmio is the underlying
    // KObj_MMIO whose PA claim this Burrow holds; pa is the device PA
    // (page-aligned, matches kobj_mmio->pa). For BURROW_TYPE_DMA:
    // kobj_dma is the underlying KObj_DMA whose pinned page chunk this
    // Burrow wraps; pa is the buddy-chosen PA (matches kobj_dma->pa).
    // Exactly one of kobj_mmio / kobj_dma is non-NULL for hw types; both
    // are NULL for BURROW_TYPE_ANON. The non-NULL hw ref is released at
    // burrow_free_internal via the type-dispatched switch.
    struct KObj_MMIO *kobj_mmio;   // NULL except for BURROW_TYPE_MMIO
    struct KObj_DMA  *kobj_dma;    // NULL except for BURROW_TYPE_DMA
    u64               pa;           // 0 except for hw-backed types
};

_Static_assert(__builtin_offsetof(struct Burrow, magic) == 0,
               "magic must be at offset 0 — SLUB freelist write on free "
               "clobbers it (use-after-free defense)");

// Bring up the BURROW subsystem. Allocates the SLUB cache. Must be called
// after slub_init; idempotent guard panics on re-call.
void burrow_init(void);

// Allocate an anonymous BURROW of `size` bytes. Size is rounded up to a
// multiple of PAGE_SIZE. Backing pages are allocated eagerly via
// alloc_pages(order, KP_ZERO) where order = ceil_log2(page_count) —
// this rounds the allocation up to a power of two of pages, possibly
// wasting some.
//
// Returns a struct Burrow * with handle_count=1, mapping_count=0,
// representing the caller's exclusive initial reference. Returns NULL
// on:
//   - size == 0
//   - SLUB OOM
//   - alloc_pages OOM
//
// The caller's handle_count=1 is "consumed" — handle_alloc on this
// BURROW does NOT increment; burrow_create_anon's count of 1 IS the count
// that the eventual handle_close will decrement.
struct Burrow *burrow_create_anon(size_t size);

// P4-Ic1: burrow_create_mmio — wrap a KObj_MMIO in a Burrow so the
// VMA + page-fault dispatch path can install device-memory PTEs
// uniformly with the anon-Burrow flow.
//
// The Burrow takes a reference on the KObj_MMIO via kobj_mmio_ref so
// the underlying PA claim survives the caller's eventual kobj_mmio_unref
// (typically the user's handle_close on their KOBJ_MMIO handle). The
// Burrow's reference is released when burrow_free_internal fires
// (handle_count + mapping_count both reach 0).
//
// Returns the Burrow with handle_count=1 (caller's construction
// reference, consumed by either burrow_map → burrow_unref transfer or
// explicit burrow_unref).
//
// Returns NULL on:
//   - NULL kobj_mmio, corrupted magic.
//   - kobj_mmio->size == 0 (defensive; kobj_mmio_create rejects this).
//   - SLUB OOM.
//
// At v1.0 P4-Ic1 the Burrow does NOT install any PTEs — that's the
// VMA layer's job at burrow_map. Demand-page integration in
// arch/arm64/fault.c (handling the MMIO PA + device-memory PTE attrs)
// lands at P4-Ic2.
struct Burrow *burrow_create_mmio(struct KObj_MMIO *kobj_mmio);

// P4-Ic5b1b: burrow_create_dma — wrap a KObj_DMA in a Burrow so the
// VMA + page-fault dispatch path can install user-VA mappings backed
// by the kernel-allocated pinned page chunk.
//
// The Burrow takes a reference on the KObj_DMA via kobj_dma_ref so the
// underlying page chunk survives the caller's eventual handle_close on
// their KOBJ_DMA handle. The Burrow's reference is released when
// burrow_free_internal fires (handle_count + mapping_count both reach 0).
//
// Returns the Burrow with handle_count=1 (caller's construction
// reference, consumed by either burrow_map → burrow_unref transfer or
// explicit burrow_unref).
//
// Returns NULL on:
//   - NULL kobj_dma, corrupted magic.
//   - kobj_dma->size == 0 (defensive; kobj_dma_create rejects).
//   - SLUB OOM.
//
// Distinct from burrow_create_mmio at the demand-page layer: DMA Burrows
// install Normal cacheable PTEs (CPU + device coherent on QEMU virt's
// VirtIO transports), MMIO Burrows install Device-nGnRnE PTEs. The
// dispatch happens in arch/arm64/fault.c::userland_demand_page.
struct Burrow *burrow_create_dma(struct KObj_DMA *kobj_dma);

// Increment handle_count. Maps to spec's HandleOpen action. Called by
// handle_dup (and Phase 4's handle_transfer_via_9p) for KOBJ_BURROW
// handles.
//
// Extincts on NULL or corrupted magic (UAF defense).
void burrow_ref(struct Burrow *v);

// Decrement handle_count. If both counts reach 0, free pages and the
// struct. Maps to spec's HandleClose action. Called by handle_close
// for KOBJ_BURROW handles.
//
// Extincts on corrupted magic or zero-ref unref. NULL is a safe no-op.
//
// IMPORTANT: after the last unref that triggers free, the v pointer
// is INVALID — the SLUB freelist clobbers magic and the memory may be
// reused. Callers must not dereference v after the unref that brings
// both counts to 0.
void burrow_unref(struct Burrow *v);

// Increment mapping_count (refcount-only — does NOT install a VMA or
// PTEs). Maps to spec's MapVmo action. Internal helper used by the VMA
// layer (vma_alloc) and by tests that exercise the refcount lifecycle in
// isolation. Public callers should use burrow_map(Proc, ...) below.
//
// Extincts on NULL, corrupted magic, or BURROW whose pages have been
// freed (this is impossible if the caller holds a handle — the handle
// keeps handle_count > 0, which keeps pages alive).
//
// Was named `burrow_map` pre-P3-Db; renamed when the high-level
// burrow_map(Proc*, ...) entry point arrived.
void burrow_acquire_mapping(struct Burrow *v);

// Decrement mapping_count (refcount-only — does NOT remove any VMA).
// If both counts reach 0, free pages and the struct. Maps to spec's
// UnmapVmo action. Internal helper used by the VMA layer (vma_free).
// Public callers should use burrow_unmap(Proc, ...) below.
//
// Extincts on NULL, corrupted magic, or zero-mapping unmap.
//
// IMPORTANT: same pointer-invalidation caveat as burrow_unref.
//
// Was named `burrow_unmap` pre-P3-Db.
void burrow_release_mapping(struct Burrow *v);

// =============================================================================
// P3-Db: high-level map / unmap into a Proc's address space.
// =============================================================================

// burrow_map: install a VMA backed by `v` into Proc `p`'s address space at
// user-VA range [vaddr, vaddr + length). Allocates a Vma via vma_alloc
// (which takes burrow_acquire_mapping; mapping_count++) and inserts it via
// vma_insert (which rejects overlap with existing VMAs).
//
// Does NOT install PTEs — the per-Proc TTBR0 tree starts empty (every L0
// entry invalid) and pages are populated on demand by the user-mode page-
// fault handler (P3-Dc).
//
// Returns:
//   0  on success.
//   -1 on any failure: invalid argument (zero-length, unaligned vaddr/
//      length, NULL inputs, W+X prot), VMA SLUB OOM, or VMA overlap with
//      existing entry. On failure, no VMA is installed and mapping_count
//      is unchanged.
//
// Constraints (validated by vma_alloc):
//   - p, v non-NULL.
//   - length > 0.
//   - vaddr and (vaddr + length) page-aligned.
//   - prot ∈ {0, R, RW, RX} (W+X rejected per ARCH §28 I-12).
//
// At v1.0 P3-Db, burrow_offset is implicitly 0 (the VMA covers the head of
// the BURROW). Phase 5+ extends with an explicit offset for shared VMOs.
int burrow_map(struct Proc *p, struct Burrow *v, u64 vaddr, size_t length, u32 prot);

// burrow_unmap: remove the VMA at user-VA range [vaddr, vaddr + length)
// from Proc `p`. Calls vma_remove + vma_free (which calls
// burrow_release_mapping; mapping_count--).
//
// At v1.0 P3-Db, the (vaddr, length) range must match an existing VMA
// EXACTLY — partial unmap (splitting a VMA into two halves with a hole)
// is post-v1.0.
//
// Returns:
//   0  on success.
//   -1 on no matching VMA (no VMA starts at `vaddr` with the requested
//      length).
//
// Does NOT touch PTEs (no PTEs were installed for the unmapped range
// pre-P3-Dc, since demand paging hadn't fired yet; once P3-Dc lands,
// burrow_unmap will additionally tear down installed PTEs in the range).
int burrow_unmap(struct Proc *p, u64 vaddr, size_t length);

// Diagnostic accessors. Safe to call on a non-NULL, non-freed BURROW.
// Behavior on a freed BURROW is UB (the magic check would catch a
// straight post-free deref, but a coincidental magic re-use after
// SLUB recycle is theoretically possible).
size_t burrow_get_size(const struct Burrow *v);
int    burrow_handle_count(const struct Burrow *v);
int    burrow_mapping_count(const struct Burrow *v);

// Cumulative diagnostic counters. Tests use these to verify lifecycle
// transitions:
//   - burrow_total_created increments on every successful burrow_create_anon.
//   - burrow_total_destroyed increments on every actual page-free.
//   - At any state, (created - destroyed) == live BURROW count.
u64 burrow_total_created(void);
u64 burrow_total_destroyed(void);

#endif // THYLACINE_VMO_H
