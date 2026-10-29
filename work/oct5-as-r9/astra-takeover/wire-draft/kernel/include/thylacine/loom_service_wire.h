// Internal single-frame adapter. No user pointers, admission or DAC authority.
#ifndef THYLACINE_LOOM_SERVICE_WIRE_H
#define THYLACINE_LOOM_SERVICE_WIRE_H
#include <thylacine/9p_client.h>
#include <thylacine/loom_service_reply.h>
struct loom_service_wire {
    u32 fid, new_fid, mode, count, iounit;
    u64 offset;
    u32 payload_capacity;
    u16 component_len;
    u8 kind;
    u8 component[256]; // copied while the canonical WALK slice was retained
    u8 *payload; // retained resolved kernel extent; never shared SQE storage
    struct loom_service_reply_expect expect;
    bool built, local_again;
};
// Exactly one session_send call under the actual client's lock. kind is an
// internal program step: GETATTR precedes ordinary search/open DAC checks;
// selecting WALK/LOPEN here does NOT establish that authority. Caller retains
// exact FID claims, payload, creator image and absolute request deadline.
int loom_private_wire_build(struct p9_session *,u8 *,size_t,void *);
// Sole scope driver only, outside Ll. Callback/container MUST remain alive
// through this call and merely capture; it must never free its owner. Begin
// must arm the callback before entry. Like submit_async, invalid arguments can
// refuse without a callback; the driver must settle that local refusal itself.
int loom_private_wire_submit(struct p9_client *,struct p9_rpc *,struct loom_service_wire *);
// Only synchronous local submit refusal proves no bytes reached the peer.
// A later Rlerror(EAGAIN) is a terminal remote result, never a replay license.
// Caller additionally proves no sending/callback borrow and rechecks deadline,
// scope abort and program stage under Ll before requeueing the same obligation.
bool loom_private_wire_retry(const struct loom_service_wire *,int capture_error,
                             const struct loom_service_reply *);
#endif
