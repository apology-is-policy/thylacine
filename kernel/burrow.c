// Virtual Memory Object (BURROW) impl — dual-refcount lifecycle (P2-Fd /
// P3-Db).
//
// Per ARCHITECTURE.md §19 + specs/burrow.tla. Implements the spec's
// VmoCreate / HandleOpen / HandleClose / MapVmo / UnmapVmo actions on
// SLUB-allocated struct Burrow with eager-allocated backing pages.
//
// State invariant (TLC-checked in specs/burrow.tla::NoUseAfterFree):
//
//   pages alive iff (handle_count > 0 OR mapping_count > 0)
//
// Enforced at runtime by the dual-check in burrow_unref AND
// burrow_release_mapping:
//
//   if (handle_count == 0 && mapping_count == 0) burrow_free_internal(v);
//
// Either path can trigger the free; whichever brings the LAST count to
// zero releases the pages. The spec's three buggy variants
// (BUGGY_FREE_ON_HANDLE_CLOSE / BUGGY_FREE_ON_UNMAP / BUGGY_NEVER_FREE)
// each correspond to a way this dual-check could be wrong: free without
// checking the other count (premature) or fail to check at all (leak).
//
// API split (P3-Db):
//   - burrow_acquire_mapping / burrow_release_mapping: bare refcount ops
//     (formerly burrow_map / burrow_unmap pre-P3-Db). Internal to the VMA
//     layer; tests use them to exercise the refcount lifecycle.
//   - burrow_map(Proc, Burrow, vaddr, length, prot): public mapping entry —
//     calls vma_alloc + vma_insert. Returns 0/-1.
//   - burrow_unmap(Proc, vaddr, length): public unmap entry — calls
//     vma_remove + vma_free. Returns 0/-1.
//
// At v1.0 P3-Db:
//   - Anonymous VMOs only. alloc_pages(order, KP_ZERO) for the chunk.
//   - burrow_map installs a VMA only — no PTE installation. Demand paging
//     via the user-mode fault path lands at P3-Dc.
//   - Single-CPU lifecycle; Phase 5+ adds atomic ops.

#include <thylacine/extinction.h>
#include <thylacine/mmio_handle.h>   // P4-Ic1: kobj_mmio_ref/unref for MMIO Burrows
#include <thylacine/dma_handle.h>    // P4-Ic5b1b: kobj_dma_ref/unref for DMA Burrows
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>
#include <thylacine/burrow.h>

#include "../mm/phys.h"
#include "../mm/slub.h"

static struct kmem_cache *g_vmo_cache;
static u64                g_vmo_created;
static u64                g_vmo_destroyed;

// =============================================================================
// Lifecycle.
// =============================================================================

void burrow_init(void) {
    if (g_vmo_cache) extinction("burrow_init called twice");

    // R5-F F50/F51 close: NO PANIC_ON_FAIL. BURROW allocation is reachable
    // from userspace via burrow_create syscall (Phase 5+) — userspace OOM
    // shouldn't extinct the kernel. The burrow_create_anon documented
    // contract is "returns NULL on OOM"; the cache flag must match.
    g_vmo_cache = kmem_cache_create("burrow",
                                    sizeof(struct Burrow),
                                    8,
                                    0);
    if (!g_vmo_cache) {
        extinction("kmem_cache_create(burrow) returned NULL");
    }
}

// Compute the buddy order for at least `page_count` pages. Returns the
// smallest order such that 2^order >= page_count. Result is the
// argument to alloc_pages.
static unsigned order_for_pages(size_t page_count) {
    unsigned order = 0;
    size_t n = 1;
    while (n < page_count) {
        n <<= 1;
        order++;
    }
    return order;
}

struct Burrow *burrow_create_anon(size_t size) {
    if (!g_vmo_cache) extinction("burrow_create_anon before burrow_init");
    if (size == 0)    return NULL;
    // Overflow guard: `size + PAGE_SIZE - 1` wraps to a small value when
    // size is within (PAGE_SIZE - 1) of SIZE_MAX, producing a tiny
    // page_count for what was supposed to be an enormous request. Reject
    // such requests explicitly (the caller meant something pathological;
    // we won't silently truncate to a 1-page BURROW claiming size = 0).
    if (size > SIZE_MAX - (PAGE_SIZE - 1)) return NULL;

    struct Burrow *v = kmem_cache_alloc(g_vmo_cache, KP_ZERO);
    if (!v) return NULL;

