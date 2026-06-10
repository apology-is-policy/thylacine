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
// #847 (SMP): the two counts AND the dual-check run under a per-Burrow
// spin_lock (struct Burrow::lock). Without it a handle-path burrow_unref and
// a vma-path burrow_release_mapping on the same Burrow raced the free
// decision (double-free / leak), and the plain ++/-- torn-updated under
// concurrent handle_dup/handle_close -- pre-existing, reachable from a
// multi-threaded Proc (stratumd) and amplified by the #844 handle_put, which
// drops the Burrow ref OUTSIDE the handle-table lock. burrow_free_internal
// runs outside v->lock (leaf-lock discipline; it takes the buddy / mmio /
// dma locks, none of which may nest under v->lock).
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

#include "../arch/arm64/mmu.h"        // R12-vaddr: USER_VA_TOP for burrow_map upper bound
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

    // P4-N R13 F213 close: clobber magic before returning the slot to
    // SLUB. Sibling kobjs (kobj_mmio, kobj_dma) clobber per R9 F148.
    // SLUB's free path does NOT zero the slot; any stale-pointer read
    // between this kmem_cache_free and the next allocator-side write
    // would otherwise see a valid VMO_MAGIC and pass burrow_ref's /
    // burrow_acquire_mapping's magic check, masking the UAF. With
    // magic = 0 the same UAF extincts loudly.
    v->magic = 0;

    kmem_cache_free(g_vmo_cache, v);
    g_vmo_destroyed++;
}

// =============================================================================
// Refcount ops.
// =============================================================================

void burrow_ref(struct Burrow *v) {
    if (!v)                       extinction("burrow_ref(NULL)");
    if (v->magic != VMO_MAGIC)    extinction("burrow_ref of corrupted BURROW");

    // #847: the both-counts-zero check + the increment run under v->lock so a
    // sibling thread's concurrent unref / release_mapping can neither
    // torn-update the count nor race the free decision.
    spin_lock(&v->lock);
    // Defensive: both counts 0 means already-freed; ref-ing resurrects a dead
    // identity (UAF-class). Coherent under the lock.
    if (v->handle_count == 0 && v->mapping_count == 0) {
        spin_unlock(&v->lock);
        extinction("burrow_ref on BURROW with both counts=0 (already freed?)");
    }
    v->handle_count++;
    spin_unlock(&v->lock);
}

void burrow_unref(struct Burrow *v) {
    if (!v) return;                            // NULL-safe
    if (v->magic != VMO_MAGIC)
        extinction("burrow_unref of corrupted BURROW (use-after-free?)");
    // #847: decrement + the dual-counter free decision under v->lock; the
    // free runs OUTSIDE the lock (leaf discipline -- see file header).
    spin_lock(&v->lock);
    if (v->handle_count <= 0) {
        spin_unlock(&v->lock);
        extinction("burrow_unref of zero-ref BURROW");
    }
    v->handle_count--;
    // Dual-check: free only when BOTH counts reach 0. Maps to the spec's
    // NoUseAfterFree iff invariant — premature free violates (counts > 0 ∧
    // pages dead); delayed free violates (counts = 0 ∧ pages alive). Exactly
    // one racing unref/release_mapping sees the 0,0 edge.
    bool should_free = (v->handle_count == 0 && v->mapping_count == 0);
    spin_unlock(&v->lock);

    if (should_free)
        burrow_free_internal(v);
}

