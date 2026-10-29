// Internal staged operation. Caller owns all physical resources and authority.
#ifndef THYLACINE_LOOM_SERVICE_PROGRAM_H
#define THYLACINE_LOOM_SERVICE_PROGRAM_H
#include <thylacine/loom_service_wire.h>
#include <thylacine/proc.h>
enum loom_program_phase { LPROGRAM_NEW, LPROGRAM_STAT, LPROGRAM_EFFECT, LPROGRAM_DONE };
enum loom_program_action { LPROGRAM_TERMINAL, LPROGRAM_NEXT, LPROGRAM_ABORT };
struct loom_service_program {
    struct loom_service_wire wire;
    struct ProcAccessIdentity identity; // one admission-time creator value
    s32 result; // staged only: no terminal or FID publication implied
    u8 operation, phase;
};
// wire contains already resolved/pinned arguments and copied WALK component.
// kind is one of the five private effect T-types, never an arbitrary user step.
// The caller obtains identity from the actual creator/image snapshot for EACH
// newly admitted operation, not protocol->identity cached at CONNECT.
int loom_private_program_start(struct loom_service_program *,
                               const struct ProcAccessIdentity *,u8 kind);
// Sole driver, AFTER actual client return and capture, never from callback.
// Must be called once per completed attempt; the owner's callback-seen/phase
// guards that edge. NEXT retains the same request and physical borrows; driver
// must recheck scope/close/absolute deadline under Ll before requeue/submission.
// ABORT requires whole-scope abort, not merely one negative CQE. TERMINAL result
// remains tentative until the owner's abort/deadline and physical-drop gates.
// WALK result 0 means protocol success; driver publishes exact destination and
// returns its reserved slot only after the same-Ll final checks.
enum loom_program_action loom_private_program_reply(struct loom_service_program *,
                              const struct loom_service_reply *,int capture_error);
#endif
