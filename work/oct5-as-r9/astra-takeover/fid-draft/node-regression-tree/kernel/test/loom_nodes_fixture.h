// Real reservation and slot modules; simulated parent READY/ABORTING only.
// No protocol, allocator, authority, usercopy or concurrent-lock claim.
#ifndef LOOM_NODES_FIXTURE_H
#define LOOM_NODES_FIXTURE_H
#include <thylacine/loom_service_nodes.h>
#include <thylacine/errno.h>
#define LN_CHECK(c,msg) do { if (!(c)) return msg; } while (0)
static const char *loom_nodes_fixture(void) {
    static struct loom_slot_table slots;
    static struct loom_service_node_table nodes;
    static struct loom_service_ref others[LOOM_SERVICE_SLOT_COUNT];
    for (size_t i=0;i<sizeof(slots);++i) ((u8 *)&slots)[i]=0;
    for (size_t i=0;i<sizeof(nodes);++i) ((u8 *)&nodes)[i]=0;
    struct loom_service_ref zero={0},scope,child,stale,second,failed={.slot=999,.incarnation=999};
    LN_CHECK(loom_service_nodes_empty(&nodes), "initial node table empty");
    LN_CHECK(loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_TARGET,zero,&failed)==-T_E_INVAL,
             "target kind refused by node reservation");
    LN_CHECK(loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_SCOPE,failed,&scope)==-T_E_INVAL,
             "scope parent must be zero");
    LN_CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_SCOPE,zero,&scope),
             "scope provisional reservation");
    LN_CHECK(loom_slot_pin(&slots,scope,LOOM_SERVICE_SCOPE)==-T_E_NOENT,
             "provisional scope cannot be borrowed");
    LN_CHECK(!loom_service_node_publish(&slots,&nodes,scope), "scope publication");
    LN_CHECK(loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_FID,scope,&failed)==-T_E_NOENT,
             "unused scope cannot authorize child");
    // Stand-in for the future engine's successful CONNECT linearization. This
    // assignment is deliberately NOT a production API or protocol witness.
    nodes.nodes[scope.slot].state=LOOM_SERVICE_READY;
    LN_CHECK(loom_service_node_reap_unused(&slots,&nodes,scope)==-T_E_BUSY,
             "ready scope cannot use unused reap");
    LN_CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_FID,scope,&child) &&
             slots.slots[scope.slot].pins==1, "provisional child retains parent");
    LN_CHECK(loom_slot_pin(&slots,child,LOOM_SERVICE_FID)==-T_E_NOENT,
             "provisional child cannot be borrowed");
    stale=child;
    LN_CHECK(!loom_service_node_rollback(&slots,&nodes,child) &&
             !slots.slots[scope.slot].pins && slots.slots[child.slot].phase==LSLOT_FREE &&
             nodes.nodes[child.slot].state==LOOM_SERVICE_EMPTY,
             "child rollback releases parent and both ledgers");
    LN_CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_FID,scope,&child) &&
             child.incarnation>stale.incarnation && child.slot==stale.slot &&
             !loom_service_node_publish(&slots,&nodes,child), "child reuses slot with fresh identity");
    LN_CHECK(loom_service_node_reap_unused(&slots,&nodes,stale)==-T_E_NOENT &&
             slots.slots[scope.slot].pins==1, "stale child cannot release replacement parent");
    LN_CHECK(!loom_slot_pin(&slots,child,LOOM_SERVICE_FID), "child metadata borrowed");
    LN_CHECK(loom_service_node_reap_unused(&slots,&nodes,child)==-T_E_BUSY &&
             slots.slots[scope.slot].pins==1 && nodes.nodes[child.slot].state==LOOM_SERVICE_RESERVED,
             "borrowed unused child cannot be reaped");
    LN_CHECK(!loom_slot_unpin(&slots,child), "child borrow released");
    // Root metadata borrowers include children even if its engine changes state.
    nodes.nodes[scope.slot].state=LOOM_SERVICE_RESERVED;
    LN_CHECK(loom_service_node_reap_unused(&slots,&nodes,scope)==-T_E_BUSY,
             "child pin prevents parent reuse");
    nodes.nodes[scope.slot].state=LOOM_SERVICE_READY;
    for (u32 i=0;i<LOOM_SERVICE_SLOT_COUNT-2;++i)
        LN_CHECK(!loom_slot_reserve(&slots,LOOM_SERVICE_POOL,&others[i]) &&
                 !loom_slot_publish(&slots,others[i]), "nodes share pool capacity");
    u64 generation=slots.next_incarnation;
    LN_CHECK(loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_FID,scope,&failed)==-T_E_NOSPC &&
             slots.slots[scope.slot].pins==1 && generation==slots.next_incarnation &&
             failed.slot==999 && failed.incarnation==999, "quota refusal restores parent and output");
    for (u32 i=0;i<LOOM_SERVICE_SLOT_COUNT-2;++i)
        LN_CHECK(!loom_slot_retire(&slots,others[i]) && !loom_slot_reap(&slots,others[i]), "pool cleanup");
    LN_CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_FID,scope,&second),
             "provisional child before parent abort");
    nodes.nodes[scope.slot].state=LOOM_SERVICE_ABORTING;
    LN_CHECK(loom_service_node_publish(&slots,&nodes,second)==-T_E_CANCELED &&
             slots.slots[second.slot].phase==LSLOT_PROVISIONAL && slots.slots[scope.slot].pins==2,
             "parent abort wins child publication without losing pins");
    LN_CHECK(loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_FID,scope,&failed)==-T_E_NOENT,
             "aborting parent refuses new child");
    LN_CHECK(!loom_service_node_rollback(&slots,&nodes,second) && slots.slots[scope.slot].pins==1,
             "rollback after abort releases parent");
    loom_slots_close(&slots);
    LN_CHECK(!loom_service_node_reap_unused(&slots,&nodes,child) && !slots.slots[scope.slot].pins &&
             slots.slots[child.slot].phase==LSLOT_FREE && nodes.nodes[child.slot].state==LOOM_SERVICE_EMPTY,
             "closed unused child cleanup releases parent");
    LN_CHECK(loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_SCOPE,zero,&failed)==-T_E_CANCELED,
             "close refuses node reservation");
    // The real engine must handle ABORTING/RETIRED, with local-resource and
    // terminal-publication proofs; this module must refuse to stand in for it.
    LN_CHECK(loom_service_node_reap_unused(&slots,&nodes,scope)==-T_E_BUSY,
             "aborting scope requires engine retirement");
    nodes.nodes[scope.slot].state=LOOM_SERVICE_RETIRED;
    LN_CHECK(loom_service_node_reap_unused(&slots,&nodes,scope)==-T_E_BUSY,
             "retired scope requires engine retirement");
    nodes.nodes[scope.slot].state=LOOM_SERVICE_RESERVED;
    stale=scope;
    LN_CHECK(!loom_service_node_reap_unused(&slots,&nodes,scope) && loom_service_nodes_empty(&nodes),
             "unused scope cleanup empties node table");
    // A fresh ring exercises close versus the separate copyout continuation.
    for (size_t i=0;i<sizeof(slots);++i) ((u8 *)&slots)[i]=0;
    LN_CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_SCOPE,zero,&scope),
             "fresh scope before close");
    loom_slots_close(&slots);
    LN_CHECK(loom_service_node_publish(&slots,&nodes,scope)==-T_E_CANCELED &&
             !loom_service_node_rollback(&slots,&nodes,scope) && loom_service_nodes_empty(&nodes),
             "close wins scope publication and rollback progresses");
    (void)stale;
    return NULL;
}
#undef LN_CHECK
#endif
