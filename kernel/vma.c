// Per-Proc VMA list — implementation (P3-Da / P3-Db).
//
// Sorted doubly-linked list of VMAs anchored at struct Proc.vmas.
// O(N) operations at v1.0; RB-tree is a Phase 5+ optimization.
//
// BURROW refcounting: vma_alloc takes a burrow_acquire_mapping (mapping_count
// ++); vma_free takes a burrow_release_mapping (mapping_count--). The dual-
// refcount lifecycle in burrow.c (handle_count + mapping_count) ensures the
// BURROW survives until both reach zero — see specs/burrow.tla.
//
// (Pre-P3-Db, the refcount-only ops were named burrow_map / burrow_unmap.
// They were renamed when the public burrow_map(Proc*, ...) entry point
// arrived.)
//
// Per ARCHITECTURE.md §16.

#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/vma.h>
#include <thylacine/burrow.h>
#include <thylacine/errno.h>     // B-1a: vma_reprotect_* report -T_E_*

#include "../arch/arm64/mmu.h"     // B-1a': the range detach's own PTE uninstall
#include "../mm/slub.h"

// =============================================================================
// State
// =============================================================================

static struct kmem_cache *g_vma_cache;
static u64 g_vma_allocated;
static u64 g_vma_freed;

// =============================================================================
// init
// =============================================================================

void vma_init(void) {
    if (g_vma_cache) extinction("vma_init called twice");

    g_vma_cache = kmem_cache_create("vma",
                                    sizeof(struct Vma),
                                    8,
                                    KMEM_CACHE_PANIC_ON_FAIL);
    if (!g_vma_cache) extinction("kmem_cache_create(vma) returned NULL");
}

// =============================================================================
// alloc / free
// =============================================================================

struct Vma *vma_alloc(u64 vaddr_start, u64 vaddr_end, u32 prot,
                     struct Burrow *burrow, u64 burrow_offset) {
    if (!g_vma_cache) extinction("vma_alloc before vma_init");
    if (!burrow)         return NULL;

    if (vaddr_start >= vaddr_end) return NULL;
    if (vaddr_start & (PAGE_SIZE - 1)) return NULL;
    if (vaddr_end   & (PAGE_SIZE - 1)) return NULL;

    // W^X policy: reject W+X at the VMA layer too. This mirrors the
    // PTE-construction-time invariant + the ELF loader's rejection.
    // ARCH §28 I-12.
    if ((prot & VMA_PROT_WRITE) && (prot & VMA_PROT_EXEC)) return NULL;

    // RW-1 C-F3: reject write-without-read. AArch64 has no write-only AP, so a
    // W-only prot would map RW (readable) -- a rights/PTE mismatch the MMIO/DMA
    // syscalls already guard. Reject it here so the VMA prot matches the PTE.
    if ((prot & VMA_PROT_WRITE) && !(prot & VMA_PROT_READ)) return NULL;

    // B-1a' audit F3: the mapping must lie inside the Burrow. A VMA past the
    // Burrow's end would name slots its pagemap does not have, and the range
    // release walks the mapping's own slot bound -- refused here, at the one
    // constructor, rather than defended in every walker. Written so neither
    // term can overflow.
    u64 span = (u64)burrow->page_count << PAGE_SHIFT;
    if (burrow_offset > span)                          return NULL;
    if (vaddr_end - vaddr_start > span - burrow_offset) return NULL;

    struct Vma *v = kmem_cache_alloc(g_vma_cache, KP_ZERO);
    if (!v) return NULL;

    v->magic       = VMA_MAGIC;
    v->vaddr_start = vaddr_start;
    v->vaddr_end   = vaddr_end;
    v->prot        = prot;
    v->burrow         = burrow;
    v->burrow_offset  = burrow_offset;
    vma_set_prot_max(v, prot);   // the ceiling is the mint (vma.h: why not RW)
    // next/prev left NULL via KP_ZERO; vma_insert wires them.

    // P2-Fd contract: burrow_acquire_mapping increments mapping_count. The
    // VMA's existence in a Proc's list is an active mapping; we count
    // it against the BURROW's lifecycle. burrow_release_mapping'd when
    // vma_free runs. burrow_acquire_mapping is `void` — it cannot fail at
    // v1.0 (mapping_count saturates structurally per ARCH §28 I-7; if a
    // future overflow check is added it'd extinct internally).
    burrow_acquire_mapping(burrow);

    __atomic_fetch_add(&g_vma_allocated, 1u, __ATOMIC_RELAXED);
    return v;
}

struct Vma *vma_alloc_guard(u64 vaddr_start, u64 vaddr_end) {
    if (!g_vma_cache) extinction("vma_alloc_guard before vma_init");

    if (vaddr_start >= vaddr_end)       return NULL;
    if (vaddr_start & (PAGE_SIZE - 1))  return NULL;
    if (vaddr_end   & (PAGE_SIZE - 1))  return NULL;

    struct Vma *v = kmem_cache_alloc(g_vma_cache, KP_ZERO);
    if (!v) return NULL;

    v->magic         = VMA_MAGIC;
    v->vaddr_start   = vaddr_start;
    v->vaddr_end     = vaddr_end;
    v->prot          = 0;       // no R/W/X — every fault into it is rejected
    v->burrow        = NULL;    // no backing object: a guard owns no pages
    v->burrow_offset = 0;
    // next/prev left NULL via KP_ZERO; vma_insert wires them.

