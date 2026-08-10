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

    struct Vma *v = kmem_cache_alloc(g_vma_cache, KP_ZERO);
    if (!v) return NULL;

    v->magic       = VMA_MAGIC;
    v->vaddr_start = vaddr_start;
    v->vaddr_end   = vaddr_end;
    v->prot        = prot;
    v->burrow         = burrow;
    v->burrow_offset  = burrow_offset;
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
    // address space at PROC_VMA_MAX is rejected here, identically to an overlap (the
    // caller vma_frees the rejected Vma). The charge requires as->lock — every
    // vma_insert caller holds it (attach / share under vma_lock; the exec load path
    // builds a detached address space no other thread can reach). Paired by
    // addrspace_uncharge_vma in vma_remove_in. Charges nothing on failure, so no
    // rollback is needed on the rejected path.
    if (!addrspace_charge_vma(as, exempt)) return -1;

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

// DISTRO D-3b: the MAP_FIXED split/replace. See vma.h for the full contract --
// in particular why the old Vma is reused as the survivor rather than removed,
// and why that is what makes every failure path hole-free.
int vma_replace_range_in(struct AddrSpace *as, bool exempt,
                         u64 vaddr, u64 length,
                         struct Burrow *nb, u32 prot, u64 nb_offset) {
    if (!as || !nb)                       return -1;
    if (length == 0)                      return -1;
    if (vaddr  & (PAGE_SIZE - 1))         return -1;
    if (length & (PAGE_SIZE - 1))         return -1;
    u64 end = vaddr + length;
    if (end < vaddr)                      return -1;          // wrap

    struct Vma *old = vma_lookup_in(as, vaddr);
    if (!old) {
        // FREE SPACE -- nothing to split, so this is a plain fixed-address map.
        // Linux MAP_FIXED does not require the range to be already mapped; at an
        // unmapped address it simply places the mapping there. Refusing here is
        // what made the shell answer ENOMEM, which is a WORSE reply than the
        // ENOSYS it replaced: ENOMEM is indistinguishable from real memory
        // pressure, and an allocator reads it as OOM.
        //
        // vma_insert_in rejects any overlap, so this arm also catches the range
        // that starts free and runs INTO a later VMA -- the partial-overlap
        // case, which Linux would serve by unmapping the overlapped part and
        // which we refuse because partial unmap is post-v1.0.
        struct Vma *v = vma_alloc(vaddr, end, prot, nb, nb_offset);
        if (!v) return -1;
        if (vma_insert_in(as, exempt, v) != 0) { vma_free(v); return -1; }
        return 0;
    }
    // WHOLLY inside one VMA. A request spanning two VMAs is refused rather than
    // handled: musl's overlay always lands inside the whole-span reservation it
    // just made, so the multi-VMA shape has no producer -- and inventing one
    // here would mean inventing its failure semantics too.
    if (vaddr < old->vaddr_start || end > old->vaddr_end)     return -1;
    if (old->flags != 0 || !old->burrow)  return -1;

    bool want_left  = (vaddr > old->vaddr_start);
    bool want_right = (end   < old->vaddr_end);

    // Allocate every new piece BEFORE touching the list, so an allocation
    // shortfall costs nothing but the frees below.
    struct Vma *mid = vma_alloc(vaddr, end, prot, nb, nb_offset);
    if (!mid)                             return -1;

    // The right remainder re-derives its offset from the SAME (burrow, offset)
    // relation the old VMA had, which is what keeps every surviving VA's byte
    // identity unchanged across the cut.
    struct Vma *right = NULL;
    if (want_left && want_right) {
        right = vma_alloc(end, old->vaddr_end, old->prot, old->burrow,
                          old->burrow_offset + (end - old->vaddr_start));
        if (!right) { vma_free(mid); return -1; }
    }

    // I-32 headroom, checked BEFORE the mutation so a cap-hit changes nothing.
    // Under as->lock the count is stable (every mutator holds it), so the
    // charges taken below cannot then fail. `right` is the only case that adds
    // two VMAs; the others add one, and the exact-cover case adds none net.
    u32 adding = 1u + (right ? 1u : 0u);
    if (!exempt) {
        u32 cur = __atomic_load_n(&as->vma_count, __ATOMIC_RELAXED);
        u32 net = adding - ((want_left || want_right) ? 0u : 1u);
        if (cur > PROC_VMA_MAX - net) {
            vma_free(mid);
            if (right) vma_free(right);
            return -1;
        }
    }

    // Save what a rollback has to put back.
    u64 old_start  = old->vaddr_start;
    u64 old_end    = old->vaddr_end;
    u64 old_offset = old->burrow_offset;

    if (want_left) {
        // The survivor becomes the LEFT remainder: same start, same offset, so
        // its resident PTEs stay correct untouched. Shrinking only the end
        // cannot disturb the sort order.
        old->vaddr_end = vaddr;
    } else if (want_right) {
        // The survivor becomes the RIGHT remainder. start and offset move by
        // the SAME delta, so `burrow_offset + (va - vaddr_start)` is invariant
        // for every VA it still covers. Still sorted: its predecessor ends at
        // or below old_start < end, and its successor starts at or above
        // old_end.
        old->vaddr_start   = end;
        old->burrow_offset = old_offset + (end - old_start);
    } else {
        // Exact cover -- no remainder. This is the one case that removes.
        vma_remove_in(as, old);
    }

    if (vma_insert_in(as, exempt, mid) != 0) goto rollback_mid;
    if (right && vma_insert_in(as, exempt, right) != 0) {
        vma_remove_in(as, mid);
        goto rollback_mid;
    }

    if (!want_left && !want_right) vma_free(old);   // fully replaced
    return 0;

rollback_mid:
    // Nothing of the new mapping survives, and the survivor goes back to
    // exactly the range it had. The exact-cover re-insert cannot fail: same
    // lock hold, into the range just vacated (no overlap), with the count
    // strictly below its entry value (no cap refusal).
    vma_free(mid);
    if (right) vma_free(right);
    old->vaddr_start   = old_start;
    old->vaddr_end     = old_end;
    old->burrow_offset = old_offset;
    if (!want_left && !want_right) (void)vma_insert_in(as, exempt, old);
    return -1;
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
struct Vma *vma_next_overlap_in(struct AddrSpace *as, u64 lo, u64 hi) {
    if (!as || lo >= hi) return NULL;

    for (struct Vma *cur = as->vmas; cur; cur = cur->next) {
        if (cur->magic != VMA_MAGIC)
            extinction("vma_next_overlap: corrupted list entry");
        if (cur->vaddr_start >= hi) return NULL;   // sorted: nothing later overlaps
        if (cur->vaddr_end > lo) return cur;
    }
    return NULL;
}

// P6-pouch-mem: first-fit free-range finder for SYS_BURROW_ATTACH. The
// VMA list is sorted by vaddr_start ascending, so a single forward pass
// — advancing a candidate base past every VMA that blocks it — finds
// the lowest free gap of `length` bytes in [window_start, window_end).
int vma_find_gap(struct Proc *p, u64 length,
                 u64 window_start, u64 window_end, u64 *out_vaddr) {
    if (!p || !out_vaddr)                          return -1;
    if (length == 0)                               return -1;
    if (length        & (PAGE_SIZE - 1))           return -1;
    if (window_start  & (PAGE_SIZE - 1))           return -1;
    if (window_end    & (PAGE_SIZE - 1))           return -1;
    if (window_start > window_end)                 return -1;
    if (window_end - window_start < length)        return -1;

    // `cand` is the lowest VA not yet ruled out. Every comparison uses
    // subtraction guarded by an ordering check, so no `cand + length`
    // sum is ever formed — overflow-free for any window in the 2^47
    // user-VA space.
    u64 cand = window_start;
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
        cand = cur->vaddr_end;
    }
    // Past the last constraining VMA — take the tail gap if it fits.
    if (cand < window_end && window_end - cand >= length) {
        *out_vaddr = cand;
        return 0;
    }
    return -1;
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
    while (dead) { struct Burrow *n = dead->deferred_free_next;
                   dead->deferred_free_next = NULL;
                   burrow_free_deferred(dead); dead = n; }
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
