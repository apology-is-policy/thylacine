#ifndef THYLACINE_LOOM_SERVICE_RPC_H
#define THYLACINE_LOOM_SERVICE_RPC_H
#include <thylacine/loom_service_operation.h>
// Internal owner transitions, not user admission or protocol authority. All
// except capture, submit and advance hold Ll. Caller owns the actual operation and exact engine
// entry/Loom pins. Before enqueue it must resolve and retain FID/destination,
// payload and creator authority; do not infer those facts from this list.
// Same Ll must cover this pure check through FID claim and enqueue.
int loom_rpc_check_enqueue_locked(struct loom_service_operation *,u64 now);
int loom_rpc_enqueue_locked(struct loom_service_operation *,u64 now);
// Sole p->driving owner only: begin, requeue and settle surround actual client
// calls. Begin occurs before submit_async (which may callback synchronously).
// A successful begin MUST be followed by submission or explicit local error
// capture if client validation refused without taking ownership.
int loom_rpc_check_begin_locked(struct loom_service_operation *,u64 now);
int loom_rpc_rearm_read_locked(struct loom_service_operation *,u64 now);
int loom_rpc_begin_locked(struct loom_service_operation *,u64 now);
void loom_rpc_capture(struct p9_rpc *,int,struct p9_dispatch_result *);
// Only after the client call has returned; no callback/parser reference escapes.
// Use requeue for a next stat/open stage or a retryable no-wire AGAIN; the driver
// must prove the protocol disposition, never reissue an ambiguous wire effect.
int loom_rpc_requeue_locked(struct loom_service_operation *,u64 now);
// Sole driver outside Ll. Submit uses exactly program.wire and settles local
// validation refusal once. Advance consumes each callback attempt at most once.
int loom_rpc_submit(struct loom_service_operation *);
int loom_rpc_advance(struct loom_service_operation *,enum loom_program_action *);
int loom_rpc_settle_locked(struct loom_service_operation *);
// Settlement bars resubmission while real FID/buffer drops run unlocked. Caller
// proves all physical parser/buffer users ended before finish. Success requires
// a validated captured success and fresh absolute deadline/abort check. For FID
// publication call check_finish before publishing, then finish, in ONE Ll hold.
// A timeout/cancellation refusal leaves state unchanged for scope abort followed
// by a negative terminal. The caller must abort the scope on admitted expiry.
int loom_rpc_check_finish_locked(struct loom_service_operation *,s32 result,u64 now);
int loom_rpc_finish_locked(struct loom_service_operation *,s32 result,u64 now);
// DELIVERED includes explicit closed-consumer discard. Unlink before actual
// physical free/refund, then drop request owner/reap and protocol/Loom roots.
// A terminal alone never lets storage disappear behind a blocked CQ consumer.
int loom_rpc_unlink_locked(struct loom_service_operation *);
#endif
