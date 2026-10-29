#ifndef THYLACINE_LOOM_SERVICE_REPLY_H
#define THYLACINE_LOOM_SERVICE_REPLY_H
#include <thylacine/9p_session.h>
// Pointer-free callback capture. It is NOT a committed result or usable fid.
struct loom_service_reply {
    s32 result;
    u32 iounit;
    struct p9_qid qid;
    struct p9_attr attr;
};
struct loom_service_reply_expect {
    u32 fid, new_fid, count;
    u64 attr_mask;
    u8 kind;
    u16 walk_names; // private WALK is clone0 or one component1
};
// Called by the sole scope driver while its parser/buffer borrows remain held.
// Payload destination is a resolved, pre-reserved kernel alias of a fixed slice
// or BUSY pool member; capacity was checked against count before submission.
// No locks, allocation, usercopy, callback reentry, final drops or CQ publication.
// Negative return means malformed/inconsistent completion: owner must abort the
// scope. Negative status is captured without reading the possibly invalid dr.
// Caller commits only after rechecking cancellation/deadline under Loom lock;
// a captured success cannot authorize a destination after abort won that lock.
int loom_private_reply_capture(const struct loom_service_reply_expect *, int,
    const struct p9_dispatch_result *, u8 *payload, u32 capacity,
    struct loom_service_reply *out);
#endif