    size_t page_count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    unsigned order = order_for_pages(page_count);

    struct page *pages = alloc_pages(order, KP_ZERO);
    if (!pages) {
        kmem_cache_free(g_vmo_cache, v);
        return NULL;
    }

    v->magic         = VMO_MAGIC;
    v->type          = BURROW_TYPE_ANON;
    v->size          = page_count * PAGE_SIZE;
    v->page_count    = page_count;
    v->handle_count  = 1;            // caller's initial reference
    v->mapping_count = 0;
    v->pages         = pages;
    v->order         = order;
    g_vmo_created++;
    return v;
}

// P4-Ic1: Wrap a KObj_MMIO in a Burrow. Holds a reference on the
// underlying KObj_MMIO so the PA claim survives the caller's eventual
// handle_close. handle_count=1 is the "construction reference" pattern
// (mirrors burrow_create_anon).
struct Burrow *burrow_create_mmio(struct KObj_MMIO *kobj_mmio) {
    if (!g_vmo_cache) extinction("burrow_create_mmio before burrow_init");
    if (!kobj_mmio) return NULL;
    // Magic check defends against the caller passing a freed pointer.
    // kobj_mmio_ref also defends; we duplicate here so the OOM path
    // below doesn't bump a corrupted ref.
    if (kobj_mmio->magic != KOBJ_MMIO_MAGIC)
        extinction("burrow_create_mmio: kobj_mmio has bad magic (UAF?)");
    if (kobj_mmio->size == 0) return NULL;     // defensive; kobj_mmio_create rejects

    struct Burrow *v = kmem_cache_alloc(g_vmo_cache, KP_ZERO);
    if (!v) return NULL;

    // Hold a ref on the kobj_mmio for the Burrow's lifetime. The ref
    // is released in burrow_free_internal when both counts reach 0.
    kobj_mmio_ref(kobj_mmio);

    v->magic         = VMO_MAGIC;
    v->type          = BURROW_TYPE_MMIO;
    v->size          = kobj_mmio->size;
    v->page_count    = kobj_mmio->size / PAGE_SIZE;
    v->handle_count  = 1;            // construction reference
    v->mapping_count = 0;
    v->pages         = NULL;         // MMIO: no struct page backing
    v->order         = 0;
    v->kobj_mmio     = kobj_mmio;
    v->pa            = kobj_mmio->pa;
    g_vmo_created++;
    return v;
}

// P4-Ic5b1b: Wrap a KObj_DMA in a Burrow. Holds a reference on the
// underlying KObj_DMA so the pinned page chunk survives the caller's
// eventual handle_close on their KObj_DMA handle. Mirrors
// burrow_create_mmio's construction-reference pattern.
struct Burrow *burrow_create_dma(struct KObj_DMA *kobj_dma) {
    if (!g_vmo_cache) extinction("burrow_create_dma before burrow_init");
    if (!kobj_dma) return NULL;
    if (kobj_dma->magic != KOBJ_DMA_MAGIC)
        extinction("burrow_create_dma: kobj_dma has bad magic (UAF?)");
    if (kobj_dma->size == 0) return NULL;     // defensive; kobj_dma_create rejects

    struct Burrow *v = kmem_cache_alloc(g_vmo_cache, KP_ZERO);
    if (!v) return NULL;

    // Hold a ref on the kobj_dma for the Burrow's lifetime. Released in
    // burrow_free_internal when both counts reach 0.
    kobj_dma_ref(kobj_dma);

    v->magic         = VMO_MAGIC;
    v->type          = BURROW_TYPE_DMA;
    v->size          = kobj_dma->size;
    v->page_count    = kobj_dma->size / PAGE_SIZE;
    v->handle_count  = 1;            // construction reference
    v->mapping_count = 0;
    v->pages         = NULL;         // DMA: page chunk lives on the KObj_DMA
    v->order         = 0;
    v->kobj_dma      = kobj_dma;
    v->pa            = kobj_dma->pa;
    g_vmo_created++;
    return v;
}

