// Internal SCOPE/FID reservation metadata; no protocol storage or user ABI.
#ifndef THYLACINE_LOOM_SERVICE_NODES_H
#define THYLACINE_LOOM_SERVICE_NODES_H
#include <thylacine/loom_service_slots.h>
struct loom_service_node {
    struct loom_service_ref parent; // zero for SCOPE, exact pinned SCOPE for FID
    u32 state;
    u32 reserved;
};
struct loom_service_node_table {
    struct loom_service_node nodes[LOOM_SERVICE_SLOT_COUNT];
};
_Static_assert(sizeof(struct loom_service_node) == 24, "scope/fid reservation ledger");
_Static_assert(sizeof(struct loom_service_node_table) == 1536, "scope/fid table ledger");
// Zero once with its owner. Live calls hold the SAME ring lock as the shared
// slot ledger and retain the ring. The zero-reference retire worker instead has
// exclusive ownership. No allocation, usercopy, callbacks or drops.
// A private Loom owns and charges a full buddy page for this table. Last-owner
// cleanup has exclusive access and may discard only unused RESERVED nodes.
// The engine alone advances state beyond RESERVED under that lock, after actual
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
// retired nodes require future engine cleanup and terminal-publication proof.
int loom_service_node_reap_unused(struct loom_slot_table *, struct loom_service_node_table *,
                                 struct loom_service_ref);
bool loom_service_nodes_empty(const struct loom_service_node_table *);
#endif
