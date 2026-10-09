// Internal private-service terminal obligations, not a user ABI.
#ifndef THYLACINE_LOOM_SERVICE_REQUESTS_H
#define THYLACINE_LOOM_SERVICE_REQUESTS_H
#include <thylacine/loom_service_nodes.h>
struct loom_request_ref { u32 index, reserved; u64 identity; };
enum loom_request_phase { LREQ_FREE, LREQ_PREPARED, LREQ_ACTIVE,
                          LREQ_TERMINAL, LREQ_DELIVERED };
struct loom_service_request {
    struct loom_service_ref scope;
    u64 identity, user_data, deadline;
    s32 result;
    u32 phase, borrows, owners; // one physical RPC-storage owner, independent of completion
};
struct loom_request_table {
    struct loom_service_request *records;
    u64 next_identity;
    u32 capacity, occupied;
    bool detached;
};
_Static_assert(sizeof(struct loom_service_request) == 56, "request obligation storage");
_Static_assert(sizeof(struct loom_request_table) == 32, "request table storage");
// Owner precharges/zeros records[capacity], with capacity equal to its CQ size.
// All calls hold the SAME Loom lock as nodes/slots. Caller retains the Loom and
// all enclosing protocol storage. No allocation, callbacks, usercopy or drops.
// Each occupied record pins its exact SCOPE; user_data never names a request.
// prepare burns a monotone identity and reserves terminal storage. Admission
// and all operation-specific authority checks still precede external effects.
int loom_request_prepare(struct loom_request_table *, struct loom_slot_table *,
                         struct loom_service_node_table *, struct loom_service_ref,
                         u64 user_data, u64 deadline, u64 now, struct loom_request_ref *);
// Attach the sole physical RPC-storage owner while PREPARED and still under Ll.
// Hold before releasing Ll to allocate/initialize storage. Delivery/abort cannot
// permit record reuse while this owner exists. No independent parser borrow is
// implied. Caller keeps its Loom/protocol roots through physical destruction.
int loom_request_hold_owner(struct loom_request_table *, struct loom_request_ref);
// After the actual owner storage is destroyed, using a saved exact ref. Requires
// no ACTIVE operation or remaining parser/buffer borrows; does not consume
// terminal or rewind ID.
int loom_request_drop_owner(struct loom_request_table *, struct loom_request_ref);
// Same admission predicates, without mutation, for locked native publication.
// Caller must hold the SAME lock continuously from check through admit.
int loom_request_check_admit(struct loom_request_table *, struct loom_slot_table *,
                             struct loom_service_node_table *, struct loom_request_ref, u64 now);
// Negative completion for a reserved operation whose native admission failed.
// No external effects or parser borrows may have escaped; retain CQ ownership.
int loom_request_reject(struct loom_request_table *, struct loom_slot_table *,
                        struct loom_request_ref, s32 reason);
int loom_request_admit(struct loom_request_table *, struct loom_slot_table *,
                       struct loom_service_node_table *, struct loom_request_ref, u64 now);
int loom_request_rollback(struct loom_request_table *, struct loom_slot_table *, struct loom_request_ref);
int loom_request_borrow(struct loom_request_table *, struct loom_slot_table *,
                        struct loom_service_node_table *, struct loom_request_ref);
int loom_request_put(struct loom_request_table *, struct loom_request_ref);
// Finish only after ALL local buffer/parser/callback borrows represented by the
// request are over. A success after close/abort is refused unchanged: caller
// must instead commit the actual cancellation/timeout reason. An earlier
// committed success stays immutable and may be delivered after scope abort.
int loom_request_finish(struct loom_request_table *, struct loom_slot_table *,
                        struct loom_service_node_table *, struct loom_request_ref, s32 result);
int loom_request_peek(const struct loom_request_table *, struct loom_request_ref,
                      u64 *user_data, s32 *result);
// Only after owner copies into reserved CQ capacity, before publishing CQ tail.
// Terminal records carry the final non-MORE obligation; provided-buffer receipt
// and multishot intermediate-result ownership remain in the pool ledger.
int loom_request_deliver(struct loom_request_table *, struct loom_request_ref);
// Owner has detached the consumer and ended every local protocol/buffer borrow.
// Closing slots and zero recorded borrows are necessary but not sufficient
// proof of that external lifetime fact. They are checked here as backstops.
int loom_requests_detach(struct loom_request_table *, const struct loom_slot_table *);
int loom_request_discard(struct loom_request_table *, struct loom_request_ref);
// Delivery/discard is not itself storage release. Reap only after local users
// no longer retain a request pointer; stale continuations use exact refs.
int loom_request_reap(struct loom_request_table *, struct loom_slot_table *, struct loom_request_ref);
bool loom_requests_empty(const struct loom_request_table *);
#endif
