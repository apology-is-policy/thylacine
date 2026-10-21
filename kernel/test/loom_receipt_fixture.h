// Shared native/host fixture for actual Loom receipt geometry and paired
// publication. The pool lives in a real slot/pool table, built through the
// table transactions, and is published by loom_provided_deliver -- the one
// live publication path -- from the states loom_provided_commit_locked leaves:
// a MORE shot (settling, then after cancellation) and a single-shot terminal
// whose request record is TERMINAL. The tables are this fixture's own storage,
// lent to the rings for the checks and taken back before they are freed;
// registration and creator authority are separate private-owner integration
// obligations.
#ifndef LOOM_RECEIPT_FIXTURE_H
#define LOOM_RECEIPT_FIXTURE_H
#include <thylacine/loom_service_pool_table.h>
#include <thylacine/loom_service_operation.h>
#include <thylacine/loom_service_provided.h>
#include <thylacine/errno.h>
#define RC_CHECK(x,msg) do { if (!(x)) { error=msg; goto done; } } while (0)
// Only the host counts poll wakes; the guest's are real.
#ifdef LOOM_RECEIPT_HOST_OBSERVE
#define RC_WOKEN(n,msg) RC_CHECK(wakes==(n),msg)
#else
#define RC_WOKEN(n,msg) do { } while (0)
#endif
static struct loom_slot_table rc_slots;
static struct loom_service_pool_table rc_pools;
static struct loom_service_operation rc_op;
static struct loom_service_request rc_records[1];
static u8 rc_payload[64];
static void rc_zero(void *p,size_t n) { for(size_t i=0;i<n;i++) ((u8 *)p)[i]=0; }
static bool rc_receipt_equal(struct loom_service_buffer_receipt a,
                              struct loom_service_buffer_receipt b) {
    return a.pool.slot==b.pool.slot && a.pool.reserved==b.pool.reserved &&
           a.pool.incarnation==b.pool.incarnation && a.member==b.member &&
           a.reserved==b.reserved && a.lease==b.lease;
}
static void rc_lend(struct Loom *l,bool lend) {
    l->service_slots=lend?&rc_slots:NULL;l->service_pools=lend?&rc_pools:NULL;
}
static const char *loom_receipt_fixture(void) {
    const char *error=NULL;
    struct Loom *l=NULL,*legacy=NULL,*large=NULL;
    struct loom_request_table requests_before={0};
    rc_zero(&rc_slots,sizeof(rc_slots));rc_zero(&rc_pools,sizeof(rc_pools));
    rc_zero(&rc_op,sizeof(rc_op));rc_zero(rc_records,sizeof(rc_records));
    l=loom_create_with_receipts(1,1,true);
    if(l)requests_before=l->service_requests;
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
    rc_op.pool=ref;rc_op.receipt=receipt;rc_op.phase=LRPC_SETTLING;
    rc_op.pool_stream=true;rc_op.pool_pending=true;rc_op.multishot=true;
    // A ring with no receipt array refuses even when it can resolve the pool;
    // its receipt_off of 0 is the shared header.
    rc_op.owner=legacy;rc_lend(legacy,true);
    RC_CHECK(loom_provided_deliver(&rc_op)==-T_E_OPNOTSUPP,"legacy ring refuses paired publication");
    RC_CHECK(!legacy->cq_tail&&bank->cells[0].phase==LMEMBER_PENDING&&rc_op.pool_pending,
             "legacy refusal writes nothing");
    rc_lend(legacy,false);
    rc_op.owner=l;
    RC_CHECK(loom_provided_deliver(&rc_op)==-T_E_NOENT,"receipt ring without a pool table refuses publication");
    rc_lend(l,true);
    RC_CHECK(!loom_post_cqe(l,9,0,0),"fill CQ before pending payload");
    RC_CHECK(loom_provided_deliver(&rc_op)==-T_E_AGAIN,"full CQ retains pending payload");
    RC_CHECK(bank->cells[0].phase==LMEMBER_PENDING&&rc_op.pool_pending,"CQ backpressure cannot lease or free member");
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    struct loom_cqe *cq=(void *)(l->ring_kva+l->cqe_off);
    struct loom_service_buffer_receipt *r=(void *)(l->ring_kva+l->receipt_off);
    __atomic_store_n(&h->cq_head,1,__ATOMIC_RELEASE);
    // Hostile shared mirrors never select a write address or mint a receipt.
    h->cq_tail=0xffffffffu;h->cq_mask=0xffffffffu;r[0].lease=0xdead;
    // A backstop the engine never reaches (it detaches only with no operation
    // linked): even with CQ room, a detached consumer gets nothing written.
    l->service_requests.detached=true;
    RC_CHECK(loom_provided_deliver(&rc_op)==-T_E_CANCELED,"detached consumer refuses publication");
    RC_CHECK(bank->cells[0].phase==LMEMBER_PENDING&&rc_op.pool_pending&&!rc_op.pool_more_delivered&&
             l->cq_tail==1&&h->cq_tail==0xffffffffu&&r[0].lease==0xdead&&cq[0].user_data==9,
             "detached refusal writes nothing");
    l->service_requests.detached=false;
    RC_WOKEN(1,"refusals wake no consumer");
#ifdef LOOM_RECEIPT_HOST_OBSERVE
    rc_watch(l,bank,receipt);
#endif
    RC_CHECK(!loom_provided_deliver(&rc_op),"paired payload published");
    RC_WOKEN(2,"publication wakes the consumer");
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
    // A cancelled multishot's last committed shot is still a MORE payload.
    __atomic_store_n(&h->cq_head,3,__ATOMIC_RELEASE);
    RC_CHECK(!loom_pool_claim(bank,pool,4,&receipt)&&
             !loom_pool_commit(bank,pool,&receipt,4,0x8877665544332212ULL,true),"cancelled shot committed");
    rc_op.receipt=receipt;rc_op.pool_pending=true;rc_op.pool_more_delivered=false;rc_op.phase=LRPC_TERMINAL;
    RC_CHECK(!loom_provided_deliver(&rc_op),"a cancelled multishot's committed shot still publishes");
    RC_CHECK(cq[0].user_data==0x8877665544332212ULL&&cq[0].flags==(LOOM_CQE_MORE|LOOM_CQE_SERVICE_BUFFER)&&
             rc_receipt_equal(r[0],receipt)&&bank->cells[0].phase==LMEMBER_LEASED&&rc_op.pool_more_delivered,
             "cancelled shot published leased");
    RC_CHECK(!loom_pool_return(bank,pool,&receipt),"cancelled shot returned");
    // A single-shot terminal shares its one CQE with the payload: the request
    // record is TERMINAL (loom_admission_commit_locked) and is delivered with it.
    RC_CHECK(!loom_pool_claim(bank,pool,4,&receipt)&&
             !loom_pool_commit(bank,pool,&receipt,4,0x8877665544332213ULL,false),"single shot committed");
    rc_records[0]=(struct loom_service_request){.identity=0x51,.user_data=0x8877665544332213ULL,
                                                 .result=4,.phase=LREQ_TERMINAL,.owners=1};
    l->service_requests.records=rc_records;l->service_requests.capacity=1;
    l->service_requests.occupied=1;l->service_requests.admit=1;l->service_requests.delivered=0;
    rc_op.request=(struct loom_request_ref){.index=0,.identity=0x51};
    rc_op.receipt=receipt;rc_op.pool_pending=true;rc_op.pool_more_delivered=false;
    rc_op.multishot=false;rc_op.pool_terminal=true;
    __atomic_store_n(&h->cq_head,4,__ATOMIC_RELEASE);
    rc_records[0].result=5;
    RC_CHECK(loom_provided_deliver(&rc_op)==-T_E_IO,"terminal must match its request");
    rc_records[0].result=4;
    l->service_requests.delivered=1+LOOM_SERVICE_SLOT_COUNT;
    RC_CHECK(loom_provided_deliver(&rc_op)==-T_E_AGAIN,"terminal waits for request-delivery room");
    l->service_requests.delivered=0;
    RC_CHECK(bank->cells[0].phase==LMEMBER_PENDING&&rc_op.pool_pending&&l->cq_tail==4&&
             rc_records[0].phase==LREQ_TERMINAL,"terminal refusals write nothing");
    RC_WOKEN(4,"terminal refusals wake no consumer");
    RC_CHECK(!loom_provided_deliver(&rc_op),"single-shot terminal published");
    RC_WOKEN(5,"terminal publication wakes the consumer");
    RC_CHECK(cq[0].user_data==0x8877665544332213ULL&&cq[0].result==4&&cq[0].flags==LOOM_CQE_SERVICE_BUFFER&&
             rc_receipt_equal(r[0],receipt)&&bank->cells[0].phase==LMEMBER_LEASED&&
             rc_records[0].phase==LREQ_DELIVERED&&l->service_requests.delivered==1&&
             !rc_op.pool_pending&&!rc_op.pool_more_delivered,"single-shot terminal and payload share one CQE");
    RC_CHECK(!loom_pool_return(bank,pool,&receipt),"single shot returned");
    rc_op.pool_stream=false;
    RC_CHECK(!loom_pool_stream_put(pool)&&!loom_service_pool_reap(&rc_slots,&rc_pools,ref)&&
             loom_service_pools_empty(&rc_pools),"returned pool reaped");
    __atomic_store_n(&h->cq_head,5,__ATOMIC_RELEASE);
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
    // The tables are this fixture's storage, never the rings': take them back
    // before the free path can see them.
    if(l){rc_lend(l,false);l->service_requests=requests_before;loom_unref(l);}
    if(legacy){rc_lend(legacy,false);loom_unref(legacy);}
    if(large)loom_unref(large);
    return error;
}
#undef RC_WOKEN
#undef RC_CHECK
#endif
