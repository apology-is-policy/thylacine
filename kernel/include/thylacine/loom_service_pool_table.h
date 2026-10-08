// Joint slot/member registration transaction, ASYNC-SERVICE-BUFFERS.md.
#ifndef THYLACINE_LOOM_SERVICE_POOL_TABLE_H
#define THYLACINE_LOOM_SERVICE_POOL_TABLE_H
#include <thylacine/loom_service_slots.h>
#include <thylacine/loom_service_pool.h>

struct loom_service_pool_table {
    struct loom_pool_bank bank;
    struct loom_pool pools[LOOM_SERVICE_SLOT_COUNT];
};
_Static_assert(sizeof(struct loom_service_pool_table) == 7176, "pool table ledger");
// Every call holds the owning Loom lock and a retained Loom reference. Caller
// has resolved/pinned canonical Burrows and checked fixed-I/O conflicts before
// prepare. These helpers neither allocate nor own backing pins; drop pins only
// outside the lock after successful rollback/reap. Private Loom owns and
// charges this table. User buffer authority/resolution remain caller obligations;
// service registration and protocol handlers are not enabled by these helpers.
// Prepare consumes a shared slot AND member capacity before output copyout.
// Publish or rollback follows copyout under the same lock; close may win first.
int loom_service_pool_prepare(struct loom_slot_table *, struct loom_service_pool_table *,
                              const struct loom_pool_extent *, u32,
                              struct loom_service_ref *);
int loom_service_pool_publish(struct loom_slot_table *, struct loom_service_pool_table *,
                              struct loom_service_ref);
int loom_service_pool_rollback(struct loom_slot_table *, struct loom_service_pool_table *,
                               struct loom_service_ref);
// Busy leaves BOTH ledgers unchanged. Return payloads through the exact receipt
// API first; CQ consumption alone never makes a member reapable.
int loom_service_pool_reap(struct loom_slot_table *, struct loom_service_pool_table *,
                           struct loom_service_ref);
bool loom_service_pools_empty(const struct loom_service_pool_table *);
#endif