    // No burrow_acquire_mapping: a guard VMA has no BURROW and thus does
    // not participate in the BURROW dual-refcount lifecycle (specs/
    // burrow.tla). vma_free's burrow_release_mapping is guarded by
    // v->burrow != NULL, so the alloc/free pair stays balanced.

    __atomic_fetch_add(&g_vma_allocated, 1u, __ATOMIC_RELAXED);
    return v;
}

bool vma_free_freed(struct Vma *v) {
    if (!v)                     extinction("vma_free(NULL)");
    if (v->magic != VMA_MAGIC)  extinction("vma_free of corrupted/already-freed Vma");
    if (v->next || v->prev)     extinction("vma_free of Vma still in a list");

    // Release the BURROW mapping ref. burrow_release_mapping may free the BURROW
    // if both handle_count and mapping_count reach zero (see
    // specs/burrow.tla).
    //
    // #130: report whether THIS release was the one that freed the pages. I-32
    // charges occupancy, so the uncharge belongs to the drop that ends the
    // occupancy -- and which drop that is cannot be known before the fact: a
    // Loom's registered buffer, a Loom's ring, and a Weft share all hold a
    // handle_count ref that can outlive the VMA, so tearing the VMA down is not
    // the same event as freeing the pages. The caller (which knows what it
    // charged) pairs its uncharge to this bool.
    bool freed = false;
    if (v->burrow) {
        freed = burrow_release_mapping_freed(v->burrow);
        v->burrow = NULL;
    }

    kmem_cache_free(g_vma_cache, v);
    __atomic_fetch_add(&g_vma_freed, 1u, __ATOMIC_RELAXED);
    return freed;
}

void vma_free(struct Vma *v) { (void)vma_free_freed(v); }

// D-3c F1: the deferred twin of vma_free_freed. Drops the mapping ref via
// burrow_release_mapping_deferred (which does NOT free), frees the Vma struct,
// and returns the Burrow that still owes a free (or NULL). The caller pushes it
// onto a local stack and frees it with burrow_free_deferred AFTER dropping
// as->lock -- the FILE arm's spoor_clunk may sleep, and a sleeping free under a
// spinlock is the lock-across-sleep extinction. *out_freed reports the same
// event vma_free_freed's bool does, for the I-32 uncharge (which stays under
// the lock -- only the physical free moves out).
struct Burrow *vma_free_deferred(struct Vma *v, bool *out_freed) {
    if (out_freed) *out_freed = false;
    if (!v)                     extinction("vma_free(NULL)");
    if (v->magic != VMA_MAGIC)  extinction("vma_free of corrupted/already-freed Vma");
    if (v->next || v->prev)     extinction("vma_free of Vma still in a list");

    struct Burrow *to_free = NULL;
    if (v->burrow) {
        to_free = burrow_release_mapping_deferred(v->burrow);
        if (out_freed) *out_freed = (to_free != NULL);
        v->burrow = NULL;
    }

    kmem_cache_free(g_vma_cache, v);
    __atomic_fetch_add(&g_vma_freed, 1u, __ATOMIC_RELAXED);
    return to_free;
}

// =============================================================================
// Sorted-list operations
// =============================================================================

// True iff [a, b) overlaps [c, d). Both half-open intervals.
static inline bool ranges_overlap(u64 a, u64 b, u64 c, u64 d) {
    return a < d && c < b;
}

int vma_insert(struct Proc *p, struct Vma *v) {
    if (!p) extinction("vma_insert(NULL)");
    return vma_insert_in(p->as, proc_resource_exempt(p), v);
}

int vma_insert_in(struct AddrSpace *as, bool exempt, struct Vma *v) {
    if (!as || !v)               extinction("vma_insert_in(NULL)");
    if (v->magic != VMA_MAGIC)   extinction("vma_insert of corrupted Vma");
    if (v->next || v->prev)      extinction("vma_insert of already-linked Vma");

    // Walk the sorted list to find:
    //   - The insertion point (last node with start < v->start).
    //   - Any overlap with existing VMAs.
    struct Vma *prev = NULL;
    struct Vma *cur  = as->vmas;
    while (cur) {
        if (cur->magic != VMA_MAGIC) extinction("vma_insert: corrupted list entry");
        if (ranges_overlap(v->vaddr_start, v->vaddr_end,
                           cur->vaddr_start, cur->vaddr_end)) {
            return -1;            // overlap rejected
        }
        if (cur->vaddr_start >= v->vaddr_end) break;     // first node past v
        prev = cur;
        cur  = cur->next;
    }

    // I-32 FOURTH axis (overcommit, ARCH §6.5): bound live VMAs — the DoS a free
    // SYS_BURROW_ATTACH_LAZY reservation (uncharged at attach) would otherwise open.
    // Checked AFTER the overlap walk (so a rejected overlap doesn't consume the
    // budget) and BEFORE the list mutation (so a cap-hit installs nothing). A non-TCB
    // address space at PROC_VMA_MAX is rejected here as -T_E_NOMEM (an overlap is
    // -1: the cap is a resource refusal, reportable as one -- B-1a' audit F18; the
    // caller vma_frees the rejected Vma either way). The charge requires as->lock — every
    // vma_insert caller holds it (attach / share under vma_lock; the exec load path
    // builds a detached address space no other thread can reach). Paired by
    // addrspace_uncharge_vma in vma_remove_in. Charges nothing on failure, so no
    // rollback is needed on the rejected path.
    if (!addrspace_charge_vma(as, exempt)) return -(int)T_E_NOMEM;

    // Insert v between prev and cur.
    v->prev = prev;
    v->next = cur;
    if (prev) prev->next = v;
    else      as->vmas   = v;
    if (cur)  cur->prev  = v;

    return 0;
}

