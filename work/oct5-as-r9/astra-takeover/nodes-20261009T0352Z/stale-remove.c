// Shared quota + parent retention for unused scope/fid reservations. Under the
// owner lock, a FID pins its exact READY parent BEFORE reserving a shared slot.
// A provisional child already keeps that parent from being reaped. Publication
// rechecks parent admission: abort can win during unlocked usercopy. All failure
// paths preserve caller output and unwind the parent pin; incarnation burns are
// never rewound. Rollback/reap validate every refusal before changing either
// ledger; parent unpin remains legal after close/abort. This metadata module
// proves neither protocol state nor actual allocator/refcount ownership. Owner
// charging, usercopy, CONNECT/WALK and local retirement remain engine obligations.
#include <thylacine/loom_service_nodes.h>
#include <thylacine/errno.h>

static struct loom_service_node *node_exact(struct loom_slot_table *t,
                         struct loom_service_node_table *nodes,
                         struct loom_service_ref ref, u32 phase) {
    if (!t || !nodes || ref.reserved || !ref.incarnation ||
        ref.slot >= LOOM_SERVICE_SLOT_COUNT) return NULL;
    struct loom_service_slot *s = &t->slots[ref.slot];
    if (s->phase != phase ||
        (s->kind != LOOM_SERVICE_SCOPE && s->kind != LOOM_SERVICE_FID)) return NULL;
    return &nodes->nodes[ref.slot];
}
static bool node_zero_ref(struct loom_service_ref ref) {
    return !ref.slot && !ref.reserved && !ref.incarnation;
}
static bool node_parent_held(struct loom_slot_table *t, struct loom_service_ref ref) {
    if (ref.reserved || !ref.incarnation || ref.slot >= LOOM_SERVICE_SLOT_COUNT)
        return false;
    struct loom_service_slot *s = &t->slots[ref.slot];
    return s->incarnation == ref.incarnation && s->kind == LOOM_SERVICE_SCOPE &&
           (s->phase == LSLOT_LIVE || s->phase == LSLOT_RETIRING) && s->pins;
}
static bool node_parent_ready(struct loom_slot_table *t,
                              struct loom_service_node_table *nodes,
                              struct loom_service_ref ref) {
    struct loom_service_node *p = node_exact(t, nodes, ref, LSLOT_LIVE);
    return p && t->slots[ref.slot].kind == LOOM_SERVICE_SCOPE &&
           p->state == LOOM_SERVICE_READY;
}
int loom_service_node_prepare(struct loom_slot_table *t,
                              struct loom_service_node_table *nodes,
                              u32 kind, struct loom_service_ref parent,
                              struct loom_service_ref *out) {
    if (!t || !nodes || !out ||
        (kind != LOOM_SERVICE_SCOPE && kind != LOOM_SERVICE_FID)) return -T_E_INVAL;
    if (kind == LOOM_SERVICE_SCOPE && !node_zero_ref(parent)) return -T_E_INVAL;
    if (t->closing) return -T_E_CANCELED;
    if (kind == LOOM_SERVICE_FID) {
        if (!node_parent_ready(t, nodes, parent)) return -T_E_NOENT;
        int rc = loom_slot_pin(t, parent, LOOM_SERVICE_SCOPE);
        if (rc) return rc;
    }
    struct loom_service_ref ref;
    int rc = loom_slot_reserve(t, kind, &ref);
    if (!rc && nodes->nodes[ref.slot].state != LOOM_SERVICE_EMPTY) {
        // A caller must not mix raw reservations with unretired node payloads.
        (void)loom_slot_rollback(t, ref);
        rc = -T_E_BUSY;
    }
    if (rc) {
        if (kind == LOOM_SERVICE_FID) (void)loom_slot_unpin(t, parent);
        return rc;
    }
    nodes->nodes[ref.slot] = (struct loom_service_node){
        .parent = parent, .state = LOOM_SERVICE_RESERVED };
    *out = ref;
    return 0;
}
int loom_service_node_publish(struct loom_slot_table *t,
                              struct loom_service_node_table *nodes,
                              struct loom_service_ref ref) {
    struct loom_service_node *n = node_exact(t, nodes, ref, LSLOT_PROVISIONAL);
    if (!n || n->state != LOOM_SERVICE_RESERVED) return -T_E_NOENT;
    if (t->slots[ref.slot].kind == LOOM_SERVICE_FID &&
        !node_parent_ready(t, nodes, n->parent)) return -T_E_CANCELED;
    return loom_slot_publish(t, ref);
}
static int node_remove(struct loom_slot_table *t, struct loom_service_node_table *nodes,
                       struct loom_service_ref ref, bool rollback) {
    struct loom_service_node *n = node_exact(t, nodes, ref,
                                    rollback ? LSLOT_PROVISIONAL : LSLOT_LIVE);
    if (!n) return -T_E_NOENT;
    if (n->state != LOOM_SERVICE_RESERVED || t->slots[ref.slot].pins) return -T_E_BUSY;
    bool child = t->slots[ref.slot].kind == LOOM_SERVICE_FID;
    struct loom_service_ref parent = n->parent;
    if (child ? !node_parent_held(t, parent) : !node_zero_ref(parent)) return -T_E_INVAL;
    // Phase, incarnation and pins are stable under the common lock. These slot
    // transitions cannot fail after validation; no freed identity is reused in
    // between, and the parent remains retained until the child payload is gone.
    *n = (struct loom_service_node){0};
    if (rollback) (void)loom_slot_rollback(t, ref);
    else {
        (void)loom_slot_retire(t, ref);
        (void)loom_slot_reap(t, ref);
    }
    if (child) (void)loom_slot_unpin(t, parent);
    return 0;
}
int loom_service_node_rollback(struct loom_slot_table *t,
                               struct loom_service_node_table *nodes,
                               struct loom_service_ref ref) {
    return node_remove(t, nodes, ref, true);
}
int loom_service_node_reap_unused(struct loom_slot_table *t,
                                  struct loom_service_node_table *nodes,
                                  struct loom_service_ref ref) {
    return node_remove(t, nodes, ref, false);
}
bool loom_service_nodes_empty(const struct loom_service_node_table *nodes) {
    if (!nodes) return false;
    for (u32 i = 0; i < LOOM_SERVICE_SLOT_COUNT; ++i)
        if (nodes->nodes[i].state != LOOM_SERVICE_EMPTY ||
            !node_zero_ref(nodes->nodes[i].parent) || nodes->nodes[i].reserved) return false;
    return true;
}
