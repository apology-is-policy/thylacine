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

void vma_free(struct Vma *v) {
    if (!v)                     extinction("vma_free(NULL)");
    if (v->magic != VMA_MAGIC)  extinction("vma_free of corrupted/already-freed Vma");
    if (v->next || v->prev)     extinction("vma_free of Vma still in a list");

    // Release the BURROW mapping ref. burrow_release_mapping may free the BURROW
    // if both handle_count and mapping_count reach zero (see
    // specs/burrow.tla).
    if (v->burrow) {
        burrow_release_mapping(v->burrow);
        v->burrow = NULL;
    }

    kmem_cache_free(g_vma_cache, v);
    __atomic_fetch_add(&g_vma_freed, 1u, __ATOMIC_RELAXED);
}

// =============================================================================
// Sorted-list operations
// =============================================================================

// True iff [a, b) overlaps [c, d). Both half-open intervals.
static inline bool ranges_overlap(u64 a, u64 b, u64 c, u64 d) {
    return a < d && c < b;
}

int vma_insert(struct Proc *p, struct Vma *v) {
    if (!p || !v)                extinction("vma_insert(NULL)");
    if (v->magic != VMA_MAGIC)   extinction("vma_insert of corrupted Vma");
    if (v->next || v->prev)      extinction("vma_insert of already-linked Vma");

    // Walk the sorted list to find:
    //   - The insertion point (last node with start < v->start).
    //   - Any overlap with existing VMAs.
    struct Vma *prev = NULL;
    struct Vma *cur  = p->vmas;
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

    // Insert v between prev and cur.
    v->prev = prev;
    v->next = cur;
    if (prev) prev->next = v;
    else      p->vmas    = v;
    if (cur)  cur->prev  = v;

    return 0;
}

void vma_remove(struct Proc *p, struct Vma *v) {
    if (!p || !v)                extinction("vma_remove(NULL)");
    if (v->magic != VMA_MAGIC)   extinction("vma_remove of corrupted Vma");

    if (v->prev) v->prev->next = v->next;
    else         p->vmas       = v->next;
    if (v->next) v->next->prev = v->prev;

    v->next = NULL;
    v->prev = NULL;
}

struct Vma *vma_lookup(struct Proc *p, u64 vaddr) {
    if (!p) return NULL;

    for (struct Vma *cur = p->vmas; cur; cur = cur->next) {
        if (cur->magic != VMA_MAGIC) extinction("vma_lookup: corrupted list entry");
        if (vaddr >= cur->vaddr_start && vaddr < cur->vaddr_end) return cur;
        // Sorted-list optimization: if cur->vaddr_start > vaddr, every
        // subsequent node has even larger start; lookup miss.
        if (cur->vaddr_start > vaddr) return NULL;
    }
    return NULL;
}

void vma_drain(struct Proc *p) {
    if (!p) return;

    while (p->vmas) {
        struct Vma *v = p->vmas;
        vma_remove(p, v);
        vma_free(v);
    }
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