void vma_remove(struct Proc *p, struct Vma *v) {
    if (!p) extinction("vma_remove(NULL)");
    vma_remove_in(p->as, v);
}

void vma_remove_in(struct AddrSpace *as, struct Vma *v) {
    if (!as || !v)               extinction("vma_remove_in(NULL)");
    if (v->magic != VMA_MAGIC)   extinction("vma_remove of corrupted Vma");

    if (v->prev) v->prev->next = v->next;
    else         as->vmas      = v->next;
    if (v->next) v->next->prev = v->prev;

    v->next = NULL;
    v->prev = NULL;

    // I-32: a removed VMA frees its slab slot -> uncharge the live-VMA count (pairs
    // with the charge in vma_insert_in). Under as->lock (every vma_remove caller
    // holds it: detach / share teardown; vma_drain at proc_free is single-threaded).
    // Clamp-safe.
    addrspace_uncharge_vma(as);
}

struct Vma *vma_lookup(struct Proc *p, u64 vaddr) {
    if (!p) return NULL;
    return vma_lookup_in(p->as, vaddr);
}

// =============================================================================
// B-1a': the range detach, and the MAP_FIXED replace over it (vma.h has the
// contracts; specs/capacity.tla the accounting law they keep).
// =============================================================================

int vma_detach_range_in(struct AddrSpace *as, bool exempt, struct Proc *payer,
                        u64 vaddr, u64 length, u32 extra_vmas,
                        struct Burrow **out_dead) {
    // Mandatory: a whole mapping's drop may be a FILE Burrow's last ref, whose
    // free reaches a possibly-sleeping spoor_clunk, and this runs under
    // as->lock (the D-3c F1 rule). Fail loud rather than free inline.
    if (!out_dead) extinction("vma_detach_range_in without out_dead (would free under as->lock)");
    *out_dead = NULL;
    if (!as)                                            return -(int)T_E_INVAL;
    if (length == 0)                                    return -(int)T_E_INVAL;
    if (vaddr  & (PAGE_SIZE - 1))                       return -(int)T_E_INVAL;
    if (length & (PAGE_SIZE - 1))                       return -(int)T_E_INVAL;
    u64 end = vaddr + length;
    if (end < vaddr)                                    return -(int)T_E_INVAL;
    if (end > USER_VA_TOP)                              return -(int)T_E_INVAL;

    // PHASE 1 -- decide every refusal, mutate nothing. One scan from the head,
    // then successors: the list is sorted (B-1a audit F2).
    struct Vma *first = vma_next_overlap_in(as, vaddr, end);
    if (!first)                                         return 0;   // nothing mapped
    struct Vma *last = first;
    u32 whole_n = 0;
    for (struct Vma *v = first; v && v->vaddr_start < end; v = v->next) {
        if (v->magic != VMA_MAGIC)
            extinction("vma_detach_range: corrupted list entry");
        bool whole = v->vaddr_start >= vaddr && v->vaddr_end <= end;
        if (v->burrow) {
            if (v->burrow->magic != VMO_MAGIC)          return -(int)T_E_ACCES;
            // I-42: a CODE region is a PAIR of aliases over one charge, and
            // this path has no concept of the pair. Detaching one alias would
            // refund the charge once per alias (a bound a CAP_JIT holder can
            // drive to zero is not a bound) and orphan its peer (SYS_JIT_DESTROY
            // then refuses it). The JIT syscalls own that lifetime.
            if (v->burrow->type == BURROW_TYPE_CODE)    return -(int)T_E_ACCES;
        }
        if ((v->flags & VMA_FLAG_SHARED_IN) && !whole)  return -(int)T_E_ACCES;
        if (whole) whole_n++;
        last = v;
    }
    // Only a range strictly inside ONE mapping adds a mapping (its tail); every
    // other shape trims or removes. The count is stable under as->lock, so the
    // inserts that follow cannot fail once the headroom -- for the piece AND
    // for whatever the caller inserts next -- clears against the count AFTER
    // the removals.
    bool middle = (first == last) &&
                  first->vaddr_start < vaddr && first->vaddr_end > end;
    if (!exempt) {
        u32 cnt   = __atomic_load_n(&as->vma_count, __ATOMIC_RELAXED);
        u32 after = cnt - whole_n + (middle ? 1u : 0u) + extra_vmas;
        if (after > PROC_VMA_MAX)                       return -(int)T_E_NOMEM;
    }
    // The tail piece re-derives its offset from the SAME (burrow, offset)
    // relation its parent has, so every surviving VA keeps its byte identity;
    // it carries the parent's flags whole -- the ceiling and the COW routing
    // bit (the per-page share counts are per page, so a cut is sound for a
    // forked mapping). A guard's tail is a guard.
    struct Vma *piece = NULL;
    if (middle) {
        piece = first->burrow
              ? vma_alloc(end, first->vaddr_end, first->prot, first->burrow,
                          first->burrow_offset + (end - first->vaddr_start))
              : vma_alloc_guard(end, first->vaddr_end);
        if (!piece)                                     return -(int)T_E_NOMEM;
        piece->flags = first->flags;
    }

    // PHASE 2 -- the range's leaf PTEs go BEFORE any page is freed or any
    // mapping changes (the burrow_unmap discipline: a stale PTE or TLB entry
    // would alias a recycled page). Absent subtrees are skipped; the asid arg
    // is vestigial (all-ASID tlbi vaae1is). Mapping by mapping, so a FILE
    // mapping's leaves refund this space's holder charge (audit F8).
    (void)vma_uninstall_range_in(as, vaddr, end);

    // PHASE 3 -- release, then re-shape. The successor is read before a whole
    // mapping is freed; the piece is consumed by the one shape that needs it.
    struct Burrow *dead = NULL;
    for (struct Vma *v = first, *nx = NULL; v && v->vaddr_start < end; v = nx) {
        nx = v->next;
        u64 lo = v->vaddr_start > vaddr ? v->vaddr_start : vaddr;
        u64 hi = v->vaddr_end   < end   ? v->vaddr_end   : end;
        struct Burrow *b = v->burrow;
        bool shared_in = (v->flags & VMA_FLAG_SHARED_IN) != 0;

        // The release FIRST (capacity.tla, Detach): the overlap's resident
        // slots are freed and uncharged while the mapping still names them.
        // burrow_free_internal cannot refund a page, so a slot unmapped while
        // resident would be charged for the address space's life (NoOrphan).
        if (b && !shared_in && b->type == BURROW_TYPE_ANON_LAZY)
            (void)burrow_release_lazy_range_in(as, v, lo, hi);

        if (lo == v->vaddr_start && hi == v->vaddr_end) {
            // WHOLE: the mapping goes. A shared-in span's budget charge goes
            // with it (the burrow_share_into pairing, exact under this lock).
            if (shared_in)
                addrspace_uncharge_shared_map(as, (u32)((hi - lo) / PAGE_SIZE));
            // The eager-ANON refund (#130/#131): the charge RECORD says who
            // paid, never the region's shape. Claimed BEFORE the drop (a
            // freeing drop takes the record with it) and refunded iff the drop
            // actually freed the pages -- a Loom ring, a registered buffer and
            // a Weft share each hold a handle_count ref that can outlive the
            // mapping -- or the region survives only in ANOTHER Proc, which
            // this one can no longer reach and whose eventual last drop has no
            // way to name the payer. Alive on one of this Proc's OWN claims,
            // the claim is put back for that claim's drop to settle.
            bool shared_out = false;
            u32  paid       = 0;
            if (payer && b && !shared_in && b->type == BURROW_TYPE_ANON) {
                shared_out = burrow_is_shared_out(b);
                paid       = burrow_charge_claim(b, payer);
            }
            vma_remove_in(as, v);
            bool freed = false;
            struct Burrow *tf = vma_free_deferred(v, &freed);
            if (paid) {
                if (freed || shared_out) addrspace_uncharge_pages(as, paid);
                else                     burrow_charge_restore(b, payer, paid);
            }
            if (tf) { tf->deferred_free_next = dead; dead = tf; }
        } else if (lo == v->vaddr_start) {
            // The range covers the mapping's head: the survivor is its tail.
            // start and offset move by the SAME delta (identity preserved);
            // still sorted, since the predecessor ends at or below the old
            // start.
            v->burrow_offset += hi - v->vaddr_start;
            v->vaddr_start    = hi;
        } else if (hi == v->vaddr_end) {
            // The range covers the mapping's tail: the survivor is its head
            // (same start, same offset).
            v->vaddr_end = lo;
        } else {
            // The range is strictly inside: the head survives in place and the
            // pre-allocated tail lands in the space the shrink just vacated.
            // Infallible by construction (no overlap; headroom checked), so a
            // refusal here means the count's bookkeeping lied -- fail loud
            // rather than leave the tail's bytes unreachable.
            v->vaddr_end = lo;
            if (vma_insert_in(as, exempt, piece) != 0)
                extinction("vma_detach_range: the tail piece was refused after the headroom check");
            piece = NULL;
        }
    }
    *out_dead = dead;
    return 0;
}

