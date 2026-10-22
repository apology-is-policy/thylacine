// Transactions over the existing identity and payload ledgers. The owning ring
// lock serializes both records; no callback, usercopy or allocation occurs here.
// Copyout runs BETWEEN prepare and publish/rollback. PROVISIONAL/RESERVED already
// exclude competing slot/member/fixed-I/O reservations but cannot be borrowed.
// Commit validates both phases before touching either. slot_publish may reject
// close without mutation; after success pool_publish cannot fail (RESERVED was
// checked under the same lock, and slot_publish never touches that record).
// Rollback/reap likewise validate every refusal BEFORE freeing either ledger.
// Non-repeating slot and lease counters belong to their ledgers and never reset.
// These functions supply no buffer authority or protocol lifetime guarantee.
#include <thylacine/loom_service_pool_table.h>
#include <thylacine/errno.h>

static struct loom_pool *pool_record(struct loom_slot_table *t,
                                     struct loom_service_pool_table *pools,
                                     struct loom_service_ref ref, u32 phase) {
    if (!t || !pools || ref.reserved || !ref.incarnation ||
        ref.slot >= LOOM_SERVICE_SLOT_COUNT) return NULL;
    struct loom_service_slot *s = &t->slots[ref.slot];
    struct loom_pool *p = &pools->pools[ref.slot];
    if (s->phase != phase || s->kind != LOOM_SERVICE_POOL ||
        s->incarnation != ref.incarnation || p->ref.slot != ref.slot ||
        p->ref.incarnation != ref.incarnation || p->ref.reserved) return NULL;
    u32 expected = phase == LSLOT_PROVISIONAL ? LPOOL_RESERVED : LPOOL_LIVE;
    return p->phase == expected ? p : NULL;
}

int loom_service_pool_prepare(struct loom_slot_table *t,
                               struct loom_service_pool_table *pools,
                               const struct loom_pool_extent *extents, u32 count,
                               struct loom_service_ref *out) {
    if (!t || !pools || !out) return -T_E_INVAL;
    struct loom_service_ref ref;
    int rc = loom_slot_reserve(t, LOOM_SERVICE_POOL, &ref);
    if (rc) return rc;
    rc = loom_pool_prepare(&pools->bank, &pools->pools[ref.slot], ref, extents, count);
    if (rc) {
        // The untouched provisional identity has no borrower; rollback cannot
        // fail while this lock remains held, even when member validation fails.
        (void)loom_slot_rollback(t, ref);
        return rc;
    }
    *out = ref;
    return 0;
}

int loom_service_pool_publish(struct loom_slot_table *t,
                               struct loom_service_pool_table *pools,
                               struct loom_service_ref ref) {
    struct loom_pool *p = pool_record(t, pools, ref, LSLOT_PROVISIONAL);
    if (!p) return -T_E_NOENT;
    int rc = loom_slot_publish(t, ref);
    if (rc) return rc;
    return loom_pool_publish(p);
}

int loom_service_pool_rollback(struct loom_slot_table *t,
                                struct loom_service_pool_table *pools,
                                struct loom_service_ref ref) {
    struct loom_pool *p = pool_record(t, pools, ref, LSLOT_PROVISIONAL);
    if (!p) return -T_E_NOENT;
    if (t->slots[ref.slot].pins) return -T_E_BUSY;
    (void)loom_pool_rollback(&pools->bank, p);
    return loom_slot_rollback(t, ref);
}

int loom_service_pool_reap(struct loom_slot_table *t,
                            struct loom_service_pool_table *pools,
                            struct loom_service_ref ref) {
    struct loom_pool *p = pool_record(t, pools, ref, LSLOT_LIVE);
    if (!p) return -T_E_NOENT;
    int rc = loom_pool_reap(&pools->bank, p);
    if (rc) return rc;
    // No streams, local writers, pending/leased payloads or metadata borrowers
    // remain. Both slot transitions are guaranteed by the checks above and the
    // common lock; the same identity cannot acquire a new borrower in between.
    (void)loom_slot_retire(t, ref);
    return loom_slot_reap(t, ref);
}

bool loom_service_pools_empty(const struct loom_service_pool_table *pools) {
    if (!pools) return false;
    for (u32 i = 0; i < LOOM_SERVICE_SLOT_COUNT; ++i)
        if (pools->pools[i].phase != LPOOL_EMPTY) return false;
    for (u32 i = 0; i < LOOM_SERVICE_POOL_MEMBERS; ++i)
        if (pools->bank.cells[i].phase != LMEMBER_FREE) return false;
    return true;
}
