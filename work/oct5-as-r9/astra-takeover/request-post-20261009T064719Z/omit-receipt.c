
#include <stdio.h>
#include <stdlib.h>
#include <thylacine/loom_service_requests.h>
#include <thylacine/errno.h>
#define LOOM_MAGIC 123
#define CHECK(c,s) do { if(!(c)) { puts(s);exit(11); } } while(0)
typedef struct { int held; } spin_lock_t;
static int held,wakes;
static void spin_lock(spin_lock_t *l) {CHECK(!l->held && !held,"lock reentry");l->held=1;held++;}
static void spin_unlock(spin_lock_t *l) {CHECK(l->held && held==1,"lock release");l->held=0;held--;}
struct loom_ring_hdr {u32 cq_head,cq_tail,cq_mask;};
struct loom_cqe {u64 user_data;s32 result;u32 flags;};
struct Loom {u32 magic;spin_lock_t lock;void *service_as;bool service_closing;
 struct loom_request_table service_requests;u8 *ring_kva;u32 hdr_off,cqe_off,receipt_off,receipt_size,cq_tail,cq_entries;int cq_waiters;};
static bool loom_is_private(struct Loom *l) {return l && l->service_as;}
static void poll_waiter_list_wake(int *p) {(void)p;CHECK(!held,"wake outside owner lock");wakes++;}
int loom_private_post_terminal(struct Loom *l, struct loom_request_ref ref) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l)) return -T_E_INVAL;
    spin_lock(&l->lock);
    struct loom_request_table *t = &l->service_requests;
    u64 user_data; s32 result;
    int rc = loom_request_peek(t,ref,&user_data,&result);
    if (rc) { spin_unlock(&l->lock); return rc; }
    if (t->detached) { spin_unlock(&l->lock); return -T_E_CANCELED; }
    struct loom_ring_hdr *h = (void *)(l->ring_kva+l->hdr_off);
    u32 tail = l->cq_tail;
    u32 head = __atomic_load_n(&h->cq_head,__ATOMIC_ACQUIRE);
    if ((u32)(tail-head) >= l->cq_entries) {
        spin_unlock(&l->lock);
        return -T_E_AGAIN;
    }
    u32 idx = tail & (l->cq_entries-1u);
    struct loom_cqe *cq = (void *)(l->ring_kva+l->cqe_off);
    cq[idx] = (struct loom_cqe){ .user_data=user_data, .result=result };
    if (l->receipt_size) {
        struct loom_service_buffer_receipt *receipts = (void *)(l->ring_kva+l->receipt_off);
        (void)receipts;
    }
    // No abort check here: a terminal committed before abort stays truthful.
    // Copy, mark delivered, and publish the tail under one lock interval.
    rc = loom_request_deliver(t,ref);
    if (rc) { spin_unlock(&l->lock); return rc; }
    l->cq_tail = tail+1u;
    __atomic_store_n(&h->cq_tail,l->cq_tail,__ATOMIC_RELEASE);
    spin_unlock(&l->lock);
    poll_waiter_list_wake(&l->cq_waiters);
    return 0;
}
int main(void) {
 struct {struct loom_ring_hdr hdr;struct loom_cqe cq[2];struct loom_service_buffer_receipt receipts[2];} ring={0};
 struct loom_service_request records[2]={0};
 struct Loom l={.magic=LOOM_MAGIC,.service_as=&ring,.ring_kva=(u8 *)&ring,.cq_entries=2,.cq_tail=2,
 .cqe_off=(u32)((u8 *)ring.cq-(u8 *)&ring),.receipt_off=(u32)((u8 *)ring.receipts-(u8 *)&ring),.receipt_size=sizeof(ring.receipts),
 .service_requests={.records=records,.capacity=2,.occupied=1}};
 struct loom_request_ref ref={.index=0,.identity=3};
 records[0]=(struct loom_service_request){.identity=3,.user_data=77,.result=9,.phase=LREQ_TERMINAL};
 ring.cq[0].user_data=999;ring.receipts[0].lease=999;
 CHECK(loom_private_post_terminal(&l,ref)==-T_E_AGAIN && records[0].phase==LREQ_TERMINAL &&
       l.cq_tail==2 && ring.cq[0].user_data==999 && ring.receipts[0].lease==999 && !wakes,"full CQ leaves terminal and output unchanged");
 ring.hdr.cq_head=1;ring.hdr.cq_tail=0xFFFFFFFFu;ring.hdr.cq_mask=0xFFFFFFFFu;l.service_closing=true;
 CHECK(!loom_private_post_terminal(&l,ref) && l.cq_tail==3 && ring.hdr.cq_tail==3 && wakes==1,
       "prior committed terminal posts after close using private tail");
 CHECK(ring.cq[0].user_data==77 && ring.cq[0].result==9 && !ring.cq[0].flags && !ring.receipts[0].lease,
       "exact scalar terminal clears old receipt");
 CHECK(records[0].phase==LREQ_DELIVERED && loom_private_post_terminal(&l,ref)==-T_E_BUSY && wakes==1 && l.cq_tail==3,
       "terminal delivered exactly once");
 records[1]=(struct loom_service_request){.identity=4,.user_data=88,.result=-T_E_CANCELED,.phase=LREQ_TERMINAL};
 l.service_requests.detached=true;ring.hdr.cq_head=2;ref=(struct loom_request_ref){.index=1,.identity=4};
 CHECK(loom_private_post_terminal(&l,ref)==-T_E_CANCELED && l.cq_tail==3 && records[1].phase==LREQ_TERMINAL && wakes==1,
       "detached consumer receives no new CQE");
 ref.identity=99;
 CHECK(loom_private_post_terminal(&l,ref)==-T_E_NOENT,"stale terminal refused");
 CHECK(!held,"lock balanced");puts("PASS actual request terminal post");return 0;
}
