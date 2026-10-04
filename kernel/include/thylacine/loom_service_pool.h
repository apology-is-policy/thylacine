// Internal provided-buffer state, ASYNC-SERVICE-BUFFERS.md. Not a user ABI.
#ifndef THYLACINE_LOOM_SERVICE_POOL_H
#define THYLACINE_LOOM_SERVICE_POOL_H
#include <thylacine/loom_service_abi.h>

// One bank per private ring; descriptors live in its SHARED service slot table.
// Caller holds the ring lock for every operation, including fixed-I/O exclusion
// and pool publication. No internal allocation, locks, usercopy or callbacks.
// Extents are canonical offsets within PINNED Burrows, resolved by the caller;
// virtual addresses and user buffer indices are deliberately absent here.
struct loom_pool_extent { const void *backing; u64 offset, length; };
enum loom_pool_phase { LPOOL_EMPTY, LPOOL_RESERVED, LPOOL_LIVE };
enum loom_member_phase { LMEMBER_FREE, LMEMBER_AVAILABLE, LMEMBER_BUSY,
                         LMEMBER_PENDING, LMEMBER_LEASED };
struct loom_pool {
    struct loom_service_ref ref;
    u32 phase, count, streams, cursor;
};
struct loom_pool_cell {
    struct loom_service_ref pool;
    struct loom_pool_extent extent;
    u64 nonce, user_data;
    u32 ordinal, phase, requested, result;
    bool more;
};
struct loom_pool_bank {
    u64 next_nonce; // last issued value; zero initially, never wraps
    struct loom_pool_cell cells[LOOM_SERVICE_POOL_MEMBERS];
};
struct loom_pool_result {
    struct loom_service_buffer_receipt receipt;
    u64 user_data;
    u32 length;
    bool more;
};
// Pin the internal storage ledger before the owner allocates/charges it.
_Static_assert(sizeof(struct loom_pool_cell) == 80, "pool cell ledger");
_Static_assert(sizeof(struct loom_pool_bank) == 5128, "pool bank ledger");
_Static_assert(sizeof(struct loom_pool) == 32, "pool descriptor ledger");
_Static_assert(sizeof(struct loom_pool_result) == 48, "pool result ledger");
// All storage is zero-initialized once by its owning ring/slot allocator.
// prepare validates everything before modifying bank/pool. Caller already owns
// each backing pin and excludes accepted fixed-I/O extents under the same lock.
// Provisional cells reserve quota AND overlap exclusion before usercopy unlock.
int loom_pool_prepare(struct loom_pool_bank *, struct loom_pool *,
                      struct loom_service_ref, const struct loom_pool_extent *, u32);
int loom_pool_publish(struct loom_pool *);
int loom_pool_rollback(struct loom_pool_bank *, struct loom_pool *);
// Validated canonical fixed-buffer range overlaps even a RESERVED pool.
bool loom_pool_conflicts(const struct loom_pool_bank *, struct loom_pool_extent);
int loom_pool_stream_get(struct loom_pool *);
int loom_pool_stream_put(struct loom_pool *);
int loom_pool_claim(struct loom_pool_bank *, struct loom_pool *, u32 maximum,
                    struct loom_service_buffer_receipt *);
// Caller finishes all local writers BEFORE release_busy (error/EOF/abort).
// Cancellation and copying+commit are serialized under the caller's ring lock.
// commit preserves a preallocated result; scope retirement must leave it alone.
int loom_pool_release_busy(struct loom_pool_bank *, const struct loom_pool *,
                           const struct loom_service_buffer_receipt *);
int loom_pool_commit(struct loom_pool_bank *, const struct loom_pool *,
                     const struct loom_service_buffer_receipt *, u32 length,
                     u64 user_data, bool more);
// Peek does not consume; deliver is called only with reserved CQ capacity and
// after copying BOTH CQE and receipt, before release publishing tail. There is
// deliberately NO CQ-head/acknowledgement callback into this state machine.
int loom_pool_peek(const struct loom_pool_bank *, const struct loom_pool *, u32,
                   struct loom_pool_result *);
int loom_pool_deliver(struct loom_pool_bank *, const struct loom_pool *,
                      const struct loom_service_buffer_receipt *);
int loom_pool_return(struct loom_pool_bank *, const struct loom_pool *,
                     const struct loom_service_buffer_receipt *);
int loom_pool_snapshot(const struct loom_pool_bank *, const struct loom_pool *,
                       struct loom_service_pool_snapshot *);
int loom_pool_reap(struct loom_pool_bank *, struct loom_pool *);
#endif
