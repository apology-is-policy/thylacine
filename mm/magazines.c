// Per-CPU magazines — Bonwick & Adams 2001 ("Magazines and Vmem"),
// illumos-style kmem. Each CPU has small stacks of pre-fetched
// pages; alloc / free hot-path is a stack pop / push without
// touching the buddy lock. Refill on empty / drain on full move
// MAGAZINE_SIZE/2 pages per boundary crossing — but each page is a
// separate buddy_alloc/buddy_free call, so the buddy lock is taken
// once PER PAGE on the refill/drain path, not once per batch (a
// buddy bulk-op under one lock hold is the named v1.x lift,
// HT11.R1-F6).
//
// Refill / drain target half-full so a subsequent free / alloc
// doesn't immediately swap back. Linux uses a similar half-fill
// hysteresis on its per-CPU page cache.
//
// Per ARCHITECTURE.md §6.3.

#include "magazines.h"
#include "buddy.h"

#include <thylacine/extinction.h> // ASSERT_OR_DIE (count-corruption guard)
#include <thylacine/page.h>
#include <thylacine/smp.h>        // smp_cpu_idx_self (MPIDR_EL1.Aff0)
#include <thylacine/spinlock.h>   // spin_lock_irqsave(NULL): bare IRQ mask
#include <thylacine/types.h>

struct percpu_data g_percpu[NCPUS];

void magazines_init(void) {
    for (int cpu = 0; cpu < NCPUS; cpu++) {
        for (int i = 0; i < NUM_MAG_ORDERS; i++) {
            g_percpu[cpu].mags[i].count = 0;
        }
    }
}

// Map an allocation order to a magazine slot index, or -1 if the
// order isn't magazine-managed.
static int order_to_mag_idx(unsigned order) {
    switch (order) {
    case 0: return MAG_IDX_ORDER_4K;
    case 9: return MAG_IDX_ORDER_2M;
    default: return -1;
    }
}

static unsigned mag_idx_to_order(int idx) {
    return idx == MAG_IDX_ORDER_4K ? 0 : 9;
}

// Current CPU = MPIDR_EL1.Aff0. Callers hold IRQs masked across the
// my_cpu() -> set access, so the result cannot change mid-op via
// preemption (which is IRQ-driven). Clamp a wild MPIDR onto slot 0
// (mirrors the fault.c #806 guard) so an out-of-range Aff0 can never
// index past g_percpu[].
//
// KERNEL-WIDE ASSUMPTION (#807 audit F1): Aff0 == the dense logical CPU
// index [0, online). This is the same convention sched.c (g_cpu_sched[]),
// gic.c (redistributor base), fault.c, and halls.c already require -- and
// per_cpu_main is even handed the dense DTB index as its sched id. On a
// clustered SoC (big.LITTLE: Aff0 repeats per cluster) this folds two CPUs
// onto one slot, re-opening the shared-set race for them -- so the Lazarus
// / PORTABILITY arc must introduce ONE canonical MPIDR->dense-id map and
// route smp_cpu_idx_self() through it (fixing magazines/sched/gic/fault/
// halls uniformly). Dormant on QEMU virt + RPi 400 (dense Aff0).
static inline int my_cpu(void) {
    unsigned c = smp_cpu_idx_self();
    return c < NCPUS ? (int)c : 0;
}

// Refill a magazine to half full. Called when count == 0; partial
// refill on OOM is OK — the caller will return NULL and the next
// allocation will retry.
static void mag_refill(struct magazine *m, unsigned order) {
    int target = MAGAZINE_SIZE / 2;
    while (m->count < target) {
        struct page *p = buddy_alloc(&g_zone0, order);
        if (!p) break;
        m->entries[m->count++] = p;
    }
}

// Drain a magazine to half full. Called when count == MAGAZINE_SIZE.
static void mag_drain(struct magazine *m, unsigned order) {
    int target = MAGAZINE_SIZE / 2;
    while (m->count > target) {
        struct page *p = m->entries[--m->count];
        buddy_free(&g_zone0, p, order);
    }
}

struct page *mag_alloc(unsigned order) {
    int idx = order_to_mag_idx(order);
    if (idx < 0) return NULL;

    // IRQ-masked across my_cpu() + the pop (+ any refill): pins this CPU's
    // set so no IRQ-context alloc re-enters it and preemption can't migrate
    // us onto another CPU's set mid-op. NULL lock = mask-only (spinlock.h).
    // mag_refill's buddy_alloc takes the zone lock with its own nested
    // irqsave -- fine (zone lock is a leaf; order is IRQ-off -> zone lock).
    irq_state_t s = spin_lock_irqsave(NULL);
    struct magazine *m = &g_percpu[my_cpu()].mags[idx];
    // #807 regression guard: a corrupt count (e.g. a regressed cross-CPU
    // race re-opening this set) trips here LOUDLY instead of silently
    // double-allocating. Cheap (a compare under the mask we already hold).
    ASSERT_OR_DIE((unsigned)m->count <= MAGAZINE_SIZE, "mag_alloc: count corrupt");
    if (m->count == 0) {
        mag_refill(m, mag_idx_to_order(idx));
        if (m->count == 0) {
            spin_unlock_irqrestore(NULL, s);
            return NULL;
        }
    }
    struct page *p = m->entries[--m->count];
    spin_unlock_irqrestore(NULL, s);

    // p is out of the magazine now -- private to this caller; init outside
    // the critical section.
    p->order    = order;
    p->flags    = PG_KERNEL;
    p->refcount = 1;
    p->next     = NULL;
    p->prev     = NULL;
    return p;
}

bool mag_free(struct page *p, unsigned order) {
    int idx = order_to_mag_idx(order);
    if (idx < 0) return false;

    // p is the caller's (being freed) -- not yet in any set -- so init it
    // outside the critical section.
    p->next  = NULL;
    p->prev  = NULL;
    p->flags = 0;       // not PG_FREE — magazine ownership, not free list

    // IRQ-masked across my_cpu() + the push (+ any drain): same rationale
    // as mag_alloc -- pins this CPU's set, non-reentrantly.
    irq_state_t s = spin_lock_irqsave(NULL);
    struct magazine *m = &g_percpu[my_cpu()].mags[idx];
    ASSERT_OR_DIE((unsigned)m->count <= MAGAZINE_SIZE, "mag_free: count corrupt");
    if (m->count == MAGAZINE_SIZE) {
        mag_drain(m, mag_idx_to_order(idx));
    }
    m->entries[m->count++] = p;
    spin_unlock_irqrestore(NULL, s);
    return true;
}

// Drains EVERY CPU's set back to the buddy. QUIESCENT-ONLY: it touches
// peer CPUs' sets without IRQ-masking or cross-CPU coordination, so it is
// safe only when no CPU is concurrently allocating -- i.e. the in-kernel
// test harness (secondaries parked in WFI) or a single-threaded shutdown.
// Never call it during active SMP workload.
void magazines_drain_all(void) {
    for (int cpu = 0; cpu < NCPUS; cpu++) {
        for (int idx = 0; idx < NUM_MAG_ORDERS; idx++) {
            struct magazine *m = &g_percpu[cpu].mags[idx];
            unsigned order = mag_idx_to_order(idx);
            while (m->count > 0) {
                struct page *p = m->entries[--m->count];
                buddy_free(&g_zone0, p, order);
            }
        }
    }
}
