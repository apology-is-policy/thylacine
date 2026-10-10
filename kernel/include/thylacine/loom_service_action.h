#ifndef THYLACINE_LOOM_SERVICE_ACTION_H
#define THYLACINE_LOOM_SERVICE_ACTION_H
#include <thylacine/loom_service_ready.h>
// Outside Ll, caller retains a SEPARATE engine entry pin and Loom root across
// this entire visit, including after an action consumes its operation owner.
// One atomic selector claim, one bounded action, release driving and notify.
// 1 = transition/progress, 0 = contention/backpressure, negative = invariant or
// identity refusal requiring caller diagnosis; never report such a refusal as
// a successful visit. Deadlines are sampled afresh at each committing edge.
int loom_ready_step(struct loom_service_protocol *,u64 now,bool native_ready);
#endif
