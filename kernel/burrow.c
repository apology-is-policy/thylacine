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
#include <thylacine/addrspace.h>     // LINEAGE L-4a: addrspace_charge_pages for the populate charge
#include <thylacine/cow.h>           // LINEAGE L-4b: the per-page COW share count
#include <thylacine/extinction.h>
#include <thylacine/mmio_handle.h>   // P4-Ic1: kobj_mmio_ref/unref for MMIO Burrows
#include <thylacine/dma_handle.h>    // P4-Ic5b1b: kobj_dma_ref/unref for DMA Burrows
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>        // REVENANT: spoor_ref/clunk + SPOOR_MAGIC + qid for BURROW_TYPE_FILE
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

// #106: the number of physical pages an EAGER Burrow of `size` bytes actually
// OCCUPIES -- the buddy's power-of-two rounding, not the page-rounded request.
// Every I-32 charge site must use this, never size / PAGE_SIZE.
//
// alloc_pages(order) takes 1 << order pages out of the buddy -- its KP_ZERO
// loop zeroes (1 << order) << PAGE_SHIFT bytes, which is the allocation's own
// statement of its size -- so a 2049-page request occupies 4096. Charging the
// request instead of the occupancy let a Proc hold up to ~2x its I-32 budget:
// page_count could read at most PROC_PAGE_MAX while real occupancy approached
// twice that. Bounded at 2x (the next order is never more than double), which
// is why this is an understated floor rather than an unbounded hole.
//
// The waste itself is deliberate and STAYS. A Burrow's backing must be ONE
// physically contiguous run -- exec's direct-map alias, loom_create's ring_kva,
// and the weft ring view all index v->pages as a single chunk -- and a buddy
// allocator buys that contiguity with power-of-two rounding. Accounting for the
// waste is the fix available to us; eliminating it would mean giving up
// contiguity, which is load-bearing.
//
// Shares order_for_pages with burrow_create_anon / burrow_create_code, so the
// charge and the allocation cannot drift apart by construction;
// burrow.backing_pages_matches_alloc pins that agreement against a REAL
// Burrow's recorded order, so a future change to either side that breaks the
// correspondence fails a test rather than silently re-opening the undercount.
//
// Returns 0 for exactly the two inputs that cannot produce a Burrow at all
// (size 0, and a size whose page round-up would wrap) -- the same two guards
// the creators reject on, so a caller that charges before creating and a
// creator that then refuses agree on "no pages".
size_t burrow_backing_pages(size_t size) {
    if (size == 0)                          return 0;
    if (size > SIZE_MAX - (PAGE_SIZE - 1))  return 0;
    return (size_t)1u << order_for_pages((size + PAGE_SIZE - 1) / PAGE_SIZE);
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

// REVENANT / I-36: file-backed demand-paged text Burrow (the Plan 9 Image as
// BURROW_TYPE_FILE; docs/REVENANT.md §4). ADOPTS one ref on `spoor` — the store
// is the last step, so every error path below returns NULL having taken NO ref
// (the caller retains ownership + must clunk it). Mirrors burrow_create_anon's
// page_count/size rounding, but allocates NO backing pages: the per-page
// physical pages fault in lazily (R-2) into the sparse `filepages` array, which
// is the only allocation made here.
struct Burrow *burrow_create_file(struct Spoor *spoor, u64 file_offset, size_t length) {
    if (!g_vmo_cache) extinction("burrow_create_file before burrow_init");
    if (!spoor)       return NULL;
    if (spoor->magic != SPOOR_MAGIC)
        extinction("burrow_create_file: spoor has bad magic (UAF?)");
    if (length == 0)  return NULL;
    // Overflow guard (mirror burrow_create_anon): size + PAGE_SIZE - 1 must not
    // wrap. page_count * sizeof(struct page *) then cannot overflow either
    // (page_count <= SIZE_MAX/PAGE_SIZE, so * 8 stays < SIZE_MAX).
    if (length > SIZE_MAX - (PAGE_SIZE - 1)) return NULL;

    size_t page_count = (length + PAGE_SIZE - 1) / PAGE_SIZE;

    struct Burrow *v = kmem_cache_alloc(g_vmo_cache, KP_ZERO);
    if (!v) return NULL;

    // The sparse per-page array: page_count slots, all NULL ("not resident").
    // The R-2 fault arm fills a slot on the first fault to that page;
    // burrow_free_internal frees every non-NULL slot. A few KiB even for a
    // multi-MiB segment (8 bytes/page).
    struct page **filepages = kmalloc(page_count * sizeof(struct page *), KP_ZERO);
    if (!filepages) {
        kmem_cache_free(g_vmo_cache, v);
        return NULL;     // no ref taken: caller still owns `spoor`
    }

    v->magic         = VMO_MAGIC;
    v->type          = BURROW_TYPE_FILE;
    v->size          = page_count * PAGE_SIZE;
    v->page_count    = page_count;
    v->handle_count  = 1;            // construction reference
    v->mapping_count = 0;
    v->pages         = NULL;         // FILE: no contiguous alloc_pages chunk
    v->order         = 0;
    // Adopt the caller's ref (loom_register_handles "adopt the caller's ref"
    // discipline) — stored directly, no spoor_ref bump. burrow_free_internal
    // spoor_clunks it on the last unref (the I-30 pin held for the Burrow's life).
    v->spoor         = spoor;
    v->file_offset   = file_offset;
    // KP_ZERO zeroed the struct, and 0 is a REAL size (an empty file, where
    // every page is past EOF) -- the no-bound default must be explicit (#194).
    v->file_limit    = BURROW_FILE_LIMIT_UNKNOWN;
    v->file_dc       = spoor->dc;
    v->file_devno    = spoor->devno;
    v->file_qid_path = spoor->qid.path;
    v->file_qid_vers = spoor->qid.vers;
    v->filepages     = filepages;
    g_vmo_created++;
    return v;
}

// Overcommit / I-32: the demand-ZERO anonymous Burrow (ARCH §6.5 "The overcommit
// model"; SYS_BURROW_ATTACH_LAZY). The structural twin of burrow_create_file minus
// the backing Spoor: mirrors its page_count/size rounding + the sparse `filepages`
// array (the only allocation), but allocates NO backing pages — each page faults in
// zero-filled (R-2-style) on first touch. No spoor ref taken (anon).
struct Burrow *burrow_create_anon_lazy(size_t size) {
    if (!g_vmo_cache) extinction("burrow_create_anon_lazy before burrow_init");
    if (size == 0)    return NULL;
    // Overflow guard (mirror burrow_create_anon/file): size + PAGE_SIZE - 1 must not
    // wrap; page_count * sizeof(struct page *) then cannot overflow either.
    if (size > SIZE_MAX - (PAGE_SIZE - 1)) return NULL;

    size_t page_count = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    struct Burrow *v = kmem_cache_alloc(g_vmo_cache, KP_ZERO);
    if (!v) return NULL;

    // The sparse per-page array: page_count slots, all NULL ("not resident"). The
    // demand-zero fault arm fills a slot on the first fault to that page;
    // burrow_free_internal frees every non-NULL slot. Same shape (8 bytes/page) as
    // the FILE Burrow's filepages.
    struct page **filepages = kmalloc(page_count * sizeof(struct page *), KP_ZERO);
    if (!filepages) {
        kmem_cache_free(g_vmo_cache, v);
        return NULL;
    }

    v->magic         = VMO_MAGIC;
    v->type          = BURROW_TYPE_ANON_LAZY;
    v->size          = page_count * PAGE_SIZE;
    v->page_count    = page_count;
    v->handle_count  = 1;            // construction reference
    v->mapping_count = 0;
    v->pages         = NULL;         // LAZY: no contiguous alloc_pages chunk
    v->order         = 0;
    v->filepages     = filepages;
    g_vmo_created++;
    return v;
}

// I-42 / CL-7k: the dual-mappable CODE Burrow (docs/JIT-ON-WX-DESIGN.md).
// Backing is byte-identical to burrow_create_anon -- one eager contiguous
// KP_ZERO chunk -- and deliberately so: the type is an ADMISSIBILITY token, not
// a different allocator. Keeping the backing identical means the CODE arm of
// every downstream switch (free, acquire-liveness, demand-page) is the ANON arm,
// so the new type adds a gate without adding a lifetime.
//
// KP_ZERO is load-bearing, not hygiene: an executable page handed back with a
// previous owner's bytes would be code the Proc can RUN without having emitted
// it. All-zero AArch64 is UDF #0 (permanently undefined), so an un-emitted page
// traps instead of executing residue.
struct Burrow *burrow_create_code(size_t size) {
    if (!g_vmo_cache) extinction("burrow_create_code before burrow_init");
    if (size == 0)    return NULL;
    // Same overflow guard as burrow_create_anon: size + PAGE_SIZE - 1 must not
    // wrap, or an enormous request would silently become a tiny page_count.
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
    v->type          = BURROW_TYPE_CODE;
    v->size          = page_count * PAGE_SIZE;
    v->page_count    = page_count;
    v->handle_count  = 1;            // construction reference
    v->mapping_count = 0;
    v->pages         = pages;
    v->order         = order;
    g_vmo_created++;
    return v;
}

// LINEAGE L-4a: pre-populate a run of ANON_LAZY slots (see burrow.h for the
// contract). Structurally the demand-zero fault arm's body, hoisted to exec time
// and run over a range instead of one slot -- same charge, same alloc, same
// install-once, same "buddy is never entered under v->lock" discipline.
int burrow_lazy_populate(struct AddrSpace *as, bool exempt, struct Burrow *v,
                         size_t first, size_t n) {
    if (!as || !v)                            return -1;
    if (v->magic != VMO_MAGIC)                return -1;
    if (v->type != BURROW_TYPE_ANON_LAZY)     return -1;
    if (!v->filepages)                        return -1;
    if (n == 0)                               return 0;
    // Range within the Burrow, written so neither term can overflow.
    if (first >= v->page_count)               return -1;
    if (n > v->page_count - first)            return -1;
    if (n > 0xFFFFFFFFull)                    return -1;   // the u32 charge argument

    // Charge the WHOLE run before allocating anything: the cap decision has to see
    // the entire request, or a run straddling PROC_PAGE_MAX would populate its
    // first half and only then discover it cannot finish. as->lock is the stated
    // precondition of addrspace_charge_pages -- it is what makes the cap exact
    // against a sibling charge on the same address space.
    spin_lock(&as->lock);
    bool charged = addrspace_charge_pages(as, (u32)n, exempt);
    spin_unlock(&as->lock);
    if (!charged)                             return -1;   // over cap -> caller -ENOMEM

    size_t done = 0;
    for (; done < n; done++) {
        // alloc OUTSIDE v->lock: alloc_pages takes the buddy lock, and burrow.c's
        // leaf-lock discipline keeps the buddy strictly below v->lock (the same
        // reason burrow_decommit frees after unlocking).
        struct page *pg = alloc_pages(0, KP_ZERO);
        if (!pg) break;                       // OOM -> unwind below

        // LINEAGE L-4b: entry site 1 of 3. ESTABLISH the COW share before the
        // page becomes reachable through the slot -- a page just out of the
        // buddy carries its previous owner's count, and inheriting that would
        // free a page some other address space still maps. Done here, while pg
        // is still private to this call, so it needs no nesting under v->lock.
        cow_page_set_sole(pg);

        spin_lock(&v->lock);
        bool installed = (v->filepages[first + done] == NULL);
        if (installed) v->filepages[first + done] = pg;   // the Burrow owns pg now
        spin_unlock(&v->lock);

        if (!installed) {
            // The slot was ALREADY resident. exec populates each run exactly once
            // and the Burrow is private to it, so this is a caller bug rather than a
            // race -- fail closed instead of silently leaving the run's charge
            // describing more pages than this call actually installed.
            free_pages(pg, 0);                // outside v->lock (leaf order)
            break;
        }
    }

    if (done != n) {
        // All-or-nothing: undo exactly what this call did, so the Burrow is left as
        // it was found and a caller that gives up can simply burrow_unref.
        for (size_t i = 0; i < done; i++) {
            spin_lock(&v->lock);
            struct page *pg = v->filepages[first + i];
            v->filepages[first + i] = NULL;
            spin_unlock(&v->lock);
            // These pages WERE in slots, so they leave through the share count
            // rather than by a direct free: the rule this file keeps is that a
            // page in a slot is released with cow_page_put and freed only when
            // that put reports the last holder. Nothing can have taken a share
            // of these (the Burrow is private to a caller that is still inside
            // this call), so the put always reports last -- but routing it here
            // anyway is what keeps the rule checkable by inspection.
            if (pg && cow_page_put(pg))
                free_pages(pg, 0);            // outside v->lock (leaf order)
        }
        spin_lock(&as->lock);
        addrspace_uncharge_pages(as, (u32)n); // the whole charge, matching the grant
        spin_unlock(&as->lock);
        return -1;
    }
    return 0;
}

// LINEAGE L-4b: dupseg. See burrow.h for why a fork clones the Burrow instead of
// sharing a reference to it, and cow.h for why that forces the share count onto
// the page.
struct Burrow *burrow_clone_cow(struct Burrow *src) {
    if (!g_vmo_cache)                       extinction("burrow_clone_cow before burrow_init");
    if (!src)                               return NULL;
    if (src->magic != VMO_MAGIC)            return NULL;
    if (src->type != BURROW_TYPE_ANON_LAZY) return NULL;
    if (!src->filepages)                    return NULL;
    if (src->page_count == 0)               return NULL;

    // Allocate BOTH before taking a single share: an OOM after the shares were
    // taken would have to walk them back, and the whole point of doing the
    // allocation first is that there is then nothing to unwind.
    struct Burrow *dst = kmem_cache_alloc(g_vmo_cache, KP_ZERO);
    if (!dst) return NULL;
    struct page **slots = kmalloc(src->page_count * sizeof(struct page *), KP_ZERO);
    if (!slots) {
        kmem_cache_free(g_vmo_cache, dst);
        return NULL;
    }

    dst->magic         = VMO_MAGIC;
    dst->type          = BURROW_TYPE_ANON_LAZY;
    dst->size          = src->size;
    dst->page_count    = src->page_count;
    dst->handle_count  = 1;              // construction reference, as at create
    dst->mapping_count = 0;
    dst->pages         = NULL;
    dst->order         = 0;
    dst->filepages     = slots;

    // Snapshot under src->lock so a slot cannot be filled halfway through the
    // walk (the caller additionally holds the source address space's lock, which
    // excludes its own faulters; this is the in-file discipline). cow_page_get
    // nests beneath, which is the stated as->lock -> v->lock -> g_cow_lock order.
    //
    // The share is taken for the slot that is about to point at the page, and the
    // slot is written in the same hold -- so there is no window in which the clone
    // references a page it has not counted.
    spin_lock(&src->lock);
    for (size_t i = 0; i < src->page_count; i++) {
        struct page *pg = src->filepages[i];
        if (!pg) continue;               // never touched: stays NULL, reads as zero
        cow_page_get(pg);
        slots[i] = pg;
    }
    spin_unlock(&src->lock);

    g_vmo_created++;
    return dst;
}

// LINEAGE L-4b: the COW break's commit. Kept here so filepages[] and v->lock stay
// burrow.c's alone; the share-count ordering around it is the caller's, and is
// spelled out in burrow.h.
bool burrow_lazy_swap_slot(struct Burrow *v, size_t slot,
                           struct page *expect, struct page *replacement) {
    if (!v)                               return false;
    if (v->magic != VMO_MAGIC)            return false;
    if (v->type != BURROW_TYPE_ANON_LAZY) return false;
    if (!expect || !replacement)          return false;

    spin_lock(&v->lock);
    bool ok = (v->filepages && slot < v->page_count &&
               v->filepages[slot] == expect);
    if (ok) v->filepages[slot] = replacement;
    spin_unlock(&v->lock);
    return ok;
}

// LINEAGE L-4a: the direct-map address of one resident ANON_LAZY slot. The
// caller-privacy precondition is stated in burrow.h and is what makes returning a
// raw page pointer sound here; no lock is taken because there is, by that
// precondition, nobody to race with.
void *burrow_lazy_slot_kva(struct Burrow *v, size_t slot) {
    if (!v)                                     return NULL;
    if (v->magic != VMO_MAGIC)                  return NULL;
    if (v->type != BURROW_TYPE_ANON_LAZY)       return NULL;
    if (!v->filepages || slot >= v->page_count) return NULL;
    struct page *pg = v->filepages[slot];
    return pg ? (void *)pa_to_kva(page_to_pa(pg)) : NULL;
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
    case BURROW_TYPE_CODE:
        // I-42: a CODE Burrow's backing IS an ANON Burrow's -- one contiguous
        // eager chunk -- so it frees identically. The type differs only in what
        // it AUTHORIZES (an RX alias), never in what it owns, which is why it
        // shares this arm rather than duplicating it. The two aliases a code
        // region carries are VMAs, and both must be gone before this runs: the
        // #847 dual count reaches {0,0} only after each alias's vma_free has
        // dropped its own mapping_count, so the pages outlive BOTH views.
        if (!v->pages)
            extinction("burrow_free_internal(ANON/CODE) with pages already NULL (double-free)");
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
    case BURROW_TYPE_FILE:
        // REVENANT / I-36: free every resident demand-paged page (order 0
        // each), then the sparse array, then clunk the adopted backing Spoor.
        // Runs at {handle_count==0 && mapping_count==0}, so no VMA maps this
        // Burrow and no concurrent fault touches filepages -- the walk needs no
        // lock. spoor_clunk (dev->close + unref) may sleep (a 9P Tclunk);
        // burrow_free_internal runs OUTSIDE v->lock (leaf discipline), so the
        // sleep is safe. The spoor==NULL double-free guard mirrors MMIO/DMA.
        if (!v->spoor)
            extinction("burrow_free_internal(FILE) with spoor already NULL (double-free)");
        if (v->filepages) {
            for (size_t i = 0; i < v->page_count; i++) {
                if (v->filepages[i]) {
                    free_pages(v->filepages[i], 0);
                    v->filepages[i] = NULL;
                }
            }
            kfree(v->filepages);
            v->filepages = NULL;
        }
        spoor_clunk(v->spoor);
        v->spoor = NULL;
        break;
    case BURROW_TYPE_ANON_LAZY:
        // Overcommit / I-32: free every resident demand-zeroed page (order 0 each),
        // then the sparse array. Runs at {handle_count==0 && mapping_count==0}, so no
        // VMA maps this Burrow and no concurrent faulter touches filepages -- the walk
        // needs no lock (mirrors the FILE arm, minus the spoor_clunk; no kobj). The
        // filepages==NULL double-free guard mirrors ANON's pages==NULL. NOTE: the
        // per-Proc page_count for any STILL-resident page is uncharged by the caller
        // BEFORE this runs (SYS_BURROW_DETACH reads burrow_lazy_resident_count then
        // uncharges; burrow_free_internal is Proc-agnostic so it cannot uncharge).
        if (!v->filepages)
            extinction("burrow_free_internal(ANON_LAZY) with filepages already NULL (double-free)");
        for (size_t i = 0; i < v->page_count; i++) {
            if (v->filepages[i]) {
                // LINEAGE L-4b: a page here may be COW-shared with a Burrow
                // this fork's sibling holds, so the free is CONDITIONAL -- the
                // page returns to the buddy only when this was its last holder.
                // The walk itself still needs no lock (this runs at
                // handle_count == 0 && mapping_count == 0, so nothing maps this
                // Burrow), but cow_page_put does take the global COW lock,
                // because the OTHER holder is reachable from a live address
                // space and may be putting the same page concurrently.
                if (cow_page_put(v->filepages[i]))
                    free_pages(v->filepages[i], 0);
                v->filepages[i] = NULL;
            }
        }
        kfree(v->filepages);
        v->filepages = NULL;
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

bool burrow_unref_freed(struct Burrow *v) {
    if (!v) return false;                      // NULL-safe
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
    return should_free;
}

void burrow_unref(struct Burrow *v) { (void)burrow_unref_freed(v); }

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
    case BURROW_TYPE_CODE:
        // I-42: identical backing -> identical liveness test (see the free arm).
        if (!v->pages) {
            spin_unlock(&v->lock);
            extinction("burrow_acquire_mapping of ANON/CODE BURROW with NULL pages (UAF)");
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
    case BURROW_TYPE_FILE:
        // REVENANT / I-36: liveness is the pinned backing Spoor. filepages MAY
        // be all-NULL (no page faulted in yet) -- the normal fresh-mapping
        // state, not a UAF. spoor==NULL only post-free (the {0,0} resurrection
        // guard above already covers the freed case).
        if (!v->spoor) {
            spin_unlock(&v->lock);
            extinction("burrow_acquire_mapping of FILE BURROW with NULL spoor (UAF)");
        }
        break;
    case BURROW_TYPE_ANON_LAZY:
        // Overcommit: liveness is the sparse array. filepages MAY be all-NULL (no
        // page faulted in yet, or fully decommitted) -- the normal state, not a UAF
        // (mirror FILE). filepages==NULL only post-free (the {0,0} resurrection guard
        // above already covers the freed case).
        if (!v->filepages) {
            spin_unlock(&v->lock);
            extinction("burrow_acquire_mapping of ANON_LAZY BURROW with NULL filepages (UAF)");
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

bool burrow_release_mapping_freed(struct Burrow *v) {
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
    return should_free;
}

void burrow_release_mapping(struct Burrow *v) { (void)burrow_release_mapping_freed(v); }

// D-3c F1: the deferred twin of burrow_release_mapping_freed. Drops the mapping
// ref under v->lock and, if that was the last ref, returns the now-dead Burrow
// WITHOUT freeing it -- the caller (holding as->lock) pushes it onto a local
// stack and calls burrow_free_deferred after the unlock, so the FILE arm's
// possibly-sleeping spoor_clunk never runs under a spinlock. Returns NULL when
// the Burrow survives (a handle or another mapping still holds it).
struct Burrow *burrow_release_mapping_deferred(struct Burrow *v) {
    if (!v)                       extinction("burrow_release_mapping(NULL)");
    if (v->magic != VMO_MAGIC)
        extinction("burrow_release_mapping of corrupted BURROW (use-after-free?)");
    spin_lock(&v->lock);
    if (v->mapping_count <= 0) {
        spin_unlock(&v->lock);
        extinction("burrow_release_mapping of zero-mapping BURROW");
    }
    v->mapping_count--;
    bool should_free = (v->handle_count == 0 && v->mapping_count == 0);
    spin_unlock(&v->lock);
    return should_free ? v : NULL;
}

// D-3c F1: free a Burrow the caller took off a deferred-free stack. Runs the
// same burrow_free_internal every unref/release-mapping funnels into -- but at
// a point where NO lock is held, so a sleeping teardown is legal. The {0,0}
// state was established under v->lock by burrow_release_mapping_deferred and
// nothing else can reach the Burrow, so no re-check is needed.
void burrow_free_deferred(struct Burrow *v) {
    if (!v) return;
    if (v->magic != VMO_MAGIC)
        extinction("burrow_free_deferred of corrupted BURROW (double free?)");
    burrow_free_internal(v);
}

// =============================================================================
// #131/#132: the I-32 charge record. See burrow.h for the contract + the
// rationale (a Burrow's TYPE tells you the region's shape, never who paid).
//
// All three run under v->lock -- a leaf, and the same lock the dual refcount
// uses, so a claim is serialized against the free decision it precedes. They
// must NEVER be called from inside burrow_unref_freed / burrow_release_
// mapping_freed (which hold that lock themselves); every caller claims first,
// then drops.
// =============================================================================

void burrow_charge_record(struct Burrow *v, const struct Proc *p, u32 pages) {
    if (!v || !p || pages == 0) return;
    if (v->magic != VMO_MAGIC)
        extinction("burrow_charge_record on corrupted BURROW (use-after-free?)");
    spin_lock(&v->lock);
    v->charge_pid   = p->pid;
    v->charge_pages = pages;
    spin_unlock(&v->lock);
}

u32 burrow_charge_claim(struct Burrow *v, const struct Proc *p) {
    if (!v || !p) return 0;
    if (v->magic != VMO_MAGIC)
        extinction("burrow_charge_claim on corrupted BURROW (use-after-free?)");
    u32 pages = 0;
    spin_lock(&v->lock);
    // charge_pages -- not charge_pid -- is the "held" sentinel. pid 0 is a real
    // value (proc_alloc stamps 0 and rfork_internal assigns the pid later), so
    // using it as the released marker would conflate identity with state and
    // silently refuse to settle a charge recorded before a pid was assigned.
    // A charge of zero pages is meaningless, so zero pages IS "nothing held".
    if (v->charge_pages != 0 && v->charge_pid == p->pid) {
        pages           = v->charge_pages;
        v->charge_pid   = 0;
        v->charge_pages = 0;
    }
    spin_unlock(&v->lock);
    return pages;
}

void burrow_charge_restore(struct Burrow *v, const struct Proc *p, u32 pages) {
    if (!v || !p || pages == 0) return;
    if (v->magic != VMO_MAGIC)
        extinction("burrow_charge_restore on corrupted BURROW (use-after-free?)");
    spin_lock(&v->lock);
    // The record must still be the one WE cleared. A held record here means
    // something re-charged the region between the claim and the restore, which
    // cannot happen: burrow_charge_record runs once, at creation, on a Burrow no
    // other path has a reference to yet. Extinct rather than pick a loser --
    // silently refusing would DROP the caller's `pages` on the floor, and a lost
    // charge is an under-count, the direction that inflates a Proc's budget.
    if (v->charge_pages != 0) {
        spin_unlock(&v->lock);
        extinction("burrow_charge_restore: the region was re-charged mid-settle");
    }
    v->charge_pid   = p->pid;
    v->charge_pages = pages;
    spin_unlock(&v->lock);
}

bool burrow_is_shared_out(const struct Burrow *v) {
    if (!v || v->magic != VMO_MAGIC) return false;
    // Read under the same leaf lock the setter uses -- the flag is a plain bool
    // and this costs one uncontended acquire on a path that is already taking
    // it (the charge claim follows immediately), so there is no reason to reason
    // about byte-store atomicity instead.
    //
    // Monotonic false -> true, so a MISS is the only possible staleness: a share
    // that lands after this read leaves the sharer charged until its next
    // release point. That is an over-charge on the sharer -- never a refund to a
    // Proc that did not pay -- so the race degrades in the safe direction.
    struct Burrow *m = (struct Burrow *)v;   // the lock is mutable state on a logically-const query
    spin_lock(&m->lock);
    bool out = m->shared_out;
    spin_unlock(&m->lock);
    return out;
}

// =============================================================================
// P3-Db: high-level map / unmap into a Proc's address space.
// =============================================================================

int burrow_map(struct Proc *p, struct Burrow *v, u64 vaddr, size_t length, u32 prot) {
    if (!p) return -1;
    return burrow_map_in(p->as, proc_resource_exempt(p), v, vaddr, length, prot);
}

// LINEAGE L-2: map into an explicit address space. burrow_map's ONLY use of the
// Proc was vma_insert's, so this is the whole of it -- the map installs no PTEs
// (every page is demand-paged) and touches no per-Proc state, which is what
// makes the exec load path address-space-agnostic once the target is threaded
// through. See vma.h for why exec needs a detached target.
int burrow_map_in(struct AddrSpace *as, bool exempt, struct Burrow *v,
                  u64 vaddr, size_t length, u32 prot) {
    if (!as || !v) return -1;
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
    if (vma_insert_in(as, exempt, vma) != 0) {
        vma_free(vma);
        return -1;
    }
    return 0;
}

// DISTRO D-3b: burrow_map_fixed -- install `v` over [vaddr, vaddr+length),
// which must lie wholly inside one existing VMA, splitting that VMA around the
// window. The MAP_FIXED half of the phenotype mmap; see vma.h's
// vma_replace_range_in for the surgery's contract and its hole-free argument.
//
// This wrapper owns exactly two things the surgery does not: the address
// arithmetic (the same bounds burrow_map_in enforces) and the PTE teardown.
//
// The teardown comes FIRST, and the order is load-bearing. Hardware resolves a
// leaf PTE without taking as->lock, so a thread already in EL0 could read the
// window through a stale PTE at any instant -- the only thing that stops it is
// the PTE being gone and the TLB invalidated, after which the access faults and
// blocks on the lock we hold. Mutating first and clearing second would leave a
// window in which the new mapping is live but old bytes are still reachable.
//
// A REFUSED surgery therefore costs one wasted teardown of a window we did not
// modify. That is benign -- every page refaults from backing that never changed
// -- and it is the deliberate price of keeping the VMA-shape rules in ONE place
// (the surgery) instead of mirroring them here to predict the refusal.
int burrow_map_fixed_in(struct AddrSpace *as, bool exempt, struct Burrow *v,
                        u64 vaddr, size_t length, u32 prot, u64 burrow_offset,
                        struct Burrow **out_free) {
    // D-3c re-audit F5: the surgery may free a sleeping-free FILE Burrow on its
    // exact-cover arm; defer it to the caller past as->lock (see vma.h). NULL on
    // every path but that free.
    // F7 (re-audit round 3): out_free MANDATORY -- a NULL would leak the replaced
    // Burrow (vma_free_deferred does not free). Fail loud at entry, F6 parity.
    if (!out_free) extinction("burrow_map_fixed_in without out_free (would leak the replaced Burrow)");
    *out_free = NULL;
    if (!as || !v) return -1;
    if (length == 0) return -1;
    if (vaddr  & (PAGE_SIZE - 1)) return -1;
    if (length & (PAGE_SIZE - 1)) return -1;
    if (vaddr + length < vaddr) return -1;
    if (vaddr + length > USER_VA_TOP) return -1;

    // RW-1 B-F1: the asid arg is vestigial (all-ASID `tlbi vaae1is`); pass 0.
    (void)mmu_uninstall_user_range(as->pgtable_root, 0, vaddr, vaddr + length);

    return vma_replace_range_in(as, exempt, vaddr, (u64)length,
                                v, prot, burrow_offset, out_free);
}

int burrow_map_fixed(struct Proc *p, struct Burrow *v, u64 vaddr, size_t length,
                     u32 prot, u64 burrow_offset, struct Burrow **out_free) {
    // F7 (re-audit round 3): out_free MANDATORY (F6 parity; a NULL leaks the
    // replaced Burrow). Fail loud at the first entry rather than deep in the surgery.
    if (!out_free) extinction("burrow_map_fixed without out_free (would leak the replaced Burrow)");
    *out_free = NULL;
    if (!p) return -1;
    return burrow_map_fixed_in(p->as, proc_resource_exempt(p), v, vaddr, length,
                               prot, burrow_offset, out_free);
}

int burrow_unmap_reporting(struct Proc *p, u64 vaddr, size_t length,
                           bool *out_freed, struct Burrow **out_free) {
    if (out_freed) *out_freed = false;
    if (out_free)  *out_free  = NULL;
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
    (void)mmu_uninstall_user_range(p->as->pgtable_root, 0,
                                   vaddr, want_end);

    // G-2: a SHARED_IN VMA's teardown uncharges the client's shared-in budget
    // (paired with burrow_share_into's charge; same p->vma_lock hold, so the
    // pairing is exact). Covers both the explicit SYS_BURROW_DETACH of a
    // shared VA and the weave-fid clunk unmap.
    if (vma->flags & VMA_FLAG_SHARED_IN)
        proc_shared_map_uncharge(p, (u32)(length / PAGE_SIZE));

    vma_remove(p, vma);
    // D-3c F1: DEFER the Burrow free when the caller asked for it (out_free
    // non-NULL) -- the caller holds as->lock, and a FILE Burrow's free reaches
    // a possibly-sleeping spoor_clunk. `vma_free_deferred` hands back the
    // dead Burrow; the caller frees it after the unlock. When out_free is NULL
    // (the burrow_unmap wrapper: JIT / Loom / weft / cons -- all ANON/CODE/DMA,
    // whose free never sleeps) the inline free is kept.
    bool freed;
    if (out_free) {
        *out_free = vma_free_deferred(vma, &freed);
    } else {
        freed = vma_free_freed(vma);
    }
    if (out_freed) *out_freed = freed;
    return 0;
}

int burrow_unmap(struct Proc *p, u64 vaddr, size_t length) {
    return burrow_unmap_reporting(p, vaddr, length, NULL, NULL);
}

// =============================================================================
// Overcommit / I-32: lazy-anon decommit + resident-page accounting (ARCH §6.5).
// =============================================================================

// burrow_decommit: the madvise(MADV_DONTNEED) analog (SYS_BURROW_DECOMMIT). Release
// the resident pages backing [vaddr, vaddr+length) of a BURROW_TYPE_ANON_LAZY
// mapping WITHOUT removing the VMA. See burrow.h for the full contract. The caller
// (sys_burrow_decommit_for_proc) holds p->vma_lock; that lock excludes a concurrent
// faulter from filepages (a fault on the same Proc serializes on vma_lock), so the
// per-slot read/NULL is race-free against the install-once fault arm.
int burrow_decommit(struct Proc *p, u64 vaddr, size_t length) {
    if (!p)                          return -1;
    if (length == 0)                 return -1;
    if (vaddr & (PAGE_SIZE - 1))     return -1;
    if (length & (PAGE_SIZE - 1))    return -1;
    u64 end = vaddr + length;
    if (end < vaddr)                 return -1;     // overflow
    if (end > USER_VA_TOP)           return -1;

    // The range must fall WITHIN a single BURROW_TYPE_ANON_LAZY VMA. vma_lookup
    // finds the VMA covering vaddr; require it to span the whole [vaddr, end) and be
    // ANON_LAZY (decommit is meaningless on eager anon / FILE / MMIO / DMA).
    struct Vma *vma = vma_lookup(p, vaddr);
    if (!vma)                                       return -1;
    if (vaddr < vma->vaddr_start || end > vma->vaddr_end) return -1;  // not within one VMA
    struct Burrow *v = vma->burrow;
    if (!v || v->magic != VMO_MAGIC)                return -1;
    if (v->type != BURROW_TYPE_ANON_LAZY)           return -1;

    // 1. Clear every leaf PTE in the range + broadcast TLBI -- BEFORE any page is
    //    freed to the buddy, so no stale PTE/TLB entry aliases a recycled page (the
    //    burrow_unmap / "MMU user-PTE clear + TLBI" discipline). Idempotent on
    //    never-faulted pages. asid arg vestigial (all-ASID tlbi). Held under vma_lock.
    (void)mmu_uninstall_user_range(p->as->pgtable_root, 0, vaddr, end);

    // 2. Free the resident slot pages + count them. The range maps to filepages
    //    slots; slot = (vma->burrow_offset + (page_va - vma->vaddr_start)) / PAGE_SIZE
    //    (burrow_offset is 0 for an attach VMA, but compute it generally). free_pages
    //    runs OUTSIDE v->lock (leaf-lock discipline -- it takes the buddy lock); grab
    //    + NULL the slot under v->lock (lock order vma_lock -> v->lock), free after.
    u32 freed = 0;
    for (u64 va = vaddr; va < end; va += PAGE_SIZE) {
        u64    burrow_off = vma->burrow_offset + (va - vma->vaddr_start);
        size_t slot       = burrow_off / PAGE_SIZE;
        struct page *pg = NULL;
        spin_lock(&v->lock);
        if (v->filepages && slot < v->page_count && v->filepages[slot]) {
            pg = v->filepages[slot];
            v->filepages[slot] = NULL;
        }
        spin_unlock(&v->lock);
        if (pg) {
            // LINEAGE L-4b: releasing a SLOT is not the same as freeing a PAGE
            // once the page can be COW-shared. The page goes back to the buddy
            // only when this was its last holder; `freed` keeps counting SLOTS,
            // because that is what the I-32 uncharge below is about -- this
            // address space stops mapping the page either way, so its RSS drops
            // whether or not a co-sharer keeps the page alive.
            if (cow_page_put(pg))
                free_pages(pg, 0);        // outside v->lock (leaf order)
            freed++;
        }
    }

    // 3. Uncharge the I-32 page_count for the released pages (the same per-page charge
    //    the fault arm made). Under vma_lock, so it pairs exactly with the charge. A
    //    later touch re-faults a fresh zero page + re-charges.
    if (freed)
        proc_page_uncharge(p, freed);

    return 0;
}

// burrow_lazy_resident_count: the count of resident (faulted-in, not-decommitted)
// pages in an ANON_LAZY Burrow -- the amount SYS_BURROW_DETACH uncharges (a lazy
// region charged page_count per FAULT, so a full detach uncharges only the resident
// pages). Read under v->lock (caller holds vma_lock; order vma_lock -> v->lock).
// NOT const-qualified: it TAKES v->lock (a logically-const observation that
// nonetheless mutates the lock word -- audit F3, dropping the const-cast).
u32 burrow_lazy_resident_count(struct Burrow *v) {
    if (!v || v->magic != VMO_MAGIC)        return 0;
    if (v->type != BURROW_TYPE_ANON_LAZY)   return 0;
    u32 n = 0;
    spin_lock(&v->lock);
    if (v->filepages) {
        for (size_t i = 0; i < v->page_count; i++)
            if (v->filepages[i]) n++;
    }
    spin_unlock(&v->lock);
    return n;
}

// =============================================================================
// Weft-2 / I-37: cross-Proc Burrow share (the per-flow dataplane ring).
// =============================================================================

// burrow_share_into: map an EXISTING Burrow into a SECOND Proc's address
// space, establishing the cross-Proc share. The tree's FIRST path that makes
// one Burrow reachable from two Procs concurrently -- the substrate the Weft
// capability dataplane (docs/NET-THROUGHPUT.md §6; ARCH §28 I-37) builds the
// per-flow guest<->netd shared ring on (Weft-6 wires the EL0 delivery).
//
// A share maps the WHOLE Burrow (the entire per-flow ring / weave): the caller
// passes NO length (unlike burrow_map, whose sub-range callers drive an
// explicit length) because a share is always whole-Burrow. length comes from
// v->size via burrow_get_size (magic-checked: 0 on a NULL/corrupted v ->
// reject).
//
// Admission (G-2 -- the owed I-5 analysis, TAPESTRY.md §18.1 + §18.12 R2-F1):
// ANON (a dataplane ring), or a KERNEL-MINTED device-passive DMA WEAVE (a
// framebuffer-class region whose KObj_DMA carries the create-immutable `weave`
// bit, set only by SYS_DMA_CREATE_WEAVE / kobj_dma_create_weave). A plain DMA
// region -- virtqueue, descriptor table, bounce buffer: the device-COMMAND
// class -- and MMIO stay structurally unshareable: no creator-asserted flag
// admits them, only the allocation-time kernel mint, so sharing a region the
// device INTERPRETS is impossible by construction (MMIO parity). The weave bit
// conveys no hardware authority of its own: the region is pinned Normal-WB RAM
// the device only DMA-reads (pixels); the client's PTEs are the same cacheable
// attrs an ANON share installs (the fault arm's BURROW_TYPE_DMA case -- never
// Device-nGnRnE), and W^X holds (the share prot is RW; vma_alloc rejects X).
// The #847 dual-refcount proof below is TYPE-INDEPENDENT with one addition for
// DMA: the freed Burrow's kobj_dma_unref is the SECOND refcount domain -- the
// weave's page chunk lives on the KObj_DMA, which the Burrow's own ref keeps
// alive while EITHER Proc maps, so the client's mapping transitively pins the
// pixels across the sharer's own teardown (the R2-F1 second-domain fold).
//
// Composes burrow_map, which is ALREADY Proc-agnostic (it takes any `p`) and
// #847-SMP-safe (vma_alloc -> burrow_acquire_mapping bumps mapping_count under
// the per-Burrow v->lock). The ONLY new property Weft-2 establishes is that
// the two refcount holders now sit in DIFFERENT Procs:
//
//   #847 cross-Proc proof. The dual-refcount lock (struct Burrow::lock) is
//   per-BURROW, not per-Proc, so it serializes ref/unref/{acquire,release}_
//   mapping identically whether the callers are two Threads of one Proc (the
//   #847 case) or two different Procs (Weft-2). Proc A's burrow_release_mapping
//   and Proc B's burrow_unref race the `handle_count==0 && mapping_count==0`
//   free decision under v->lock; exactly one sees the 0,0 edge and frees. The
//   Burrow lives while EITHER Proc holds a mapping OR any handle is open, and
//   frees only when ALL drop, in ANY interleaving (the spec's
//   weft.tla::ShareBoundedByFlow: free iff both share refs gone).
//
//   Lock order (acyclic across Procs). burrow_map requires the CALLER's
//   dst->vma_lock, then vma_alloc takes v->lock: dst->vma_lock -> v->lock.
//   Proc A holds A->vma_lock -> v->lock; Proc B holds B->vma_lock -> v->lock.
//   The vma_locks are distinct per-Proc leaf-ward locks; v->lock is the single
//   inner lock both reach -- no cycle, no cross-Proc deadlock.
//
// PRECONDITIONS:
//   - The caller MUST hold dst->vma_lock (this installs a VMA in dst -- the
//     #713 / RW-1 C-F2 vmas-mutator discipline, like burrow_map).
//   - The caller MUST guarantee `v` stays alive across the call (a held
//     handle/mapping ref, OR a higher-level lock excluding a concurrent
//     teardown to {h:0,m:0}). burrow_share_into takes the mapping ref that
//     keeps v alive THEREAFTER, but cannot manufacture liveness for the
//     instant before vma_alloc bumps the count -- if both counts were already
//     0, burrow_acquire_mapping extincts (the resurrection guard). In the
//     grant-is-the-share flow the kernel resolves v from a live flow and the
//     data-fid open is serialized against flow teardown (the Weft-6 caller's
//     obligation).
//
// Returns 0 on success; -1 on NULL inputs, a corrupted/inadmissible-type/
// zero-size v, an over-budget client (proc_shared_map_charge -- R2-F3), W+X
// prot (rejected by vma_alloc), VMA overlap, or VMA SLUB OOM. On failure no
// mapping is installed, no budget is charged, and v's refcount is unchanged.
int burrow_share_into(struct Proc *dst, struct Burrow *v, u64 vaddr, u32 prot) {
    if (!dst || !v) return -1;

    // burrow_get_size magic-checks v (returns 0 on NULL/corrupted) -- a freed
    // or wild v is rejected here, before any mapping ref is taken. A non-zero
    // return proves v->magic == VMO_MAGIC, so the immutable v->type read below
    // is on a valid Burrow.
    size_t length = burrow_get_size(v);
    if (length == 0) return -1;

    // The G-2 admission gate (see the header comment): ANON, or the
    // kernel-minted device-passive DMA weave. Types + the weave bit are
    // create-immutable; lock-free reads are coherent.
    bool weave = (v->type == BURROW_TYPE_DMA &&
                  v->kobj_dma != NULL && v->kobj_dma->weave);
    if (v->type != BURROW_TYPE_ANON && !weave) return -1;

    // R2-F3: charge the CLIENT's shared-in budget (the I-32 fifth axis) BEFORE
    // mapping. The pages are the SHARER's commit (netd's page_count /
    // tapestryd's DMA pool) -- dst->page_count is deliberately NOT charged;
    // this axis is what bounds a client's cross-Proc pin, including across a
    // sharer crash (the orphaned-weave leg). Exact under dst->vma_lock (the
    // precondition). v->size is always a page multiple, and bounded well
    // under u32 pages by BURROW_ATTACH_MAX / KOBJ_DMA_WEAVE_MAX_SIZE.
    u32 npages = (u32)(length / PAGE_SIZE);
    if (!proc_shared_map_charge(dst, npages)) return -1;

    // burrow_map takes the mapping_count ref (vma_alloc -> burrow_acquire_
    // mapping) that keeps v alive for dst from here on -- the cross-Proc #847
    // ref. W^X (prot) + VA bounds + overlap are validated inside.
    if (burrow_map(dst, v, vaddr, length, prot) != 0) {
        proc_shared_map_uncharge(dst, npages);
        return -1;
    }

    // Flag the freshly-installed VMA so its teardown (burrow_unmap /
    // vma_drain) uncharges exactly once -- the charge/uncharge pairing rides
    // the VMA, covering every removal path (explicit detach, the weave-fid
    // clunk unmap, exit drain). We still hold dst->vma_lock, and burrow_map
    // just inserted the VMA at exactly vaddr, so the lookup cannot miss; a
    // miss here is structural corruption of the vmas list under the held lock.
    struct Vma *vma = vma_lookup(dst, vaddr);
    if (!vma) extinction("burrow_share_into: installed VMA vanished under vma_lock");
    vma->flags |= VMA_FLAG_SHARED_IN;

    // #131: mark the region shared-out. This is what lets the SHARER's own
    // detach tell "the survivor is somebody else, so I can no longer reach
    // these pages -- release my charge" from "my own other claim survives
    // (a Loom registered buffer), so keep it until that claim drops". Set
    // AFTER the map succeeds: a failed share leaves no second Proc, so it
    // must leave no reason to release either. Monotonic -- never cleared,
    // because a sharer that walked away does not get the charge back if the
    // consumer later unmaps.
    spin_lock(&v->lock);
    v->shared_out = true;
    spin_unlock(&v->lock);
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

// #130-R2 F3/F4: these read counts that EVERY mutator writes under v->lock
// (burrow_ref / _unref / _acquire_mapping / _release_mapping), and they do NOT
// take that lock. I asserted in the #130 R2 prompt that they did; they never
// have. The reviewer corrected the premise it was handed, which is the reason
// to write the truth down here rather than leave it in a round's transcript.
//
// Not taking the lock is deliberate: a caller may already hold it (v->lock is a
// leaf and these are diagnostics), so acquiring here would be a self-deadlock
// waiting for a caller to appear. The load is ACQUIRE instead, so the value is
// at least a coherent snapshot ordered against the releasing spin_unlock rather
// than a plain read the compiler may cache or sink.
//
// What that buys, on its own, is exactly one thing: the returned number was
// true at some instant. By itself it is NOT a basis for a lifecycle decision --
// by the time a caller branches, a peer may have dropped the last ref. "Did
// this drop free the pages?" is answered by burrow_unref_freed /
// burrow_release_mapping_freed / burrow_unmap_reporting, which decide it under
// the lock and REPORT it (#130); asking these counts "may I act?" without more
// is the predicted-event bug that #130 and #131 both were.
//
// "Without more" is the load-bearing part, and the first draft of this comment
// got it wrong by stating a flat prohibition. A caller MAY branch on these when
// it holds an EXTERNAL lock that makes the value stable, plus a ref-discipline
// argument for why no untracked party can bump it. image.c is the one
// production caller and it has both: under g_image_lock, handle_count == 1
// means the cache's own ref is the only one, because an in-flight mapper would
// hold >= 2 (it takes its ref before mapping), so {1, 0} really is an idle
// image and evicting it is safe -- the I-36 eviction-safety proof, audited.
// State the precondition, not the prohibition: a flat "never" here would send
// the next reader to "fix" correct code.
int burrow_handle_count(const struct Burrow *v) {
    if (!v) return 0;
    if (v->magic != VMO_MAGIC) return 0;
    return __atomic_load_n(&v->handle_count, __ATOMIC_ACQUIRE);
}

int burrow_mapping_count(const struct Burrow *v) {
    if (!v) return 0;
    if (v->magic != VMO_MAGIC) return 0;
    return __atomic_load_n(&v->mapping_count, __ATOMIC_ACQUIRE);
}

u64 burrow_total_created(void)    { return g_vmo_created; }
u64 burrow_total_destroyed(void)  { return g_vmo_destroyed; }

#ifdef KERNEL_TESTS
// REVENANT R-1 test hooks — let test_burrow.c exercise the FILE-Burrow
// resident-page free path before the R-2 fault arm exists to populate slots.
size_t burrow_file_page_count_for_test(const struct Burrow *v) {
    if (!v || v->magic != VMO_MAGIC)
        extinction("burrow_file_page_count_for_test: bad Burrow");
    if (v->type != BURROW_TYPE_FILE)
        extinction("burrow_file_page_count_for_test: not a FILE Burrow");
    return v->page_count;
}

void burrow_file_install_page_for_test(struct Burrow *v, size_t idx) {
    if (!v || v->magic != VMO_MAGIC)
        extinction("burrow_file_install_page_for_test: bad Burrow");
    if (v->type != BURROW_TYPE_FILE)
        extinction("burrow_file_install_page_for_test: not a FILE Burrow");
    if (!v->filepages || idx >= v->page_count)
        extinction("burrow_file_install_page_for_test: idx out of range");
    if (v->filepages[idx])
        extinction("burrow_file_install_page_for_test: slot already resident");
    struct page *p = alloc_pages(0, KP_ZERO);
    if (!p)
        extinction("burrow_file_install_page_for_test: alloc_pages OOM");
    v->filepages[idx] = p;
}

struct page *burrow_file_slot_for_test(const struct Burrow *v, size_t idx) {
    if (!v || v->magic != VMO_MAGIC)
        extinction("burrow_file_slot_for_test: bad Burrow");
    if (v->type != BURROW_TYPE_FILE)
        extinction("burrow_file_slot_for_test: not a FILE Burrow");
    if (!v->filepages || idx >= v->page_count) return NULL;
    return v->filepages[idx];
}

struct page *burrow_lazy_slot_for_test(const struct Burrow *v, size_t idx) {
    if (!v || v->magic != VMO_MAGIC)
        extinction("burrow_lazy_slot_for_test: bad Burrow");
    if (v->type != BURROW_TYPE_ANON_LAZY)
        extinction("burrow_lazy_slot_for_test: not an ANON_LAZY Burrow");
    if (!v->filepages || idx >= v->page_count) return NULL;
    return v->filepages[idx];
}
#endif
