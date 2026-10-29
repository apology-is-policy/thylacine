// Internal SCOPE/FID reservation metadata; no protocol storage or user ABI.
#ifndef THYLACINE_LOOM_SERVICE_NODES_H
#define THYLACINE_LOOM_SERVICE_NODES_H
#include <thylacine/loom_service_slots.h>
struct loom_service_node {
    struct loom_service_ref parent; // zero for SCOPE, exact pinned SCOPE for FID
    u32 state;
    u32 reserved;
    s32 reason; // first scope abort reason, retained after protocol collection
    u32 status_flags; // completed-driver diagnostics only; protected by Ll
    u32 failure_phase;
    u32 diagnostic_reserved;
    // Internal protocol mapping. The engine supplies actual handshake/WALK/DAC
    // proof; these fields alone confer no authority to send an RPC.
    u64 fid_claim; // exact ring request identity; owns one destination slot pin
    u32 wire_fid, fid_access, fid_flags, fid_borrows;
};
struct loom_service_node_table {
    struct loom_service_node nodes[LOOM_SERVICE_SLOT_COUNT];
};
_Static_assert(sizeof(struct loom_service_node) == 64, "scope/fid reservation ledger");
_Static_assert(sizeof(struct loom_service_node_table) == 4096, "scope/fid table ledger");
// Zero once with its owner. Live calls hold the SAME ring lock as the shared
// slot ledger and retain the ring. The zero-reference retire worker instead has
// exclusive ownership. No allocation, usercopy, callbacks or drops.
// A private Loom owns and charges a full buddy page for this table. Last-owner
// cleanup has exclusive access. It removes unused RESERVED children first,
// then validates and removes protocol-RETIRED unpinned scopes in loom.c.
// The engine and authorized controls advance state under that lock after actual
// protocol/lifetime checks. Tests may simulate READY/ABORTING; this module does
// not provide a way to claim a handshake succeeded or a scope locally retired.
int loom_service_node_prepare(struct loom_slot_table *, struct loom_service_node_table *,
                             u32 kind, struct loom_service_ref parent,
                             struct loom_service_ref *out);
int loom_service_node_publish(struct loom_slot_table *, struct loom_service_node_table *,
                             struct loom_service_ref);
int loom_service_node_rollback(struct loom_slot_table *, struct loom_service_node_table *,
                              struct loom_service_ref);
// Only an unused RESERVED node may be reaped here. Admitted/ready/aborting/
// retired nodes require engine cleanup and terminal-publication proof.
int loom_service_node_reap_unused(struct loom_slot_table *, struct loom_service_node_table *,
                                 struct loom_service_ref);
bool loom_service_nodes_empty(const struct loom_service_node_table *);
#endif
