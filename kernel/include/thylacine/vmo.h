// Virtual Memory Object (VMO) — kernel object representing a memory
// region, independent of any address space (P2-Fd).
//
// Per ARCHITECTURE.md §19 + specs/vmo.tla. A VMO holds:
//   - size (page-aligned, rounded up at create)
//   - backing type (VMO_TYPE_ANON at v1.0; PHYS at Phase 3; FILE post-v1.0)
//   - handle_count + mapping_count (the dual-refcount lifecycle)
//   - the backing pages (alloc_pages chunk; freed when both counts reach 0)
//
// State invariant pinned by specs/vmo.tla::NoUseAfterFree (TLC-checked):
//
//   pages alive iff (handle_count > 0 OR mapping_count > 0)
//
// At v1.0 P2-Fd:
//   - VMO_TYPE_ANON only. Eager allocation via alloc_pages(order, KP_ZERO)
//     where order = ceil_log2(page_count). Size rounded up to page_count
//     * PAGE_SIZE. Wasted memory possible for non-power-of-two page_count
//     (acceptable for tests; production-grade per-page allocation deferred).
//   - vmo_map / vmo_unmap track mapping_count without actually mapping
//     into address space. The VMA + page-fault handler integration lands
//     at Phase 3. At v1.0 the "mapping" is a refcount only.
//   - Single-CPU lifecycle. Phase 5+ adds atomic ops for handle_count +
//     mapping_count when SMP syscalls go live.
//
// Phase 3+ refinement:
//   - VMO_TYPE_PHYS for DMA buffers (CMA-allocated; pinned).
//   - vmo_map wires the VMO's backing pages into a process's vma tree +
//     page-table entries.
//   - vmo_create_physical(paddr, size) for driver pre-existing-memory.
//
// Post-v1.0:
//   - VMO_TYPE_FILE for Stratum page cache integration.

#ifndef THYLACINE_VMO_H
#define THYLACINE_VMO_H

#include <thylacine/types.h>

// VMO_MAGIC — sentinel set at vmo_create_anon; checked at vmo_ref /
// vmo_unref / vmo_map / vmo_unmap. Sits at offset 0 so SLUB's freelist
// write on kmem_cache_free clobbers it; subsequent operation on a freed
// VMO sees magic != VMO_MAGIC and extincts with a clear UAF diagnostic.
#define VMO_MAGIC 0x564D4F00BADC0DE5ULL    // 'VMO\0' || 0xBADC0DE5

enum vmo_type {
    VMO_TYPE_INVALID = 0,
    VMO_TYPE_ANON    = 1,
    // Phase 3+ : VMO_TYPE_PHYS  (DMA / MMIO buffers)
    // Post-v1.0: VMO_TYPE_FILE  (Stratum page cache)
};

struct page;

struct Vmo {
    u64            magic;          // VMO_MAGIC; clobbered by SLUB on free
    enum vmo_type  type;
    size_t         size;           // rounded-up to page_count * PAGE_SIZE
    size_t         page_count;
    int            handle_count;   // open handles to this VMO
    int            mapping_count;  // open mappings (vma's)
    struct page   *pages;          // alloc_pages chunk; NULL after free
    unsigned       order;          // for free_pages
};

_Static_assert(__builtin_offsetof(struct Vmo, magic) == 0,
               "magic must be at offset 0 — SLUB freelist write on free "
               "clobbers it (use-after-free defense)");

// Bring up the VMO subsystem. Allocates the SLUB cache. Must be called
// after slub_init; idempotent guard panics on re-call.
void vmo_init(void);

// Allocate an anonymous VMO of `size` bytes. Size is rounded up to a
// multiple of PAGE_SIZE. Backing pages are allocated eagerly via
// alloc_pages(order, KP_ZERO) where order = ceil_log2(page_count) —
// this rounds the allocation up to a power of two of pages, possibly
// wasting some.
//
// Returns a struct Vmo * with handle_count=1, mapping_count=0,
// representing the caller's exclusive initial reference. Returns NULL
// on:
//   - size == 0
//   - SLUB OOM
//   - alloc_pages OOM
//
// The caller's handle_count=1 is "consumed" — handle_alloc on this
// VMO does NOT increment; vmo_create_anon's count of 1 IS the count
// that the eventual handle_close will decrement.
struct Vmo *vmo_create_anon(size_t size);

// Increment handle_count. Maps to spec's HandleOpen action. Called by
// handle_dup (and Phase 4's handle_transfer_via_9p) for KOBJ_VMO
// handles.
//
// Extincts on NULL or corrupted magic (UAF defense).
void vmo_ref(struct Vmo *v);

// Decrement handle_count. If both counts reach 0, free pages and the
// struct. Maps to spec's HandleClose action. Called by handle_close
// for KOBJ_VMO handles.
//
// Extincts on corrupted magic or zero-ref unref. NULL is a safe no-op.
//
// IMPORTANT: after the last unref that triggers free, the v pointer
// is INVALID — the SLUB freelist clobbers magic and the memory may be
// reused. Callers must not dereference v after the unref that brings
// both counts to 0.
void vmo_unref(struct Vmo *v);

// Increment mapping_count. Maps to spec's MapVmo action. Called from
// the eventual mmap_handle path (Phase 3 with VMA wire-up).
//
// Extincts on NULL, corrupted magic, or VMO whose pages have been
// freed (this is impossible if the caller holds a handle — the handle
// keeps handle_count > 0, which keeps pages alive).
void vmo_map(struct Vmo *v);

// Decrement mapping_count. If both counts reach 0, free pages and the
// struct. Maps to spec's UnmapVmo action. Called from the eventual
// munmap path (Phase 3+).
//
// Extincts on NULL, corrupted magic, or zero-mapping unmap.
//
// IMPORTANT: same pointer-invalidation caveat as vmo_unref.
void vmo_unmap(struct Vmo *v);

// Diagnostic accessors. Safe to call on a non-NULL, non-freed VMO.
// Behavior on a freed VMO is UB (the magic check would catch a
// straight post-free deref, but a coincidental magic re-use after
// SLUB recycle is theoretically possible).
size_t vmo_get_size(const struct Vmo *v);
int    vmo_handle_count(const struct Vmo *v);
int    vmo_mapping_count(const struct Vmo *v);

// Cumulative diagnostic counters. Tests use these to verify lifecycle
// transitions:
//   - vmo_total_created increments on every successful vmo_create_anon.
//   - vmo_total_destroyed increments on every actual page-free.
//   - At any state, (created - destroyed) == live VMO count.
u64 vmo_total_created(void);
u64 vmo_total_destroyed(void);

#endif // THYLACINE_VMO_H
