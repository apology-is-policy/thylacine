// Real slot, node and FID modules; the scope's engine states are stand-ins.
// No protocol, wire traffic, allocator, usercopy or concurrent-lock claim.
#ifndef LOOM_FIDS_FIXTURE_H
#define LOOM_FIDS_FIXTURE_H
#include <thylacine/loom_service_fids.h>
#include <thylacine/errno.h>
#define LF_CHECK(c,msg) do { if (!(c)) return msg; } while (0)
static const char *loom_fids_fixture(void) {
    static struct loom_slot_table slots;
    static struct loom_service_node_table nodes;
    for (size_t i=0;i<sizeof(slots);++i) ((u8 *)&slots)[i]=0;
    for (size_t i=0;i<sizeof(nodes);++i) ((u8 *)&nodes)[i]=0;
    struct loom_service_ref zero={0},scope,walked,clunked,borrowed;
    struct loom_service_ref *fid[3]={&walked,&clunked,&borrowed};
    LF_CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_SCOPE,zero,&scope) &&
             !loom_service_node_publish(&slots,&nodes,scope), "scope reservation");
    nodes.nodes[scope.slot].state=LOOM_SERVICE_READY; // stand-in for CONNECT
    for (u32 i=0;i<3;++i)
        LF_CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_FID,scope,fid[i]) &&
                 !loom_service_node_publish(&slots,&nodes,*fid[i]) &&
                 !loom_fid_claim_walk(&slots,&nodes,*fid[i],scope,10+i) &&
                 !loom_fid_commit_walk(&slots,&nodes,*fid[i],scope,10+i,100+i) &&
                 !loom_fid_release_claim(&slots,&nodes,*fid[i],10+i), "three walked FIDs");
    LF_CHECK(slots.slots[scope.slot].pins==3, "each FID pins its scope");
    LF_CHECK(loom_fid_reap_retired(&slots,&nodes,walked)==-T_E_BUSY &&
             nodes.nodes[walked.slot].state==LOOM_SERVICE_READY, "a FID of a live scope refuses REAP");
    LF_CHECK(!loom_fid_begin_clunk(&slots,&nodes,clunked,scope,20) &&
             loom_fid_reap_retired(&slots,&nodes,clunked)==-T_E_BUSY, "a FID refuses REAP while its CLUNK runs");
    LF_CHECK(!loom_fid_finish_clunk(&slots,&nodes,clunked,20) &&
             !loom_fid_release_claim(&slots,&nodes,clunked,20), "the CLUNK completes");
    LF_CHECK(!loom_fid_reap_retired(&slots,&nodes,clunked) &&
             slots.slots[clunked.slot].phase==LSLOT_FREE && slots.slots[scope.slot].pins==2 &&
             loom_fid_reap_retired(&slots,&nodes,clunked)==-T_E_NOENT,
             "a clunked FID reaps once and unpins its scope");
    u32 wire=0;
    LF_CHECK(!loom_fid_borrow(&slots,&nodes,borrowed,scope,0,&wire) && wire==102, "a FID borrowed");
    nodes.nodes[scope.slot].state=LOOM_SERVICE_ABORTING; // stand-in for ABORT_SCOPE
    LF_CHECK(loom_fid_reap_retired(&slots,&nodes,walked)==-T_E_BUSY &&
             nodes.nodes[walked.slot].state==LOOM_SERVICE_READY &&
             slots.slots[walked.slot].phase==LSLOT_LIVE,
             "an aborting scope has not proven quiescence, and the refusal changes nothing");
    nodes.nodes[scope.slot].state=LOOM_SERVICE_RETIRED; // stand-in for local retirement
    LF_CHECK(loom_fid_reap_retired(&slots,&nodes,borrowed)==-T_E_BUSY &&
             nodes.nodes[borrowed.slot].state==LOOM_SERVICE_READY, "a borrowed FID refuses REAP");
    LF_CHECK(!loom_fid_put(&slots,&nodes,borrowed), "the borrow ends");
    LF_CHECK(!loom_slot_pin(&slots,walked,LOOM_SERVICE_FID), "a FID's metadata borrowed");
    LF_CHECK(loom_fid_reap_retired(&slots,&nodes,walked)==-T_E_BUSY &&
             nodes.nodes[walked.slot].state==LOOM_SERVICE_READY &&
             slots.slots[walked.slot].phase==LSLOT_LIVE,
             "a pinned FID refuses REAP before any transition");
    LF_CHECK(!loom_slot_unpin(&slots,walked), "the metadata borrow ends");
    LF_CHECK(!loom_fid_reap_retired(&slots,&nodes,walked) && !loom_fid_reap_retired(&slots,&nodes,borrowed) &&
             !slots.slots[scope.slot].pins && slots.slots[walked.slot].phase==LSLOT_FREE &&
             slots.slots[borrowed.slot].phase==LSLOT_FREE,
             "the FIDs of a retired scope reap and release it");
    return NULL;
}
#undef LF_CHECK
#endif
