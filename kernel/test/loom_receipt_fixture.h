// Shared native/host fixture for actual Loom receipt geometry and paired
// publication. The pool lives in a real slot/pool table, built through the
// table transactions, and is published by loom_provided_deliver -- the one
// live publication path -- from the state loom_provided_commit_locked leaves
// for a MORE shot. The tables are this fixture's own storage, lent to a public
// receipt ring for the checks and taken back before it is freed; registration
// and creator authority are separate private-owner integration obligations.
#ifndef LOOM_RECEIPT_FIXTURE_H
#define LOOM_RECEIPT_FIXTURE_H
#include <thylacine/loom_service_pool_table.h>
#include <thylacine/loom_service_operation.h>
#include <thylacine/loom_service_provided.h>
#include <thylacine/errno.h>
#define RC_CHECK(x,msg) do { if (!(x)) { error=msg; goto done; } } while (0)
static struct loom_slot_table rc_slots;
static struct loom_service_pool_table rc_pools;
static struct loom_service_operation rc_op;
static u8 rc_payload[64];
static void rc_zero(void *p,size_t n) { for(size_t i=0;i<n;i++) ((u8 *)p)[i]=0; }
static bool rc_receipt_equal(struct loom_service_buffer_receipt a,
                              struct loom_service_buffer_receipt b) {
    return a.pool.slot==b.pool.slot && a.pool.reserved==b.pool.reserved &&
           a.pool.incarnation==b.pool.incarnation && a.member==b.member &&
           a.reserved==b.reserved && a.lease==b.lease;
}
static const char *loom_receipt_fixture(void) {
    const char *error=NULL;
    struct Loom *l=NULL,*legacy=NULL,*large=NULL;
    rc_zero(&rc_slots,sizeof(rc_slots));rc_zero(&rc_pools,sizeof(rc_pools));
    rc_zero(&rc_op,sizeof(rc_op));
    l=loom_create_with_receipts(1,1,true);
    legacy=loom_create(1,1,true);
    RC_CHECK(l&&legacy,"receipt rings allocated");
    RC_CHECK(!legacy->receipt_off&&!legacy->receipt_size,"legacy receipt geometry absent");
    RC_CHECK(l->receipt_size==32&&!(l->receipt_off&63)&&
             l->receipt_off>=l->cqe_off+l->cqe_size&&
             l->receipt_off+l->receipt_size<=l->ring_size,"receipt geometry bounded aligned");
    // An incarnation past 32 bits must reach the receipt whole.
    rc_slots.next_incarnation=0x100000000ULL;
    struct loom_pool_extent extent={.backing=rc_payload,.offset=0,.length=sizeof(rc_payload)};
    struct loom_service_ref ref;
    RC_CHECK(!loom_service_pool_prepare(&rc_slots,&rc_pools,&extent,1,&ref)&&
             !loom_service_pool_publish(&rc_slots,&rc_pools,ref)&&
             ref.incarnation==0x100000001ULL,"receipt pool prepared");
    struct loom_pool_bank *bank=&rc_pools.bank;
    struct loom_pool *pool=&rc_pools.pools[ref.slot];
    RC_CHECK(!loom_pool_stream_get(pool),"receipt stream retained");
    struct loom_service_buffer_receipt receipt;
    RC_CHECK(!loom_pool_claim(bank,pool,4,&receipt),"receipt member claimed");
    rc_payload[0]=0x71;rc_payload[1]=0x72;rc_payload[2]=0x73;rc_payload[3]=0x74;
    RC_CHECK(!loom_pool_commit(bank,pool,&receipt,4,0x8877665544332211ULL,true),"receipt result committed");
    // Field by field: the operation is too large for a stack temporary.
    rc_op.owner=legacy;rc_op.pool=ref;rc_op.receipt=receipt;rc_op.phase=LRPC_SETTLING;
    rc_op.pool_stream=true;rc_op.pool_pending=true;rc_op.multishot=true;
    RC_CHECK(loom_provided_deliver(&rc_op)==-T_E_NOENT,"legacy ring without a pool table refuses publication");
    rc_op.owner=l;l->service_slots=&rc_slots;l->service_pools=&rc_pools;
    RC_CHECK(!loom_post_cqe(l,9,0,0),"fill CQ before pending payload");
    RC_CHECK(loom_provided_deliver(&rc_op)==-T_E_AGAIN,"full CQ retains pending payload");
    RC_CHECK(bank->cells[0].phase==LMEMBER_PENDING&&rc_op.pool_pending,"CQ backpressure cannot lease or free member");
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    struct loom_cqe *cq=(void *)(l->ring_kva+l->cqe_off);
    struct loom_service_buffer_receipt *r=(void *)(l->ring_kva+l->receipt_off);
    __atomic_store_n(&h->cq_head,1,__ATOMIC_RELEASE);
    // Hostile shared mirrors never select a write address or mint a receipt.
    h->cq_tail=0xffffffffu;h->cq_mask=0xffffffffu;r[0].lease=0xdead;
    // A detached consumer gets nothing, even with CQ room: the refusal writes no
    // slot, no mirror and no ledger.
    l->service_requests.detached=true;
    RC_CHECK(loom_provided_deliver(&rc_op)==-T_E_CANCELED,"detached consumer refuses publication");
    RC_CHECK(bank->cells[0].phase==LMEMBER_PENDING&&rc_op.pool_pending&&l->cq_tail==1&&
             h->cq_tail==0xffffffffu&&r[0].lease==0xdead&&cq[0].user_data==9,"detached refusal writes nothing");
    l->service_requests.detached=false;
#ifdef LOOM_RECEIPT_HOST_OBSERVE
    rc_watch(l,bank,receipt);
#endif
    RC_CHECK(!loom_provided_deliver(&rc_op),"paired payload published");
    RC_CHECK(cq[0].user_data==0x8877665544332211ULL&&cq[0].result==4&&
             cq[0].flags==(LOOM_CQE_MORE|LOOM_CQE_SERVICE_BUFFER),"paired CQE exact");
    RC_CHECK(rc_receipt_equal(r[0],receipt)&&h->cq_tail==2,"paired receipt exact");
    RC_CHECK(bank->cells[0].phase==LMEMBER_LEASED&&!rc_op.pool_pending&&rc_op.pool_more_delivered,
             "published payload leased");
    __atomic_store_n(&h->cq_head,2,__ATOMIC_RELEASE);
    RC_CHECK(loom_pool_claim(bank,pool,4,&r[0])==-T_E_AGAIN,"CQ acknowledgement cannot return payload");
    RC_CHECK(rc_payload[0]==0x71&&rc_payload[3]==0x74,"leased payload intact");
    RC_CHECK(!loom_post_cqe(l,12,-T_E_CANCELED,0),"terminal CQE published");
    struct loom_service_buffer_receipt zero={0};
    RC_CHECK(rc_receipt_equal(r[0],zero),"nonleased CQE clears old receipt");
    RC_CHECK(bank->cells[0].phase==LMEMBER_LEASED,"terminal does not return old payload");
    RC_CHECK(!loom_pool_return(bank,pool,&receipt),"exact explicit payload return");
    rc_op.pool_stream=false;
    RC_CHECK(!loom_pool_stream_put(pool)&&!loom_service_pool_reap(&rc_slots,&rc_pools,ref)&&
             loom_service_pools_empty(&rc_pools),"returned pool reaped");
    __atomic_store_n(&h->cq_head,3,__ATOMIC_RELEASE);
    RC_CHECK(loom_post_cqe(l,1,1,LOOM_CQE_SERVICE_BUFFER)<0,"untyped publication cannot mint buffer flag");
    // u32 CQ wrap remains bounded by private tail, not corrupt shared mirrors.
    l->cq_tail=0xffffffffu;h->cq_head=0xffffffffu;
    RC_CHECK(!loom_post_cqe(l,17,0,0)&&l->cq_tail==0&&h->cq_tail==0,"CQ counter wraps safely");
    h->cq_head=4;
    RC_CHECK(loom_post_cqe(l,18,0,0)<0,"hostile ahead head refuses publication");
    large=loom_create_with_receipts(4096,8192,true);
    RC_CHECK(large&&large->receipt_size==262144&&large->receipt_off==409664&&
             large->ring_size==675840,"maximum paired geometry matches ABI");
    RC_CHECK(!loom_create_with_receipts(3,4,true),"invalid SQ geometry refused");
    RC_CHECK(!loom_create_with_receipts(4,2,true),"undersized CQ refused");
done:
    // The tables are this fixture's storage, never the ring's: take them back
    // before the free path can see them.
    if(l){l->service_slots=NULL;l->service_pools=NULL;l->service_requests.detached=false;loom_unref(l);}
    if(legacy)loom_unref(legacy);if(large)loom_unref(large);
    return error;
}
#undef RC_CHECK
#endif
