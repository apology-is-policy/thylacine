// Virtual Memory Object (VMO) impl — dual-refcount lifecycle (P2-Fd /
// P3-Db).
//
// Per ARCHITECTURE.md §19 + specs/vmo.tla. Implements the spec's
// VmoCreate / HandleOpen / HandleClose / MapVmo / UnmapVmo actions on
// SLUB-allocated struct Vmo with eager-allocated backing pages.
//
// State invariant (TLC-checked in specs/vmo.tla::NoUseAfterFree):
//
//   pages alive iff (handle_count > 0 OR mapping_count > 0)
//
// Enforced at runtime by the dual-check in vmo_unref AND
// vmo_release_mapping:
//
//   if (handle_count == 0 && mapping_count == 0) vmo_free_internal(v);
//
// Either path can trigger the free; whichever brings the LAST count to
// zero releases the pages. The spec's three buggy variants
// (BUGGY_FREE_ON_HANDLE_CLOSE / BUGGY_FREE_ON_UNMAP / BUGGY_NEVER_FREE)
// each correspond to a way this dual-check could be wrong: free without
// checking the other count (premature) or fail to check at all (leak).
//
// API split (P3-Db):
//   - vmo_acquire_mapping / vmo_release_mapping: bare refcount ops
//     (formerly vmo_map / vmo_unmap pre-P3-Db). Internal to the VMA
//     layer; tests use them to exercise the refcount lifecycle.
//   - vmo_map(Proc, Vmo, vaddr, length, prot): public mapping entry —
//     calls vma_alloc + vma_insert. Returns 0/-1.
//   - vmo_unmap(Proc, vaddr, length): public unmap entry — calls
//     vma_remove + vma_free. Returns 0/-1.
//
// At v1.0 P3-Db:
//   - Anonymous VMOs only. alloc_pages(order, KP_ZERO) for the chunk.
//   - vmo_map installs a VMA only — no PTE installation. Demand paging
//     via the user-mode fault path lands at P3-Dc.
//   - Single-CPU lifecycle; Phase 5+ adds atomic ops.

#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>
#include <thylacine/vmo.h>

#include "../mm/phys.h"
#include "../mm/slub.h"

static struct kmem_cache *g_vmo_cache;
static u64                g_vmo_created;
static u64                g_vmo_destroyed;

// =============================================================================
// Lifecycle.
// =============================================================================

