// Charged prepublication protocol storage. No user ABI or private activation.
#ifndef THYLACINE_LOOM_SERVICE_PROTOCOL_H
#define THYLACINE_LOOM_SERVICE_PROTOCOL_H
#include <thylacine/9p_client.h>
#include <thylacine/devsrv.h>
#include <thylacine/loom_service_requests.h>
#include <thylacine/proc.h>

struct Loom;
struct loom_service_operation;
struct loom_service_protocol {
    struct Loom *owner;
    struct ProcServiceIdentity identity;
    struct SrvConnectAdmission admission;
    struct p9_client client;
    struct p9_client_progress progress;
    u8 *recv_storage;
    u8 *send_storage;
    u32 charged_pages;
    // CONNECT engine owns these under owner->lock. A future executor must
    // supply the entry/lifetime root; raw pointers do not retain this object.
    struct loom_service_ref scope;
    struct loom_request_ref request;
    u64 connect_deadline;
    s32 abort_reason;
    u32 users;
    u32 failure_phase; // handshake phase at protocol failure, before abort overwrites it
    bool connect_bound, driving, locally_retired;
    bool rpc_quiesced; // set only after actual progress_abort returns under sole driver
    struct loom_service_operation *op_head, *op_tail;
    u32 op_count; // bounded by the ring terminal capacity, no separate allocation
    bool indexed; // engine index owns lifetime; only its collector clears this
};

// Caller retains l. Resolves an exact registered target and snapshots the actual
// creator's incarnation + image, never a worker's identity. Prepares a fresh
// exclusive native connection, reserves the complete buddy charge, and binds a
// preallocated progress client. Queues Tversion locally but performs NO I/O or
// backlog publication. Output stays untouched on error. Deadline is absolute.
//
// This is an internal construction primitive, not CONNECT admission. Its future
// scope owner must reserve the request/terminal and destination first, then
// recheck deadline/abort and publish under the Loom lock. Until that integration
// publishes it, only unused release is legal. The CONNECT owner provides the
// separate published-retirement path. No callbacks, client progress, publication
// or concurrent users may escape an unbound prepared owner. One caller owns it exclusively; no refcount is implied here.
int loom_private_protocol_prepare(struct Loom *l, struct loom_service_ref target,
                                  u64 deadline, u64 now,
                                  struct loom_service_protocol **out);
// Refuses a published connection unchanged. Unpublished cleanup aborts the
// local cursor, releases native ownership and frees all storage before refund
// against the retained image. The Loom reference is dropped LAST.
int loom_private_protocol_release_unused(struct loom_service_protocol *p);

// Internal final drop after CONNECT has unbound its exact ledger pins and
// proved local quiescence. Never turns a merely torn transport into that proof.
int loom_private_protocol_release_retired(struct loom_service_protocol *p);

#ifdef KERNEL_TESTS
// Consume once, even when an earlier validation refuses. 1=owner,2=recv,3=send.
void loom_private_protocol_fault_arm_for_test(u32 stage);
u32 loom_private_protocol_fault_pending_for_test(void);
#endif
#endif