// DISTRO D-3b as rebuilt at B-1a': the range detach, then the insert. See vma.h.
int vma_replace_range_in(struct AddrSpace *as, bool exempt, struct Proc *payer,
                         u64 vaddr, u64 length,
                         struct Burrow *nb, u32 prot, u64 nb_offset,
                         struct Burrow **out_free) {
    // F7 (re-audit round 3): out_free is MANDATORY. The detach hands back dead
    // Burrows that vma_free_deferred does NOT free, so a NULL would LEAK them
    // (the slab slot, the pagemap, a FILE Burrow's pinned Spoor). Fail loud.
    if (!out_free) extinction("vma_replace_range_in without out_free (would leak the replaced Burrows)");
    *out_free = NULL;
    if (!as || !nb)                       return -(int)T_E_INVAL;
    if (length == 0)                      return -(int)T_E_INVAL;
    if (vaddr  & (PAGE_SIZE - 1))         return -(int)T_E_INVAL;
    if (length & (PAGE_SIZE - 1))         return -(int)T_E_INVAL;
    u64 end = vaddr + length;
    if (end < vaddr)                      return -(int)T_E_INVAL;

    // The new mapping FIRST, so a slab shortfall changes nothing; it holds its
    // mapping ref on `nb` across the detach.
    struct Vma *nv = vma_alloc(vaddr, end, prot, nb, nb_offset);
    if (!nv)                              return -(int)T_E_NOMEM;

    struct Burrow *dead = NULL;
    int rc = vma_detach_range_in(as, exempt, payer, vaddr, length, 1, &dead);
    if (rc != 0) {
        // Refused: the detach changed nothing. The new mapping's ref drops the
        // deferred way -- the caller normally still holds `nb`'s construction
        // handle, but this layer does not assume it.
        struct Burrow *tf = vma_free_deferred(nv, NULL);
        if (tf) { tf->deferred_free_next = dead; dead = tf; }
        *out_free = dead;
        return rc;
    }
    // Infallible: the range was just vacated (no overlap) and the detach
    // cleared the headroom for this insert (extra_vmas = 1).
    if (vma_insert_in(as, exempt, nv) != 0)
        extinction("vma_replace_range_in: insert refused into the range the detach vacated");
    *out_free = dead;
    return 0;
}