void vmo_init(void) {
    if (g_vmo_cache) extinction("vmo_init called twice");

    // R5-F F50/F51 close: NO PANIC_ON_FAIL. VMO allocation is reachable
    // from userspace via vmo_create syscall (Phase 5+) — userspace OOM
    // shouldn't extinct the kernel. The vmo_create_anon documented
    // contract is "returns NULL on OOM"; the cache flag must match.
    g_vmo_cache = kmem_cache_create("vmo",
                                    sizeof(struct Vmo),
                                    8,
                                    0);
    if (!g_vmo_cache) {
        extinction("kmem_cache_create(vmo) returned NULL");
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

struct Vmo *vmo_create_anon(size_t size) {
    if (!g_vmo_cache) extinction("vmo_create_anon before vmo_init");
    if (size == 0)    return NULL;
    // Overflow guard: `size + PAGE_SIZE - 1` wraps to a small value when
    // size is within (PAGE_SIZE - 1) of SIZE_MAX, producing a tiny
    // page_count for what was supposed to be an enormous request. Reject
    // such requests explicitly (the caller meant something pathological;
    // we won't silently truncate to a 1-page VMO claiming size = 0).
    if (size > SIZE_MAX - (PAGE_SIZE - 1)) return NULL;

    struct Vmo *v = kmem_cache_alloc(g_vmo_cache, KP_ZERO);
    if (!v) return NULL;

    size_t page_count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    unsigned order = order_for_pages(page_count);

    struct page *pages = alloc_pages(order, KP_ZERO);
    if (!pages) {
        kmem_cache_free(g_vmo_cache, v);
        return NULL;
    }

    v->magic         = VMO_MAGIC;
    v->type          = VMO_TYPE_ANON;
    v->size          = page_count * PAGE_SIZE;
    v->page_count    = page_count;
    v->handle_count  = 1;            // caller's initial reference
    v->mapping_count = 0;
    v->pages         = pages;
    v->order         = order;
    g_vmo_created++;
    return v;
}

// Internal: release pages + struct when both counts have reached 0.
// The caller has already verified the precondition.
static void vmo_free_internal(struct Vmo *v) {
    if (v->magic != VMO_MAGIC)
        extinction("vmo_free_internal of corrupted VMO");
    if (v->handle_count != 0)
        extinction("vmo_free_internal with handle_count > 0 (premature free; "
                   "specs/vmo.tla NoUseAfterFree violation)");
    if (v->mapping_count != 0)
        extinction("vmo_free_internal with mapping_count > 0 (premature free; "
                   "specs/vmo.tla NoUseAfterFree violation)");
    if (!v->pages)
        extinction("vmo_free_internal with pages already NULL (double-free)");

    free_pages(v->pages, v->order);
    v->pages = NULL;                  // defensive — caller shouldn't use v anymore
    kmem_cache_free(g_vmo_cache, v);
    g_vmo_destroyed++;
}

// =============================================================================
// Refcount ops.
// =============================================================================

void vmo_ref(struct Vmo *v) {
    if (!v)                       extinction("vmo_ref(NULL)");
    if (v->magic != VMO_MAGIC)    extinction("vmo_ref of corrupted VMO");
    // Defensive: a VMO with handle_count=0 AND mapping_count=0 should
    // already be freed; ref-ing it would resurrect an already-freed
    // object's identity. UAF-class bug.
    if (v->handle_count == 0 && v->mapping_count == 0)
        extinction("vmo_ref on VMO with both counts=0 (already freed?)");

    v->handle_count++;
}

void vmo_unref(struct Vmo *v) {
    if (!v) return;                            // NULL-safe
    if (v->magic != VMO_MAGIC)
        extinction("vmo_unref of corrupted VMO (use-after-free?)");
    if (v->handle_count <= 0)
        extinction("vmo_unref of zero-ref VMO");

    v->handle_count--;
    // Dual-check: free only when BOTH counts reach 0. Maps to the
    // spec's NoUseAfterFree iff invariant — premature free violates
    // (counts > 0 ∧ pages dead); delayed free violates (counts = 0 ∧
    // pages alive). The dual-check is the runtime enforcement.
    if (v->handle_count == 0 && v->mapping_count == 0) {
        vmo_free_internal(v);
    }
}

void vmo_acquire_mapping(struct Vmo *v) {
    if (!v)                       extinction("vmo_acquire_mapping(NULL)");
    if (v->magic != VMO_MAGIC)    extinction("vmo_acquire_mapping of corrupted VMO");
    if (!v->pages)
        extinction("vmo_acquire_mapping of VMO with NULL pages (use-after-free)");

    v->mapping_count++;
}

void vmo_release_mapping(struct Vmo *v) {
    if (!v)                       extinction("vmo_release_mapping(NULL)");
    if (v->magic != VMO_MAGIC)
        extinction("vmo_release_mapping of corrupted VMO (use-after-free?)");
    if (v->mapping_count <= 0)
        extinction("vmo_release_mapping of zero-mapping VMO");

    v->mapping_count--;
    // Same dual-check as vmo_unref — symmetric.
    if (v->handle_count == 0 && v->mapping_count == 0) {
        vmo_free_internal(v);
    }
}

// =============================================================================
// P3-Db: high-level map / unmap into a Proc's address space.
// =============================================================================

int vmo_map(struct Proc *p, struct Vmo *v, u64 vaddr, size_t length, u32 prot) {
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
    // takes vmo_acquire_mapping internally on success.
    struct Vma *vma = vma_alloc(vaddr, vaddr + length, prot, v, /*offset=*/0);
    if (!vma) return -1;

    // vma_insert returns -1 on overlap. On overlap, the Vma is still
    // owned by us — vma_free releases the VMO mapping ref symmetrically.
    if (vma_insert(p, vma) != 0) {
        vma_free(vma);
        return -1;
    }
    return 0;
}

int vmo_unmap(struct Proc *p, u64 vaddr, size_t length) {
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
size_t vmo_get_size(const struct Vmo *v) {
    if (!v) return 0;
    if (v->magic != VMO_MAGIC) return 0;
    return v->size;
}

int vmo_handle_count(const struct Vmo *v) {
    if (!v) return 0;
    if (v->magic != VMO_MAGIC) return 0;
    return v->handle_count;
}

int vmo_mapping_count(const struct Vmo *v) {
    if (!v) return 0;
    if (v->magic != VMO_MAGIC) return 0;
    return v->mapping_count;
}

u64 vmo_total_created(void)    { return g_vmo_created; }
u64 vmo_total_destroyed(void)  { return g_vmo_destroyed; }