// Internal: release pages + struct when both counts have reached 0.
// The caller has already verified the precondition.
//
// P4-Ic1: type-dispatches between ANON (free_pages on the alloc_pages
// chunk) and MMIO (release the held KObj_MMIO ref; pages was NULL by
// construction). P4-Ic5b1b adds DMA (release the held KObj_DMA ref;
// pages was NULL on the Burrow — the page chunk lives on the KObj_DMA).
// The double-free guard differs per type:
//   - ANON: pages==NULL indicates already-freed → extinct.
//   - MMIO: kobj_mmio==NULL indicates already-freed → extinct.
//   - DMA:  kobj_dma==NULL indicates already-freed → extinct.
static void burrow_free_internal(struct Burrow *v) {
    if (v->magic != VMO_MAGIC)
        extinction("burrow_free_internal of corrupted BURROW");
    if (v->handle_count != 0)
        extinction("burrow_free_internal with handle_count > 0 (premature free; "
                   "specs/burrow.tla NoUseAfterFree violation)");
    if (v->mapping_count != 0)
        extinction("burrow_free_internal with mapping_count > 0 (premature free; "
                   "specs/burrow.tla NoUseAfterFree violation)");

    switch (v->type) {
    case BURROW_TYPE_ANON:
        if (!v->pages)
            extinction("burrow_free_internal(ANON) with pages already NULL (double-free)");
        free_pages(v->pages, v->order);
        v->pages = NULL;
        break;
    case BURROW_TYPE_MMIO:
        if (!v->kobj_mmio)
            extinction("burrow_free_internal(MMIO) with kobj_mmio already NULL (double-free)");
        // Drop the Burrow's reference to the KObj_MMIO. If the userspace
        // handle has already been closed (its own ref dropped), this
        // unref is the last one and triggers kobj_mmio_free_internal +
        // claim release. Otherwise the handle's ref keeps the kobj
        // alive and only the Burrow goes away.
        kobj_mmio_unref(v->kobj_mmio);
        v->kobj_mmio = NULL;
        break;
    case BURROW_TYPE_DMA:
        if (!v->kobj_dma)
            extinction("burrow_free_internal(DMA) with kobj_dma already NULL (double-free)");
        // Drop the Burrow's reference to the KObj_DMA. Mirror of the
        // MMIO path: the user-handle's ref and the Burrow's ref are
        // independent; whichever drops last triggers
        // kobj_dma_free_internal + free_pages on the underlying chunk.
        kobj_dma_unref(v->kobj_dma);
        v->kobj_dma = NULL;
        break;
    case BURROW_TYPE_INVALID:
    default:
        extinction("burrow_free_internal: invalid burrow type");
    }

    kmem_cache_free(g_vmo_cache, v);
    g_vmo_destroyed++;
}

// =============================================================================
// Refcount ops.
// =============================================================================

void burrow_ref(struct Burrow *v) {
    if (!v)                       extinction("burrow_ref(NULL)");
    if (v->magic != VMO_MAGIC)    extinction("burrow_ref of corrupted BURROW");
    // Defensive: a BURROW with handle_count=0 AND mapping_count=0 should
    // already be freed; ref-ing it would resurrect an already-freed
    // object's identity. UAF-class bug.
    if (v->handle_count == 0 && v->mapping_count == 0)
        extinction("burrow_ref on BURROW with both counts=0 (already freed?)");

    v->handle_count++;
}

void burrow_unref(struct Burrow *v) {
    if (!v) return;                            // NULL-safe
    if (v->magic != VMO_MAGIC)
        extinction("burrow_unref of corrupted BURROW (use-after-free?)");
    if (v->handle_count <= 0)
        extinction("burrow_unref of zero-ref BURROW");

    v->handle_count--;
    // Dual-check: free only when BOTH counts reach 0. Maps to the
    // spec's NoUseAfterFree iff invariant — premature free violates
    // (counts > 0 ∧ pages dead); delayed free violates (counts = 0 ∧
    // pages alive). The dual-check is the runtime enforcement.
    if (v->handle_count == 0 && v->mapping_count == 0) {
        burrow_free_internal(v);
    }
}

