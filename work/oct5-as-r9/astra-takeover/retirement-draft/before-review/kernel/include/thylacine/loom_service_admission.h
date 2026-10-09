#ifndef THYLACINE_LOOM_SERVICE_ADMISSION_H
#define THYLACINE_LOOM_SERVICE_ADMISSION_H
#include <thylacine/loom_service_rpc.h>
// Internal transactions only; all hold Ll continuously and retain the Loom.
// Resolve exact source scope before charged owner construction. No wire/alloc.
int loom_admission_scope_locked(struct Loom *,struct loom_service_ref source,
                                struct loom_service_ref *out);
// Caller has prepared the program, canonical payload holds and copied component;
// WALK new_fid is from the actual monotone client allocator outside Ll. This
// supplies source access/claims and transfers the PREPARED owner to its list.
// A failed claim leaves the request PREPARED; no local CLUNK retirement leaks.
int loom_admission_enqueue_locked(struct loom_service_operation *,
    struct loom_service_ref source,struct loom_service_ref destination,u64 now);
// Begin additionally waits for earlier source borrowers before Tclunk.
int loom_admission_begin_locked(struct loom_service_operation *,u64 now);
// Sole driver after program completion and real payload/parser settlement.
// Under the same Ll hold, check final abort/deadline, publish FID/open, return
// its source/claim holds and finish the request. On refusal state is retained;
// actual wire effects may require scope abort, never a silent local rollback.
int loom_admission_commit_locked(struct loom_service_operation *,u64 now);
// Negative completion requires real callback/TX quiescence established by the
// driver before it enters SETTLING. CLUNK retirement after error additionally
// requires completed scope abort; abort_quiesced is this external proof, not
// merely the client dead bit. Drop canonical payload outside Ll BEFORE this.
int loom_admission_cancel_locked(struct loom_service_operation *,s32 reason,
                                 bool abort_quiesced,u64 now);
#endif