struct Vma *vma_lookup_in(struct AddrSpace *as, u64 vaddr) {
    if (!as) return NULL;

    for (struct Vma *cur = as->vmas; cur; cur = cur->next) {
        if (cur->magic != VMA_MAGIC) extinction("vma_lookup: corrupted list entry");
        if (vaddr >= cur->vaddr_start && vaddr < cur->vaddr_end) return cur;
        // Sorted-list optimization: if cur->vaddr_start > vaddr, every
        // subsequent node has even larger start; lookup miss.
        if (cur->vaddr_start > vaddr) return NULL;
    }
    return NULL;
}

// #199: lowest-addressed VMA overlapping [lo, hi). Caller holds as->lock. The
// list layout stays this file's business -- range consumers iterate through
// this rather than walking as->vmas themselves.
// B-1a audit F2: nodes visited by vma_next_overlap_in. A diagnostic, read by
// the test that pins the range walks to ONE scan from the head -- a re-scan
// per mapping was O(k x N) under a non-preemptible lock, four passes deep.
static u64 g_vma_scan_steps;

struct Vma *vma_next_overlap_in(struct AddrSpace *as, u64 lo, u64 hi) {
    if (!as || lo >= hi) return NULL;

    for (struct Vma *cur = as->vmas; cur; cur = cur->next) {
        __atomic_add_fetch(&g_vma_scan_steps, 1, __ATOMIC_RELAXED);
        if (cur->magic != VMA_MAGIC)
            extinction("vma_next_overlap: corrupted list entry");
        if (cur->vaddr_start >= hi) return NULL;   // sorted: nothing later overlaps
        if (cur->vaddr_end > lo) return cur;
    }
    return NULL;
}

bool vma_range_is_mapped_in(struct AddrSpace *as, u64 lo, u64 hi) {
    if (!as || lo >= hi) return false;
    u64 cur = lo;
    for (struct Vma *v = vma_next_overlap_in(as, lo, hi);
         v && v->vaddr_start < hi; v = v->next) {
        if (v->vaddr_start > cur) return false;      // a gap before this one
        cur = v->vaddr_end;
        if (cur >= hi) return true;
    }
    return false;                                     // a gap at the tail
}

long vma_uninstall_range_in(struct AddrSpace *as, u64 lo, u64 hi) {
    long total = 0;
    for (struct Vma *v = vma_next_overlap_in(as, lo, hi);
         v && v->vaddr_start < hi; v = v->next) {
        u64 a = v->vaddr_start > lo ? v->vaddr_start : lo;
        u64 b = v->vaddr_end   < hi ? v->vaddr_end   : hi;
        if (a >= b) continue;
        long n = mmu_uninstall_user_range(as, a, b);
        if (n <= 0) continue;
        if (v->burrow && v->burrow->type == BURROW_TYPE_FILE)
            addrspace_uncharge_file(as, (u32)n);
        total += n;
    }
    return total;
}

// P6-pouch-mem: first-fit free-range finder for SYS_BURROW_ATTACH. The
// VMA list is sorted by vaddr_start ascending, so a single forward pass
// — advancing a candidate base past every VMA that blocks it — finds
// the lowest free gap of `length` bytes in [window_start, window_end).
int vma_find_gap(struct Proc *p, u64 length,
                 u64 window_start, u64 window_end, u64 *out_vaddr) {
    return vma_find_gap_aligned(p, length, 0, window_start, window_end, out_vaddr);
}

// Round `va` up to `align` (a power of two). Returns 0 on wrap, which no
// caller can confuse with a real base: the window starts above 0.
static u64 round_up_align(u64 va, u64 align) {
    u64 r = (va + (align - 1)) & ~(align - 1);
    return (r < va) ? 0 : r;
}

