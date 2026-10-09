#ifndef THYLACINE_LOOM_SERVICE_PROVIDED_H
#define THYLACINE_LOOM_SERVICE_PROVIDED_H
#include <thylacine/loom_service_io.h>
// Same immutable creator/SQE contract as fixed preparation. No member claimed
// or wire sent here. Stream ref keeps canonical registered pool extents pinned.
int loom_provided_prepare(struct Loom *,struct Proc *,const struct loom_sqe *,u64,
                          struct loom_service_operation **);
int loom_provided_release_prepared(struct loom_service_operation *);
int loom_provided_publish(struct loom_service_operation *,u64);
// All below except deliver hold Ll under sole protocol driver. AGAIN from begin
// means empty pool: leave QUEUED, wait for return, and still service deadlines.
// Local no-wire retries retain their BUSY member; never allocate another shot.
int loom_provided_begin_locked(struct loom_service_operation *,u64);
// Only SETTLING after actual callback/TX quiescence. Positive commits retain a
// precharged PENDING result; single shot also commits its terminal obligation.
// Multishot remains active, with at most one PENDING payload per operation.
int loom_provided_commit_locked(struct loom_service_operation *,u64);
int loom_provided_release_busy_locked(struct loom_service_operation *);
// Owns CQ copy/delivery + single-shot request delivery in one Ll interval.
// No abort check after committed success. CQ-full retains pending payload.
// Success never returns its lease. Multishot rearm only after delivery, using
// loom_rpc_rearm_read_locked with fresh time; final cancellation cannot overtake
// PENDING. Whole-ring detached discard is a separate, still-required close edge.
int loom_provided_deliver(struct loom_service_operation *);
// NEW rollback or RETIRED owner cleanup, no member/callback users. Stream count
// covers terminal bookkeeping too. Does not reap a pool or return any lease.
int loom_provided_detach_locked(struct loom_service_operation *);
#endif
