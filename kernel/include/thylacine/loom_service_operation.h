// Internal charged operation storage; no admission, submission or user dispatch.
#ifndef THYLACINE_LOOM_SERVICE_OPERATION_H
#define THYLACINE_LOOM_SERVICE_OPERATION_H
#include <thylacine/loom_service_protocol.h>
#include <thylacine/loom_service_reply.h>
struct loom_service_operation {
    struct Loom *owner;
    struct loom_service_protocol *protocol; // exact engine entry pin
    struct loom_request_ref request;
    struct loom_service_ref scope;
    struct p9_rpc rpc;
    struct loom_service_reply_expect expect;
    struct loom_service_reply reply;
    u8 component[256];
};
// Caller retains l; output unchanged on failure. Reserves a terminal and a
// physical-owner flag before releasing Ll for charge/allocation. Retains l and
// the exact engine protocol entry through all physical storage cleanup.
// Only an indexed READY scope of the retained creator image is eligible.
// This confers no fid/buffer/DAC authority and sends no protocol bytes.
// Successful storage is exclusively owned by the caller until future admission.
int loom_private_operation_prepare(struct Loom *, struct loom_service_ref,
    u64 user_data, u64 deadline, u64 now, struct loom_service_operation **out);
// Only unpublished PREPARED storage, with no RPC/buffer/callback escaping it.
// Consumes on success: physical free then charge refund then exact owner/pin
// release. Caller must not race entries or use a pointer after this call.
int loom_private_operation_release_unused(struct loom_service_operation *);
#ifdef KERNEL_TESTS
// One-shot actual allocation fault, consumed even by an early refusal.
void loom_private_operation_fault_arm_for_test(void);
bool loom_private_operation_fault_pending_for_test(void);
#endif
#endif
