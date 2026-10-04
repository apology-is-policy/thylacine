// Shared native/host fixture for actual Loom receipt geometry/publication.
// Pool setup here supplies canonical test storage directly; registration and
// creator authority are separate private-owner integration obligations.
#ifndef LOOM_RECEIPT_FIXTURE_H
#define LOOM_RECEIPT_FIXTURE_H
#include <thylacine/loom_service_pool.h>
#include <thylacine/errno.h>
#define RC_CHECK(x,msg) do { if (!(x)) { error=msg; goto done; } } while (0)
static struct loom_pool_bank rc_bank;
static struct loom_pool rc_pool;
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
    rc_zero(&rc_bank,sizeof(rc_bank));rc_zero(&rc_pool,sizeof(rc_pool));
    l=loom_create_with_receipts(1,1,true);
    legacy=loom_create(1,1,true);
    RC_CHECK(l&&legacy,"receipt rings allocated");
    RC_CHECK(!legacy->receipt_off&&!legacy->receipt_size,"legacy receipt geometry absent");
    RC_CHECK(l->receipt_size==32&&!(l->receipt_off&63)&&
             l->receipt_off>=l->cqe_off+l->cqe_size&&
             l->receipt_off+l->receipt_size<=l->ring_size,"receipt geometry bounded aligned");
    struct loom_service_ref ref={.slot=63,.incarnation=0x100000001ULL};
    struct loom_pool_extent extent={.backing=rc_payload,.offset=0,.length=sizeof(rc_payload)};
    RC_CHECK(!loom_pool_prepare(&rc_bank,&rc_pool,ref,&extent,1)&&!loom_pool_publish(&rc_pool),"receipt pool prepared");
    RC_CHECK(!loom_pool_stream_get(&rc_pool),"receipt stream retained");
    struct loom_service_buffer_receipt receipt;
    RC_CHECK(!loom_pool_claim(&rc_bank,&rc_pool,4,&receipt),"receipt member claimed");
    rc_payload[0]=0x71;rc_payload[1]=0x72;rc_payload[2]=0x73;rc_payload[3]=0x74;
    RC_CHECK(!loom_pool_commit(&rc_bank,&rc_pool,&receipt,4,0x8877665544332211ULL,true),"receipt result committed");
    RC_CHECK(loom_post_pool_cqe(legacy,&rc_bank,&rc_pool,0)==-T_E_OPNOTSUPP,"legacy rejects paired publication");
    RC_CHECK(!loom_post_cqe(l,9,0,0),"fill CQ before pending payload");
    RC_CHECK(loom_post_pool_cqe(l,&rc_bank,&rc_pool,0)==-T_E_AGAIN,"full CQ retains pending payload");
    RC_CHECK(rc_bank.cells[0].phase==LMEMBER_PENDING,"CQ backpressure cannot lease or free member");
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    struct loom_cqe *cq=(void *)(l->ring_kva+l->cqe_off);
    struct loom_service_buffer_receipt *r=(void *)(l->ring_kva+l->receipt_off);
    __atomic_store_n(&h->cq_head,1,__ATOMIC_RELEASE);
    // Hostile shared mirrors never select a write address or mint a receipt.
    h->cq_tail=0xffffffffu;h->cq_mask=0xffffffffu;r[0].lease=0xdead;
#ifdef LOOM_RECEIPT_HOST_OBSERVE
    rc_watch(l,&rc_bank,receipt);
#endif
    RC_CHECK(!loom_post_pool_cqe(l,&rc_bank,&rc_pool,0),"paired payload published");
    RC_CHECK(cq[0].user_data==0x8877665544332211ULL&&cq[0].result==4&&
             cq[0].flags==(LOOM_CQE_MORE|LOOM_CQE_SERVICE_BUFFER),"paired CQE exact");
    RC_CHECK(rc_receipt_equal(r[0],receipt)&&h->cq_tail==2,"paired receipt exact");
    RC_CHECK(rc_bank.cells[0].phase==LMEMBER_LEASED,"published payload leased");
    __atomic_store_n(&h->cq_head,2,__ATOMIC_RELEASE);
    RC_CHECK(loom_pool_claim(&rc_bank,&rc_pool,4,&r[0])==-T_E_AGAIN,"CQ acknowledgement cannot return payload");
    RC_CHECK(rc_payload[0]==0x71&&rc_payload[3]==0x74,"leased payload intact");
    RC_CHECK(!loom_post_cqe(l,12,-T_E_CANCELED,0),"terminal CQE published");
    struct loom_service_buffer_receipt zero={0};
    RC_CHECK(rc_receipt_equal(r[0],zero),"nonleased CQE clears old receipt");
    RC_CHECK(rc_bank.cells[0].phase==LMEMBER_LEASED,"terminal does not return old payload");
    RC_CHECK(!loom_pool_return(&rc_bank,&rc_pool,&receipt),"exact explicit payload return");
    RC_CHECK(!loom_pool_stream_put(&rc_pool)&&!loom_pool_reap(&rc_bank,&rc_pool),"returned pool reaped");
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
    if(l)loom_unref(l);if(legacy)loom_unref(legacy);if(large)loom_unref(large);
    return error;
}
#undef RC_CHECK
#endif
