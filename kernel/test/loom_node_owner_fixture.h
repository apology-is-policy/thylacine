// Owner transactions against real reservation modules. Caller provides a fresh
// retained private Loom and its creator. READY/ABORTING below are explicit test
// state injection, not evidence of a handshake, usercopy or concurrent schedule.
#ifndef LOOM_NODE_OWNER_FIXTURE_H
#define LOOM_NODE_OWNER_FIXTURE_H
static const char *loom_node_owner_fixture(struct Loom *l, struct Proc *p) {
    const char *error = NULL;
    struct loom_service_reserve in = { .size = sizeof(in),
        .version = LOOM_SERVICE_ABI_VERSION, .kind = LOOM_SERVICE_SCOPE };
    struct loom_service_ref ref = {0}, child = {0}, stale = {0};
    struct loom_service_ref out = { .slot = 999, .incarnation = 999 };
    struct loom_service_ref zero = {0};
    u64 creator = l->service_creator;
#define NO_CHECK(c,m) do { if (!(c)) { error = m; goto done; } } while (0)
    // Forged ring ownership exercises the incarnation gate independently of AS.
    l->service_creator ^= 1;
    int rc = loom_private_node_prepare(l,p,&in,&out);
    l->service_creator = creator;
    NO_CHECK(rc == -T_E_ACCES, "node owner requires exact creator incarnation");
    struct AddrSpace *image = l->service_as;
    l->service_as = (struct AddrSpace *)l; // distinct nonnull address, never dereferenced
    rc = loom_private_node_prepare(l,p,&in,&out);
    l->service_as = image;
    NO_CHECK(rc == -T_E_ACCES, "node owner requires exact creator image");
    for (u32 arm = 0; arm < 9; ++arm) {
        struct loom_service_reserve bad = in;
        switch (arm) {
        case 0: bad.size--; break;
        case 1: bad.version++; break;
        case 2: bad.flags = 1; break;
        case 3: bad.reserved = 1; break;
        case 4: bad.result.slot = 1; break;
        case 5: bad.result.reserved = 1; break;
        case 6: bad.result.incarnation = 1; break;
        case 7: bad.kind = LOOM_SERVICE_TARGET; break;
        default: bad.scope.incarnation = 1; break;
        }
        NO_CHECK(loom_private_node_prepare(l,p,&bad,&out) == -T_E_INVAL,
                 "node reserve rejects malformed immutable record");
    }
    NO_CHECK(out.slot == 999 && out.incarnation == 999 &&
             !l->service_slots->next_incarnation && loom_service_nodes_empty(l->service_nodes),
             "refusals preserve output and both ledgers");
    NO_CHECK(!loom_private_node_prepare(l,p,&in,&ref), "scope owner prepares");
    NO_CHECK(l->service_slots->slots[ref.slot].phase == LSLOT_PROVISIONAL,
             "copyout gap remains provisional");
    stale = ref;
    NO_CHECK(!loom_private_node_rollback(l,ref), "failed copyout rollback");
    NO_CHECK(!loom_private_node_prepare(l,p,&in,&ref) &&
             ref.incarnation > stale.incarnation, "copyout failure burns identity");
    NO_CHECK(loom_private_node_publish(l,stale) == -T_E_NOENT &&
             loom_private_node_rollback(l,stale) == -T_E_NOENT,
             "stale continuation cannot affect replacement");
    NO_CHECK(!loom_private_node_publish(l,ref), "scope owner publishes");
    in.kind = LOOM_SERVICE_FID; in.scope = ref;
    NO_CHECK(loom_private_node_prepare(l,p,&in,&out) == -T_E_NOENT,
             "reserved scope grants no child authority");
    spin_lock(&l->lock);
    l->service_nodes->nodes[ref.slot].state = LOOM_SERVICE_READY;
    spin_unlock(&l->lock);
    NO_CHECK(!loom_private_node_prepare(l,p,&in,&child) &&
             l->service_slots->slots[ref.slot].pins == 1, "child owner retains parent before copyout");
    spin_lock(&l->lock);
    l->service_nodes->nodes[ref.slot].state = LOOM_SERVICE_ABORTING;
    spin_unlock(&l->lock);
    NO_CHECK(loom_private_node_publish(l,child) == -T_E_CANCELED &&
             l->service_slots->slots[ref.slot].pins == 1, "parent abort wins publication gap");
    NO_CHECK(!loom_private_node_rollback(l,child) && !l->service_slots->slots[ref.slot].pins,
             "child rollback after abort releases parent");
    NO_CHECK(loom_private_node_reap_unused(l,ref) == -T_E_BUSY,
             "active scope requires engine retirement");
    spin_lock(&l->lock);
    l->service_nodes->nodes[ref.slot].state = LOOM_SERVICE_RESERVED;
    spin_unlock(&l->lock);
    NO_CHECK(!loom_private_node_reap_unused(l,ref), "unused scope owner reap");
    in.kind = LOOM_SERVICE_SCOPE; in.scope = zero;
    NO_CHECK(!loom_private_node_prepare(l,p,&in,&ref), "scope prepared before close");
    loom_exec_latch(l);
    NO_CHECK(loom_private_node_publish(l,ref) == -T_E_CANCELED &&
             loom_private_node_prepare(l,p,&in,&out) == -T_E_CANCELED,
             "close blocks scope reservation and copyout publication");
    NO_CHECK(!loom_private_node_rollback(l,ref) && loom_service_nodes_empty(l->service_nodes),
             "rollback progresses after owner close");
done:
    // Restore only this fixture's injected parent state on assertion failure;
    // owner teardown then removes unused children before parents normally.
    if (ref.incarnation && ref.slot < LOOM_SERVICE_SLOT_COUNT) {
        spin_lock(&l->lock);
        if (l->service_slots->slots[ref.slot].incarnation == ref.incarnation &&
            l->service_slots->slots[ref.slot].phase != LSLOT_FREE)
            l->service_nodes->nodes[ref.slot].state = LOOM_SERVICE_RESERVED;
        spin_unlock(&l->lock);
    }
#undef NO_CHECK
    return error;
}
#endif