int vma_find_gap_aligned(struct Proc *p, u64 length, u64 align,
                         u64 window_start, u64 window_end, u64 *out_vaddr) {
    if (!p || !out_vaddr)                          return -1;
    if (length == 0)                               return -1;
    if (length        & (PAGE_SIZE - 1))           return -1;
    if (window_start  & (PAGE_SIZE - 1))           return -1;
    if (window_end    & (PAGE_SIZE - 1))           return -1;
    if (window_start > window_end)                 return -1;
    if (window_end - window_start < length)        return -1;
    if (align == 0) align = PAGE_SIZE;
    if (align & (align - 1))                       return -1;   // not a power of two
    if (align < PAGE_SIZE)                         return -1;

    // `cand` is the lowest VA not yet ruled out. Every comparison uses
    // subtraction guarded by an ordering check, so no `cand + length`
    // sum is ever formed — overflow-free for any window in the 2^47
    // user-VA space. B-1a: the candidate is rounded up to `align` at the
    // start and after every jump; a VMA that begins below a rounded
    // candidate but ends above it still overlaps it and still bounds it.
    u64 cand = round_up_align(window_start, align);
    if (cand == 0)                                 return -1;
    for (struct Vma *cur = p->as->vmas; cur; cur = cur->next) {
        if (cur->magic != VMA_MAGIC)
            extinction("vma_find_gap: corrupted list entry");
        // A VMA entirely at/below `cand` does not constrain it.
        if (cur->vaddr_end <= cand)                continue;
        // A VMA starting at/after the window end cannot bound a gap
        // inside the window; the list is sorted, so neither can any
        // later VMA — stop.
        if (cur->vaddr_start >= window_end)        break;
        // Does [cand, cand + length) fit in the gap before `cur`?
        if (cur->vaddr_start >= cand &&
            cur->vaddr_start - cand >= length) {
            *out_vaddr = cand;
            return 0;
        }
        // No fit before `cur`; it overlaps or abuts `cand`. Jump the
        // candidate past it — cur->vaddr_end > cand here (the entirely-
        // below case was filtered by the first check).
        cand = round_up_align(cur->vaddr_end, align);
        if (cand == 0)                             return -1;
    }
    // Past the last constraining VMA — take the tail gap if it fits.
    if (cand < window_end && window_end - cand >= length) {
        *out_vaddr = cand;
        return 0;
    }
    return -1;
}

// =============================================================================
// B-1a: the permission ceiling -- reprotect a range (vma.h has the contract).
// =============================================================================

// The backing kinds a protect may touch. CODE is the I-42 pair (one charge, two
// aliases: neither is a plain mapping); MMIO / DMA / HOSTMEM are I-34 hardware
// windows whose permissions were conferred, not chosen.
static bool reprotect_admits(const struct Burrow *b) {
    switch (b->type) {
    case BURROW_TYPE_ANON:
    case BURROW_TYPE_ANON_LAZY:
    case BURROW_TYPE_FILE:
        return true;
    default:
        return false;
    }
}

int vma_reprotect_precheck_in(struct AddrSpace *as, u64 vaddr, u64 length,
                              u32 prot) {
    if (!as)                                            return -(int)T_E_INVAL;
    if (length == 0)                                    return -(int)T_E_INVAL;
    if (vaddr  & (PAGE_SIZE - 1))                       return -(int)T_E_INVAL;
    if (length & (PAGE_SIZE - 1))                       return -(int)T_E_INVAL;
    u64 end = vaddr + length;
    if (end < vaddr)                                    return -(int)T_E_INVAL;
    // X is never a target (ARCH 6.5). The syscall boundary refuses it before any
    // lookup; this is the mechanism refusing it too, so no in-kernel caller can
    // reach an RX target through a path the boundary did not see.
    if (prot & VMA_PROT_EXEC)                           return -(int)T_E_ACCES;
    if ((prot & VMA_PROT_WRITE) && !(prot & VMA_PROT_READ)) return -(int)T_E_INVAL;
    if (prot & ~(u32)(VMA_PROT_READ | VMA_PROT_WRITE))  return -(int)T_E_INVAL;

    u64 cur = vaddr;
    struct Vma *v = vma_next_overlap_in(as, vaddr, end);
    if (!v)                                             return -(int)T_E_NOMEM;
    // ONE scan from the head, then the successors: the list is sorted, so every
    // later mapping in the range is a `next`. A re-scan per mapping was
    // O(k x N), and this loop ran four times per protect (B-1a audit F2).
    for (; v && v->vaddr_start < end; v = v->next) {
        if (v->magic != VMA_MAGIC)
            extinction("vma_reprotect_precheck: corrupted list entry");
        if (v->vaddr_start > cur)                       return -(int)T_E_NOMEM; // a hole
        if (!v->burrow)                                 return -(int)T_E_NOMEM; // a guard: reserved, not mapped
        if (v->burrow->magic != VMO_MAGIC)              return -(int)T_E_ACCES;
        if (v->flags & VMA_FLAG_SHARED_IN)              return -(int)T_E_ACCES;
        if (!reprotect_admits(v->burrow))               return -(int)T_E_ACCES;
        if (prot & ~vma_prot_max(v))                    return -(int)T_E_ACCES; // above the ceiling
        cur = v->vaddr_end;
    }
    if (cur < end)                                      return -(int)T_E_NOMEM; // a hole at the tail
    return 0;
}

// The I-32 headroom for the (at most two) pieces a cut adds, decided by
// burrow_protect_in BEFORE its uninstall -- so a cap hit refuses changing
// nothing, not even a re-fault -- and AFTER the no-op short-circuit, since a
// no-op cuts nothing. Caller has run the precheck (the range is contiguous).
// vma_reprotect_range_in re-checks it under the same lock hold, where the
// count cannot have moved.
int vma_reprotect_headroom_in(struct AddrSpace *as, bool exempt, u64 vaddr,
                              u64 length) {
    if (exempt) return 0;
    u64 end = vaddr + length;
    struct Vma *first = vma_next_overlap_in(as, vaddr, end);
    if (!first)                                         return -(int)T_E_NOMEM;
    struct Vma *last = first;
    for (struct Vma *v = first; v && v->vaddr_start < end; v = v->next)
        last = v;
    u32 adding = (first->vaddr_start < vaddr ? 1u : 0u) +
                 (last->vaddr_end    > end   ? 1u : 0u);
    if (!adding) return 0;
    u32 cnt = __atomic_load_n(&as->vma_count, __ATOMIC_RELAXED);
    return (cnt > PROC_VMA_MAX - adding) ? -(int)T_E_NOMEM : 0;
}

