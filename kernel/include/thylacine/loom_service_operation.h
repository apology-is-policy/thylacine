// Internal charged operation storage; no admission, submission or user dispatch.
#ifndef THYLACINE_LOOM_SERVICE_OPERATION_H
#define THYLACINE_LOOM_SERVICE_OPERATION_H
#include <thylacine/loom_service_protocol.h>
#include <thylacine/loom_service_program.h>
#include <thylacine/loom_service_fixed.h>
enum loom_rpc_phase { LRPC_NEW, LRPC_QUEUED, LRPC_INFLIGHT, LRPC_SETTLING, LRPC_TERMINAL, LRPC_RETIRED };
struct loom_service_operation {
    struct Loom *owner;
    struct loom_service_protocol *protocol; // exact engine entry pin
    struct loom_request_ref request;
    struct loom_service_ref scope;
    struct p9_rpc rpc;
    struct loom_service_program program;
    struct loom_service_reply reply;
    // Linked under Ll; sole protocol driver owns callback capture while driving.
    struct loom_service_operation *next, *prev;
    u32 phase;
    bool callback_seen;
    s32 capture_error;
    bool submitted, advanced;
    enum loom_program_action action;
    struct loom_service_ref source, destination;
    struct loom_service_ref storage_home; // the FID this page was reserved for, else zero
    u8 fid_claim; // 0 none, P9_TWALK / P9_TLOPEN / P9_TCLUNK
    bool source_borrow;
    struct loom_private_fixed fixed;
    bool fixed_dropping;
    struct loom_service_ref pool;
    struct loom_service_buffer_receipt receipt;
    bool pool_stream, pool_busy, pool_pending, pool_terminal, multishot, pool_more_delivered;
};
// Caller retains l; output unchanged on failure. Reserves a terminal and a
// physical-owner flag before releasing Ll for charge/allocation. Retains l and
// the exact engine protocol entry through all physical storage cleanup.
// Only an indexed READY scope of the retained creator image is eligible.
// This confers no fid/buffer/DAC authority and sends no protocol bytes.
// Successful storage is exclusively owned by the caller until future admission.
int loom_private_operation_prepare(struct Loom *, struct loom_service_ref,
    u64 user_data, u64 deadline, u64 now, struct loom_service_operation **out);
// The same, on the page the exact LIVE FID holds for its CLUNK: nothing is charged
// or allocated. -ENOENT if fid is no such slot, -EBUSY if its page is already out.
int loom_private_operation_prepare_clunk(struct Loom *, struct loom_service_ref scope,
    struct loom_service_ref fid, u64 user_data, u64 deadline, u64 now,
    struct loom_service_operation **out);
// Only unpublished PREPARED storage, with no RPC/buffer/callback escaping it.
// Consumes on success: storage first, then exact owner/pin release. Storage goes
// back to the FID it was reserved for while that FID lives, else physical free and
// charge refund. Caller must not race entries or use a pointer after this call.
int loom_private_operation_release_unused(struct loom_service_operation *);
// One charged, zeroed page for a FID's CLUNK, charged to the ring's image; NULL if
// the budget or the allocator refuses. free takes NULL. No lock held by the caller.
struct loom_service_operation *loom_private_clunk_storage_new(struct Loom *, bool exempt);
void loom_private_clunk_storage_free(struct Loom *, struct loom_service_operation *);
// Sole driver and a separate retained engine entry. Consumes only a delivered
// TERMINAL owner with all FID/payload users settled. Unlink/drop stream under Ll,
// then physical free/refund, exact request owner-drop/reap, engine/Loom last.
int loom_private_operation_release_retired(struct loom_service_operation *);
#ifdef KERNEL_TESTS
// One-shot actual allocation fault, consumed even by an early refusal.
void loom_private_operation_fault_arm_for_test(void);
bool loom_private_operation_fault_pending_for_test(void);
#endif
#endif