void burrow_acquire_mapping(struct Burrow *v) {
    if (!v)                       extinction("burrow_acquire_mapping(NULL)");
    if (v->magic != VMO_MAGIC)    extinction("burrow_acquire_mapping of corrupted BURROW");

    // #847 / RW-1 C-F4: the both-counts-zero resurrection guard + the per-type
    // liveness read + the count mutation ALL run under v->lock, mirroring
    // burrow_ref. (Previously the liveness switch read create-immutable fields
    // outside the lock -- safe at v1.0 because the sole caller holds a handle,
    // but a latent race once a sibling can free the backing resource. Moving it
    // inside the lock + adding the zero-zero guard closes it.)
    spin_lock(&v->lock);
    // Defensive: both counts 0 means already-freed; acquiring resurrects a dead
    // identity (UAF-class). Coherent under the lock (mirror burrow_ref).
    if (v->handle_count == 0 && v->mapping_count == 0) {
        spin_unlock(&v->lock);
        extinction("burrow_acquire_mapping on BURROW with both counts=0 (already freed?)");
    }
    // P4-Ic1: per-type liveness. ANON: pages alive; MMIO/DMA: the kobj held.
    switch (v->type) {
    case BURROW_TYPE_ANON:
        if (!v->pages) {
            spin_unlock(&v->lock);
            extinction("burrow_acquire_mapping of ANON BURROW with NULL pages (UAF)");
        }
        break;
    case BURROW_TYPE_MMIO:
        if (!v->kobj_mmio) {
            spin_unlock(&v->lock);
            extinction("burrow_acquire_mapping of MMIO BURROW with NULL kobj_mmio (UAF)");
        }
        break;
    case BURROW_TYPE_DMA:
        if (!v->kobj_dma) {
            spin_unlock(&v->lock);
            extinction("burrow_acquire_mapping of DMA BURROW with NULL kobj_dma (UAF)");
        }
        break;
    case BURROW_TYPE_INVALID:
    default:
        spin_unlock(&v->lock);
        extinction("burrow_acquire_mapping: invalid burrow type");
    }
    v->mapping_count++;
    spin_unlock(&v->lock);
}

void burrow_release_mapping(struct Burrow *v) {
    if (!v)                       extinction("burrow_release_mapping(NULL)");
    if (v->magic != VMO_MAGIC)
        extinction("burrow_release_mapping of corrupted BURROW (use-after-free?)");
    // #847: symmetric with burrow_unref -- decrement + dual-check under
    // v->lock, free outside.
    spin_lock(&v->lock);
    if (v->mapping_count <= 0) {
        spin_unlock(&v->lock);
        extinction("burrow_release_mapping of zero-mapping BURROW");
    }
    v->mapping_count--;
    bool should_free = (v->handle_count == 0 && v->mapping_count == 0);
    spin_unlock(&v->lock);

    if (should_free)
        burrow_free_internal(v);
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
    // R12-vaddr close of F180 (R12-DMA deferred): SYS_DMA_MAP accepted
    // vaddr ≥ 2^47, deferring rejection to mmu_install_user_pte (R10 F158
    // MMU-layer reject) — too late, because by then vma_alloc + vma_insert
    // had already populated the per-Proc tree. Reject at the VMA layer so
    // MMIO + DMA + anon callers fail uniformly before any state changes.
    _Static_assert(USER_VA_TOP == (1ull << 47),
        "USER_VA_TOP must equal mmu_install_user_pte's (vaddr >> 47) bound");
    if (vaddr + length > USER_VA_TOP) return -1;

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

    // P6 hardening #2 / audit F1 (P1): clear the leaf PTEs in the
    // per-Proc TTBR0 tree AND invalidate the TLB for the unmapped
    // range BEFORE returning the pages to the buddy. Without this
    // step, the PTEs stay valid pointing at PAs that the buddy is
    // about to recycle to a different mapping -- a later access to
    // the same VA (dangling pointer, sibling-buffer overflow,
    // mallocng's slot-canary read on a re-attached VA that happens
    // to get a different PA) silently routes through the stale PTE
    // / TLB entry to the now-recycled page. This is content-
    // sensitive (the trigger depends on buddy LIFO returning the
    // same PA vs a different one) and is the suspected root cause
    // of the 16b-gamma-mount-bind AEGIS-256 / mallocng heap
    // corruption.
    //
    // mmu_uninstall_user_range walks per-page; for each, it walks
    // L0..L3, clears the leaf PTE, and issues tlbi vaae1is + dsb
    // ish + isb. Returns 0 even if some pages were never faulted
    // in (PTE absent at L3, or any of L1/L2/L3 sub-tables absent
    // -- the dual-refcount lifecycle is still safe because
    // burrow_release_mapping in vma_free runs regardless of how
    // many pages were demand-paged).
    //
    // Note: pgtable_root may be 0 in pathological cases (early-
    // boot helper paths that synthesize a Proc without an MMU
    // arm). mmu_uninstall_user_range rejects pgtable_root == 0;
    // we ignore the rc (vma_free still runs).
    // RW-1 B-F1: the asid arg is vestigial (mmu_uninstall_user_range does an
    // all-ASID `tlbi vaae1is`); pass 0 now that the Proc has no permanent ASID.
    (void)mmu_uninstall_user_range(p->pgtable_root, 0,
                                   vaddr, want_end);

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