// Two adjacent VMAs that describe one contiguous window of one Burrow at one
// prot are one mapping split by history. Shared-in mappings are never merged:
// their exact (vaddr, length) is what the sharer's detach matches.
static bool reprotect_mergeable(const struct Vma *a, const struct Vma *b) {
    if (!a->burrow || !b->burrow)                  return false;
    if (a->burrow != b->burrow)                    return false;
    if (a->vaddr_end != b->vaddr_start)            return false;
    if (a->prot != b->prot || a->flags != b->flags) return false;
    if (a->flags & VMA_FLAG_SHARED_IN)             return false;
    return a->burrow_offset + (a->vaddr_end - a->vaddr_start) == b->burrow_offset;
}

// Coalesce every pair that involves a piece inside [lo, hi): the left
// neighbour with the first piece, the pieces among themselves, the last piece
// with the right neighbour. Two mappings both outside the range are never
// considered -- nothing about them changed.
static void reprotect_merge_in(struct AddrSpace *as, u64 lo, u64 hi) {
    struct Vma *v = vma_next_overlap_in(as, lo, hi);
    if (!v) return;
    struct Vma *a = v->prev ? v->prev : v;
    while (a && a->next && a->vaddr_start < hi) {
        struct Vma *b = a->next;
        if (!reprotect_mergeable(a, b)) { a = b; continue; }
        a->vaddr_end = b->vaddr_end;
        vma_remove_in(as, b);
        // `a` still maps the same Burrow, so this drop cannot be its last: the
        // deferred form's "owes a free" return is the impossible case, and a
        // free under as->lock is exactly what it exists to prevent.
        if (vma_free_deferred(b, NULL) != NULL)
            extinction("vma merge dropped the last ref of a Burrow its neighbour maps");
    }
}

// A protect that would change nothing -- every mapping in the range already at
// `prot`, and (no seal, or) every ceiling already `prot` -- is answered 0
// before any uninstall or cut. A sub-range cut would otherwise still demand up
// to two slots of I-32 headroom for pieces the merge pass folds straight back,
// so at PROC_VMA_MAX - 1 a no-op answered ENOMEM where Linux succeeds (B-1a
// audit F6). Caller has run the precheck: the range is contiguous and admitted.
bool vma_reprotect_is_noop_in(struct AddrSpace *as, u64 vaddr, u64 length,
                              u32 prot, bool seal) {
    u64 end = vaddr + length;
    for (struct Vma *v = vma_next_overlap_in(as, vaddr, end);
         v && v->vaddr_start < end; v = v->next) {
        if (v->prot != prot)                        return false;
        if (seal && vma_prot_max(v) != prot)        return false;
    }
    return true;
}

int vma_reprotect_range_in(struct AddrSpace *as, bool exempt,
                           u64 vaddr, u64 length, u32 prot, bool seal) {
    int rc = vma_reprotect_precheck_in(as, vaddr, length, prot);
    if (rc != 0) return rc;
    if (vma_reprotect_is_noop_in(as, vaddr, length, prot, seal)) return 0;
    u64 end = vaddr + length;

    struct Vma *first = vma_next_overlap_in(as, vaddr, end);
    struct Vma *last  = first;
    for (struct Vma *v = first; v && v->vaddr_start < end; v = v->next)
        last = v;                        // one scan, then successors (audit F2)

    // Only the first and the last mapping can be cut, so at most two pieces --
    // allocated BEFORE the list is touched (the D-3b shape), so a shortfall
    // costs nothing but the frees below. Each piece re-derives its offset from
    // the SAME (burrow, offset) relation its parent had, which is what keeps
    // every surviving VA's byte identity unchanged across the cut, and copies
    // the parent's flags whole: the ceiling AND the COW routing bit (the
    // per-page share counts are per page, so a cut is sound for a forked
    // mapping -- which is why addrspace_clone dedupes its clones per Burrow).
    bool cut_left  = first->vaddr_start < vaddr;
    bool cut_right = last->vaddr_end    > end;
    struct Vma *lpiece = NULL, *rpiece = NULL;
    if (cut_left) {
        lpiece = vma_alloc(first->vaddr_start, vaddr, first->prot,
                           first->burrow, first->burrow_offset);
        if (!lpiece) return -(int)T_E_NOMEM;
        lpiece->flags = first->flags;
    }
    if (cut_right) {
        rpiece = vma_alloc(end, last->vaddr_end, last->prot, last->burrow,
                           last->burrow_offset + (end - last->vaddr_start));
        if (!rpiece) { if (lpiece) vma_free(lpiece); return -(int)T_E_NOMEM; }
        rpiece->flags = last->flags;
    }

    // I-32 headroom, checked BEFORE the mutation so a cap-hit changes nothing.
    // Under as->lock the count is stable, so the charges taken by the inserts
    // below cannot then fail.
    u32 adding = (lpiece ? 1u : 0u) + (rpiece ? 1u : 0u);
    if (!exempt && adding) {
        u32 cnt = __atomic_load_n(&as->vma_count, __ATOMIC_RELAXED);
        if (cnt > PROC_VMA_MAX - adding) {
            if (lpiece) vma_free(lpiece);
            if (rpiece) vma_free(rpiece);
            return -(int)T_E_NOMEM;
        }
    }

    // The ORIGINAL structs survive as the in-range pieces (shrunk in place, so
    // an interior mapping is never reallocated and no mapping ref ever drops);
    // the new pieces are the remainders outside the range. Save what a
    // rollback puts back.
    u64 first_start = first->vaddr_start;
    u64 first_off   = first->burrow_offset;
    u64 last_end    = last->vaddr_end;
    if (cut_left) {
        // start and offset move by the SAME delta: identity preserved. Still
        // sorted: the predecessor ends at or below the old start.
        first->burrow_offset += vaddr - first->vaddr_start;
        first->vaddr_start    = vaddr;
    }
    if (cut_right)
        last->vaddr_end = end;          // same start, same offset

    if (lpiece && vma_insert_in(as, exempt, lpiece) != 0) goto rollback;
    if (rpiece && vma_insert_in(as, exempt, rpiece) != 0) {
        if (lpiece) vma_remove_in(as, lpiece);
        goto rollback;
    }

    // APPLY, in place. The range's leaf PTEs are already gone (the caller's
    // half of the contract), so the next fault sees the new prot: refused at
    // none, installed read-only at R, writable at RW -- fault.c step 2.
    for (struct Vma *v = vma_next_overlap_in(as, vaddr, end);
         v && v->vaddr_start < end; v = v->next) {
        v->prot = prot;
        if (seal) vma_set_prot_max(v, prot);
    }

    reprotect_merge_in(as, vaddr, end);
    return 0;

rollback:
    // Nothing changed: the originals go back to exactly the ranges they had.
    // Unreachable in practice (the cap was checked, the vacated ranges cannot
    // overlap), kept so an insert refusal can never leave a hole.
    if (lpiece) vma_free(lpiece);
    if (rpiece) vma_free(rpiece);
    first->vaddr_start   = first_start;
    first->burrow_offset = first_off;
    last->vaddr_end      = last_end;
    return -(int)T_E_NOMEM;
}

