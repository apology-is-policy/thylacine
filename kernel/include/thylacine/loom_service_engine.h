// Internal bounded owner index. No private syscall/worker activation.
#ifndef THYLACINE_LOOM_SERVICE_ENGINE_H
#define THYLACINE_LOOM_SERVICE_ENGINE_H
#include <thylacine/loom_service_abi.h>
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
// Returns 1 if a scope was visited, 0 if none eligible. Not a sleep/readiness
// predicate: the future executor must register native poll interest/recheck.
int loom_engine_visit(struct Loom *, u64 now);
// Removes only locally retired owners with delivered/discarded terminals and
// zero entry pins. Final drops outside Ll. Leaves scope metadata for REAP.
int loom_engine_collect(struct Loom *, struct loom_service_ref);
// Whole-ring-close consumer detach requires every indexed protocol locally
// retired, no entry/install/parser/fixed borrows. Then discard retained terminals
// and collect their storage, independent of CQ room. No native peer wait.
int loom_engine_discard_closed(struct Loom *);
// One close quantum: 0 blocked on external local holders, 1 immediate work,
// 2 complete (index/request/fixed ownership empty after consumer detach).
// Retirer owns a queue root; its internal puts never kick its own RUNNING work.
int loom_engine_close_visit(struct Loom *);
bool loom_engine_empty(const struct loom_service_engine *);
#endif
