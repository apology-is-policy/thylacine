#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define THYLACINE_SPINLOCK_H
#include <thylacine/types.h>
typedef struct { int held; } spin_lock_t;
#define SPIN_LOCK_INIT ((spin_lock_t){0})
static void spin_lock_init(spin_lock_t *l) {l->held=0;}
static void spin_lock(spin_lock_t *l) {if(l->held)abort();l->held=1;}
static void spin_unlock(spin_lock_t *l) {if(!l->held)abort();l->held=0;}
#include <thylacine/loom.h>
#include <thylacine/loom_service_pool.h>
#include <thylacine/errno.h>
#include <thylacine/page.h>
struct Burrow { u8 *pages; size_t size; };
#define page_to_pa(p) (p)
#define pa_to_kva(p) (p)
static u64 g_loom_created;
static unsigned heap_live,burrow_live,wakes;
static void *kmalloc(size_t n,unsigned flags) {(void)flags;void *p=calloc(1,n);if(p)heap_live++;return p;}
static void kfree(void *p) {if(p){if(!heap_live)abort();heap_live--;free(p);}}
static struct Burrow *burrow_create_anon(size_t n,bool exempt) {
    (void)exempt;struct Burrow *b=calloc(1,sizeof(*b));if(!b)return NULL;
    b->pages=calloc(1,n+64);if(!b->pages){free(b);return NULL;}
    b->size=n;memset(b->pages+n,0xa5,64);burrow_live++;return b;
}
void poll_waiter_list_init(struct poll_waiter_list *l) {memset(l,0,sizeof(*l));}
void poll_waiter_list_wake(struct poll_waiter_list *l) {(void)l;wakes++;}
void loom_unref(struct Loom *l) {
    if(!l)return;
    for(unsigned i=0;i<64;i++) if(l->ring->pages[l->ring->size+i]!=0xa5) {
        fputs("FAIL ring guard corrupted\n",stderr);exit(1);
    }
    free(l->ring->pages);free(l->ring);burrow_live--;kfree(l);
}
static struct Loom *watch_ring;
static struct loom_pool_bank *watch_bank;
static struct loom_service_buffer_receipt watch_receipt;
static unsigned watched;
static void rc_watch(struct Loom *l,struct loom_pool_bank *bank,
                      struct loom_service_buffer_receipt receipt) {
    watch_ring=l;watch_bank=bank;watch_receipt=receipt;
}
static void observed_store(u32 *p,u32 v,int order) {
    __atomic_store_n(p,v,order);
    if(!watch_ring)return;
    struct Loom *l=watch_ring;struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    if(p!=&h->cq_tail)return;
    u32 idx=(v-1)&(l->cq_entries-1);
    struct loom_cqe *cq=(void *)(l->ring_kva+l->cqe_off);
    struct loom_service_buffer_receipt *r=(void *)(l->ring_kva+l->receipt_off);
    if(cq[idx].user_data!=0x8877665544332211ULL||cq[idx].result!=4||
       cq[idx].flags!=(LOOM_CQE_MORE|LOOM_CQE_SERVICE_BUFFER)||
       memcmp(&r[idx],&watch_receipt,sizeof(watch_receipt))||
       watch_bank->cells[0].phase!=LMEMBER_LEASED) {
        fputs("FAIL publication pairs payload receipt and lease before tail\n",stderr);exit(1);
    }
    watched++;watch_ring=NULL;
}
#define __atomic_store_n(p,v,o) observed_store((p),(v),(o))
/* ACTUAL_LOOM */
#undef __atomic_store_n
#define LOOM_RECEIPT_HOST_OBSERVE 1
#include "loom_receipt_fixture.h"
int main(void) {
    const char *err=loom_receipt_fixture();
    if(err){fprintf(stderr,"FAIL %s\n",err);return 1;}
    if(heap_live||burrow_live||watched!=1||!wakes){fputs("FAIL fixture ownership/publication witnesses\n",stderr);return 1;}
    puts("PASS actual paired CQ publication, full-CQ retention, explicit return and geometry");return 0;
}
