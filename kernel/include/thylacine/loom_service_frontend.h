#ifndef THYLACINE_LOOM_SERVICE_FRONTEND_H
#define THYLACINE_LOOM_SERVICE_FRONTEND_H
#include <thylacine/types.h>
struct Loom;struct Proc;
// Internal only: no public ENTER/syscall activation. Caller retains Loom and its
// exact creator Proc in a stable syscall image. The worker never consumes SQEs.
// Returns number of claimed SQ positions, bounded by budget/SQ size, or negative
// authority/close error before any claim. A malformed SQ index counts as dropped.
// Copies each accepted SQE once, then prepares/publishes outside Ll. Publication
// transfers ownership; failures occupy one precharged record until CQ or close.
int loom_private_submit(struct Loom *,struct Proc *,u32 budget);
// One pending refusal delivery. Retained Loom required. 1 progress, 0 parked.
int loom_frontend_step(struct Loom *);
#endif
