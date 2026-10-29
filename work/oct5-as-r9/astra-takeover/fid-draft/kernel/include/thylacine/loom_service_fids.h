// Internal protocol mapping and exact WALK destination claims, not a user ABI.
#ifndef THYLACINE_LOOM_SERVICE_FIDS_H
#define THYLACINE_LOOM_SERVICE_FIDS_H
#include <thylacine/loom_service_nodes.h>
enum { LFID_BOUND=1, LFID_OPEN=2, LFID_OPENING=4, LFID_CLUNKING=8, LFID_READ=1, LFID_WRITE=2 };
// All calls hold the shared Loom lock and retain the ring. No allocator,
// callback, protocol I/O, usercopy or final drop. Engine MUST supply successful
// protocol and DAC proof before binding/open publication, with cancellation and
// absolute deadlines rechecked in that same lock interval. The wire fid comes
// from the retained private client's monotone allocator, never a user field.
// Parent identity is exact; wire-fid uniqueness is checked within one scope.
int loom_fid_bind_root(struct loom_slot_table *,struct loom_service_node_table *,
                      struct loom_service_ref scope,u32 wire_fid);
// Claims exactly one pre-reserved LIVE FID destination. A nonzero kernel request
// identity owns the destination pin across unlocked work; no pointer authority.
int loom_fid_claim_walk(struct loom_slot_table *,struct loom_service_node_table *,
    struct loom_service_ref dest,struct loom_service_ref scope,u64 request_id);
int loom_fid_commit_walk(struct loom_slot_table *,struct loom_service_node_table *,
    struct loom_service_ref dest,struct loom_service_ref scope,u64 request_id,u32 wire_fid);
// Drop only after the actual operation has relinquished all destination users.
// Works after parent abort/close; committed mapping stays bound, failed claim
// remains RESERVED for ordinary unused reap. Does not undo remote wire effects.
int loom_fid_release_claim(struct loom_slot_table *,struct loom_service_node_table *,
    struct loom_service_ref dest,u64 request_id);
// need=0 for metadata/WALK, READ/WRITE for actual data access. Resolves ONLY an
// already committed mapping, retaining source slot; output unchanged on refusal.
// Caller still owes fresh ordinary stat/DAC/search proof where required.
int loom_fid_borrow(struct loom_slot_table *,struct loom_service_node_table *,
    struct loom_service_ref source,struct loom_service_ref scope,u32 need,u32 *wire_fid);
// Ordinary LOPEN mode0/1/2; owner supplies successful stat/DAC/protocol proof.
// Requires its exact exclusive open claim, retained through physical cleanup.
int loom_fid_commit_open(struct loom_slot_table *,struct loom_service_node_table *,
    struct loom_service_ref source,struct loom_service_ref scope,u64 request_id,u32 mode);
// Put an existing source borrow after close/retire; never substitute a raw unpin.
int loom_fid_put(struct loom_slot_table *,struct loom_service_node_table *,struct loom_service_ref);
// Claim before any unlocked stat/open work; bars all new borrows. Release with
// release_claim after actual users stop, on either failure or committed success.
int loom_fid_claim_open(struct loom_slot_table *,struct loom_service_node_table *,
    struct loom_service_ref source,struct loom_service_ref scope,u64 request_id);
// Derived only. Closes admission immediately; earlier borrows may still drain.
// clunk_wire returns BUSY until they do. Caller separately checks scope/deadline
// before protocol submission. finish requires actual Tclunk success OR completed
// scope-abort callback quiescence, never simply a terminal CQ record.
int loom_fid_begin_clunk(struct loom_slot_table *,struct loom_service_node_table *,
    struct loom_service_ref source,struct loom_service_ref scope,u64 request_id);
int loom_fid_clunk_wire(struct loom_slot_table *,struct loom_service_node_table *,
    struct loom_service_ref source,u64 request_id,u32 *wire_fid);
int loom_fid_finish_clunk(struct loom_slot_table *,struct loom_service_node_table *,
    struct loom_service_ref source,u64 request_id);
// Engine proves protocol abort quiescence first. This is local retirement; it
// never waits for a remote Tclunk and never implies payload leases were returned.
int loom_fid_retire_aborted(struct loom_slot_table *,struct loom_service_node_table *,
    struct loom_service_ref source);
int loom_fid_reap(struct loom_slot_table *,struct loom_service_node_table *,struct loom_service_ref);
#endif
