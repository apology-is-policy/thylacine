// Per-CPU magazines — Bonwick & Adams 2001 ("Magazines and Vmem"),
// illumos-style kmem. Each CPU has small stacks of pre-fetched
// pages; alloc / free hot-path is a stack pop / push without
// touching the buddy lock. Refill on empty / drain on full are
// batched so each buddy-lock acquisition covers MAGAZINE_SIZE pages.
//
// Refill / drain target half-full so a subsequent free / alloc
// doesn't immediately swap back. Linux uses a similar half-fill
// hysteresis on its per-CPU page cache.
//
// Per ARCHITECTURE.md §6.3.

#include "magazines.h"
#include "buddy.h"

#include <thylacine/page.h>
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

// Current CPU. P1-D pins to CPU 0; P1-F reads MPIDR_EL1.Aff0.
static inline int my_cpu(void) {
    return 0;
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

    struct magazine *m = &g_percpu[my_cpu()].mags[idx];
    if (m->count == 0) {
        mag_refill(m, mag_idx_to_order(idx));
        if (m->count == 0) return NULL;
    }
    struct page *p = m->entries[--m->count];
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

    struct magazine *m = &g_percpu[my_cpu()].mags[idx];
    if (m->count == MAGAZINE_SIZE) {
        mag_drain(m, mag_idx_to_order(idx));
    }
    p->next  = NULL;
    p->prev  = NULL;
    p->flags = 0;       // not PG_FREE — magazine ownership, not free list
    m->entries[m->count++] = p;
    return true;
}

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