void vma_drain(struct Proc *p) {
    if (!p) return;
    // LINEAGE L-1: no address space means no VMA list to drain -- a kernel-only
    // Proc, or a proc_alloc rollback that failed before addrspace_alloc ran (the
    // path proc_free reaches with a partially-built Proc).
    vma_drain_in(p->as);
}

// LINEAGE L-2: drain by address space. Two callers with genuinely different
// shapes -- proc_free's (through the wrapper above, on a Proc that is dying) and
// proc_exec_replace's, which drains the OUTGOING address space of a Proc that
// stays alive, and the DETACHED half-built one on the exec-failure rollback.
// Neither needs a Proc: the drain frees Vma structs and drops Burrow mapping
// refs, and the I-32 uncharge it performs is pure arithmetic on this address
// space's own counter.
void vma_drain_in(struct AddrSpace *as) {
    if (!as) return;

    // G-3 (the reaper-audit F1 fix): vma_drain now TAKES p->vma_lock --
    // retiring its lockless exemption. The weft reaper's cross-Proc
    // force-reclaim holds the target's vma_lock ACROSS its per-page
    // TLBI unmap after dropping g_proc_table_lock (so the multi-ms loop
    // runs IRQs-on, off the global lock); this acquire is what makes a
    // reap racing that window serialize instead of draining under it.
    // proc_free's callers are otherwise single-threaded here (the
    // original exemption argument), so the lock is uncontended on every
    // path but the rare reclaim race.
    // D-3c F1: DEFER the sleeping Burrow frees past the unlock (the same twin
    // #193/the detach paths use). vma_free of a 9P-backed FILE Burrow reaches
    // spoor_clunk, which may sleep -- and this whole drain runs under as->lock,
    // so an inline free would be the lock-across-sleep extinction. Reachable at
    // proc-exit for an exec text Burrow paged from a 9P FS (D-4/D-5); latent
    // today only because /bin execs come from the non-sleeping devramfs. Collect
    // the dead Burrows on a deferred_free_next stack; free after the unlock.
    struct Burrow *dead = NULL;
    spin_lock(&as->lock);
    while (as->vmas) {
        struct Vma *v = as->vmas;
        // G-2: a SHARED_IN VMA's teardown uncharges the shared-in budget (the
        // burrow_share_into pairing). Moot for a dying Proc's counters but
        // keeps the invariant (shared_map_pages == Σ flagged-VMA pages) exact
        // on every path, so the accounting is auditable at any point -- and it
        // is NOT moot for exec, where the Proc survives the drain.
        if (v->flags & VMA_FLAG_SHARED_IN)
            addrspace_uncharge_shared_map(as,
                (u32)((v->vaddr_end - v->vaddr_start) / PAGE_SIZE));
        vma_remove_in(as, v);
        struct Burrow *tf = vma_free_deferred(v, NULL);
        if (tf) { tf->deferred_free_next = dead; dead = tf; }
    }
    spin_unlock(&as->lock);
    burrow_free_deferred(dead);          // the whole chain, no lock held
}

// =============================================================================
// Diagnostics
// =============================================================================

u64 vma_total_allocated(void) {
    return __atomic_load_n(&g_vma_allocated, __ATOMIC_RELAXED);
}

u64 vma_total_freed(void) {
    return __atomic_load_n(&g_vma_freed, __ATOMIC_RELAXED);
}

u64 vma_scan_steps(void) {
    return __atomic_load_n(&g_vma_scan_steps, __ATOMIC_RELAXED);
}
