// Internal bounded owner index and readiness. Private user dispatch stays disabled.
#ifndef THYLACINE_LOOM_SERVICE_ENGINE_H
#define THYLACINE_LOOM_SERVICE_ENGINE_H
#include <thylacine/loom_service_abi.h>
#include <thylacine/poll.h>
#include <thylacine/proc.h>
struct Loom;
struct loom_service_protocol;
enum loom_engine_phase { LENGINE_EMPTY, LENGINE_INSTALLING, LENGINE_LIVE };
struct loom_engine_entry {
    struct loom_service_protocol *protocol;
    u64 incarnation;
    u32 pins, phase;
};
struct loom_service_engine {
    struct loom_engine_entry entries[LOOM_SERVICE_SLOT_COUNT];
    u32 count, cursor;
    // One serialized creator submission, with a precharged refusal record.
    // A refused copied SQE needs no scope pin; it has made no external effects.
    // Ll owns these fields. Pending refusal blocks the next SQ claim.
    u64 rejected_user_data;
    s32 rejected_result;
    bool submitting, rejected;
    struct ProcSqpollTicket worker_ticket;
    bool worker_starting; // Ll serializes unpublished internal start attempts
    // Single wait owner, retained entry pins and embedded native hooks fit the
    // already charged index page. Only that owner touches the wait fields.
    // change is atomic: the Rendez predicate NEVER takes Ll (lock inversion).
    u64 change, wait_change, wait_mask, wait_deadline;
    u32 wait_cq_head;
    bool wait_active, wait_work;
    struct poll_waiter waiters[LOOM_SERVICE_SLOT_COUNT];
};
// A separately charged buddy page in a private Loom, zeroed at creation. All fields below are serialized by the SAME Loom lock.
// Caller retains l for every call, including final collection. Adopt transfers
// p only on return 0. Once indexed, even failed publication retains a terminal
// obligation; its negative result is driven/posted through the ordinary path.
// Install owns a pin through native publication AND unlocked accept wake.
int loom_engine_adopt(struct loom_service_protocol *, u64 now);
// Get/put retain protocol storage, not authority to issue general RPCs. A get
// against installing storage returns AGAIN. Abort handles that state internally.
// No pointer may outlive its put; exactly one put per successful get.
int loom_engine_get(struct Loom *, struct loom_service_ref, struct loom_service_protocol **);
int loom_engine_put(struct Loom *, struct loom_service_ref, struct loom_service_protocol *);
int loom_engine_abort(struct Loom *, struct loom_service_ref, s32 reason);
// One round-robin scope visit (one bounded client step, at most one CQ post).
// Readiness-aware alias of ready_visit; returns 1 on progress, 0 if idle.
// The executor must still register native poll interest and recheck before sleep.
int loom_engine_visit(struct Loom *, u64 now);
// Removes only locally retired owners with delivered/discarded terminals and
// zero operation owners and entry pins. Final drops outside Ll. Leaves scope metadata for REAP.
int loom_engine_collect(struct Loom *, struct loom_service_ref);
// Whole-ring-close consumer detach requires every indexed protocol locally
// retired, no entry/install/parser/fixed borrows. Then discard retained terminals
// and collect their storage, independent of CQ room. No native peer wait.
int loom_engine_discard_closed(struct Loom *);
// One close quantum: 0 blocked on external local holders, 1 immediate work,
// 2 complete (index/request/fixed ownership empty after consumer detach).
// Drains operations even after local retirement, before global detach/collection.
// A separate visit entry pin and Loom root survive consuming an operation.
// Ownership actions publish change wakes; internal final puts are quiet.
int loom_engine_close_visit(struct Loom *);
bool loom_engine_empty(const struct loom_service_engine *);
// Readiness-aware bounded visit; no peer waits or allocation. Caller retains l.
// Scans at most64 owners plus one precharged refusal, performing one action round-robin.
int loom_engine_ready_visit(struct Loom *, u64 now);
// Internal wait transaction. A nonnegative arm result owns the sweep even when
// it reports immediate work (1); always disarm it. BUSY owns nothing. One owner
// may arm/sleep/disarm; concurrent explicit ready visits remain permitted.
// Caller retains l through disarm. tsleep uses the returned absolute deadline.
int loom_engine_wait_arm(struct Loom *, u64 now, u64 *deadline);
// ONLY a sleep/tsleep predicate: caller holds the existing sqpoll_park Rendez.
// No Ll, device, allocator, timer or protocol lock is acquired here.
int loom_engine_wait_changed(void *);
void loom_engine_wait_disarm(struct Loom *);
#endif