void burrow_acquire_mapping(struct Burrow *v) {
    if (!v)                       extinction("burrow_acquire_mapping(NULL)");
    if (v->magic != VMO_MAGIC)    extinction("burrow_acquire_mapping of corrupted BURROW");
    // P4-Ic1: per-type liveness check. ANON: pages must still be alive.
    // MMIO: kobj_mmio must still be held (the equivalent "backing
    // resource" for MMIO Burrows).
    switch (v->type) {
    case BURROW_TYPE_ANON:
        if (!v->pages)
            extinction("burrow_acquire_mapping of ANON BURROW with NULL pages (UAF)");
        break;
    case BURROW_TYPE_MMIO:
        if (!v->kobj_mmio)
            extinction("burrow_acquire_mapping of MMIO BURROW with NULL kobj_mmio (UAF)");
        break;
    case BURROW_TYPE_DMA:
        if (!v->kobj_dma)
            extinction("burrow_acquire_mapping of DMA BURROW with NULL kobj_dma (UAF)");
        break;
    case BURROW_TYPE_INVALID:
    default:
        extinction("burrow_acquire_mapping: invalid burrow type");
    }

    v->mapping_count++;
}

void burrow_release_mapping(struct Burrow *v) {
    if (!v)                       extinction("burrow_release_mapping(NULL)");
    if (v->magic != VMO_MAGIC)
        extinction("burrow_release_mapping of corrupted BURROW (use-after-free?)");
    if (v->mapping_count <= 0)
        extinction("burrow_release_mapping of zero-mapping BURROW");

    v->mapping_count--;
    // Same dual-check as burrow_unref — symmetric.
    if (v->handle_count == 0 && v->mapping_count == 0) {
        burrow_free_internal(v);
    }
}

// =============================================================================
// P3-Db: high-level map / unmap into a Proc's address space.
// =============================================================================

int burrow_map(struct Proc *p, struct Burrow *v, u64 vaddr, size_t length, u32 prot) {
    if (!p || !v) return -1;
    if (length == 0) return -1;
    if (vaddr & (PAGE_SIZE - 1)) return -1;
    if (length & (PAGE_SIZE - 1)) return -1;
    // Overflow guard: vaddr + length must not wrap. The page-aligned
    // checks above bound `vaddr` and `length` to non-pathological values
    // but a sufficiently large `vaddr` near the top of the user-VA space
    // could still wrap.
    if (vaddr + length < vaddr) return -1;

    // Delegate constraint validation (W+X reject, prot value) to vma_alloc.
    // It returns NULL on any constraint violation OR on SLUB OOM, and
    // takes burrow_acquire_mapping internally on success.
    struct Vma *vma = vma_alloc(vaddr, vaddr + length, prot, v, /*offset=*/0);
    if (!vma) return -1;

    // vma_insert returns -1 on overlap. On overlap, the Vma is still
    // owned by us — vma_free releases the BURROW mapping ref symmetrically.
    if (vma_insert(p, vma) != 0) {
        vma_free(vma);
        return -1;
    }
    return 0;
}

int burrow_unmap(struct Proc *p, u64 vaddr, size_t length) {
    if (!p) return -1;
    if (length == 0) return -1;
    if (vaddr & (PAGE_SIZE - 1)) return -1;
    if (length & (PAGE_SIZE - 1)) return -1;

    // Find the VMA whose range matches [vaddr, vaddr + length) exactly.
    // vma_lookup with vaddr would find any VMA covering vaddr, but at
    // v1.0 we require exact-match (no partial unmap), so we walk + check.
    u64 want_end = vaddr + length;
    if (want_end < vaddr) return -1;     // overflow

    struct Vma *vma = vma_lookup(p, vaddr);
    if (!vma) return -1;
    if (vma->vaddr_start != vaddr) return -1;
    if (vma->vaddr_end   != want_end) return -1;

    vma_remove(p, vma);
    vma_free(vma);
    return 0;
}

// =============================================================================
// Diagnostics.
// =============================================================================

// R5-F F52 close: magic check on the read-only accessors. The asymmetric
// "queries are safe to call on freed VMOs" gap from the original impl
// could let a stale pointer return plausible-looking stale values. The
// magic check rejects with 0 — a defined sentinel — for any UAF. The
// cost is one load + compare per call.
size_t burrow_get_size(const struct Burrow *v) {
    if (!v) return 0;
    if (v->magic != VMO_MAGIC) return 0;
    return v->size;
}

int burrow_handle_count(const struct Burrow *v) {
    if (!v) return 0;
    if (v->magic != VMO_MAGIC) return 0;
    return v->handle_count;
}

int burrow_mapping_count(const struct Burrow *v) {
    if (!v) return 0;
    if (v->magic != VMO_MAGIC) return 0;
    return v->mapping_count;
}

u64 burrow_total_created(void)    { return g_vmo_created; }
u64 burrow_total_destroyed(void)  { return g_vmo_destroyed; }
