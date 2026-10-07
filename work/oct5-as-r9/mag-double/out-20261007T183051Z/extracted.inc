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

static inline int my_cpu(void) {
    unsigned c = smp_cpu_idx_self();
    return c < NCPUS ? (int)c : 0;
}

static void mag_refill(struct magazine *m, unsigned order) {
    int target = MAGAZINE_SIZE / 2;
    while (m->count < target) {
        struct page *p = buddy_alloc(&g_zone0, order);
        if (!p) break;
        m->entries[m->count++] = p;
    }
}

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

