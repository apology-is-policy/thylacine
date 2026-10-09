#ifndef THYLACINE_LOOM_SERVICE_RETIREMENT_H
#define THYLACINE_LOOM_SERVICE_RETIREMENT_H
#include <thylacine/loom_service_provided.h>
// Outside Ll, exclusive driving plus independent engine entry/Loom root. Requires
// a real scope abort latch. Tears down native transport and actually aborts the
// client cursor/callbacks before publishing rpc_quiesced. Does not retire scope.
int loom_retirement_quiesce(struct loom_service_protocol *);
// Same exclusive driver/root. Whole-ring close only, not scope cancellation.
// Consumes one admitted operation after quiescence, even with full CQ and other
// active/unpublished owners. BUSY CLUNK can wait for earlier FID borrowers; rotate
// to them, never wait for all entry pins (each operation owns one). Success frees
// o: caller saves traversal before call. No callbacks, user delivery or peer wait.
int loom_retirement_close_operation(struct loom_service_operation *,u64 now);
// Ordinary terminal publication gate: a negative terminal may COMMIT while a
// MORE payload waits for CQ, but cannot be DELIVERED before that payload. Call
// provided_deliver first. Single positive pooled terminal is already delivered
// by its one combined CQE; never duplicate it here. Outside Ll, sole driver.
int loom_retirement_post_terminal(struct loom_service_operation *);
#endif
