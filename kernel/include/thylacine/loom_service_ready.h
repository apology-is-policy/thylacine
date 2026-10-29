#ifndef THYLACINE_LOOM_SERVICE_READY_H
#define THYLACINE_LOOM_SERVICE_READY_H
#include <thylacine/loom_service_retirement.h>
enum loom_ready_action {
    LREADY_NONE, LREADY_QUIESCE, LREADY_CANCEL, LREADY_ADVANCE,
    LREADY_SUBMIT, LREADY_COMMIT, LREADY_DELIVER, LREADY_REARM,
    LREADY_POST, LREADY_FREE, LREADY_DISCARD, LREADY_IO
};
struct loom_ready_interest { bool work; short events; u64 deadline; };
struct loom_ready_selection {
    enum loom_ready_action action;
    struct loom_service_operation *operation;
    s32 reason;
};
// Both APIs hold Ll with an independently retained engine entry + Loom root.
// Interest returns values only: no operation pointer escapes an unlocked sample.
// Native poll registration precedes the caller's recheck. RETURN/CQ-head changes
// and a driver/constructor return must wake that same executor predicate.
struct loom_ready_interest loom_ready_interest_locked(struct loom_service_protocol *,u64);
// Revalidates selection under Ll, claims p.driving and rotates the chosen owner
// to the tail atomically. Only a successful claim returns an operation pointer;
// the caller keeps driving plus the independent entry root through its use/free.
// native_ready authorizes one nonblocking I/O step, never bypasses local gates.
// Caller executes ONE selected transition then releases driving under Ll and
// notifies waiters. No submission, callback, free or native poll happens here.
int loom_ready_claim_locked(struct loom_service_protocol *,u64,bool native_ready,
                            struct loom_ready_selection *);
#endif
