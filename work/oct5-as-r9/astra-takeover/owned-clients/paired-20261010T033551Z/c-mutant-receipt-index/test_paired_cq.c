#include "paired_cq.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
struct memory{_Atomic uint32_t header[16];struct t_owned_cqe cq[4];struct t_loom_service_buffer_receipt receipts[4];};
_Static_assert(sizeof(struct memory)==256,"fixture geometry");
static void init(struct memory *m,uint32_t cursor){memset(m,0,sizeof(*m));for(unsigned i=0;i<16;++i)atomic_init(&m->header[i],0);atomic_store(&m->header[4],cursor);atomic_store(&m->header[5],cursor);}
static void publish(struct memory *m,uint32_t cursor,uint64_t seq){unsigned slot=cursor&3;m->cq[slot]=(struct t_owned_cqe){seq,(int32_t)(seq%4096),4};m->receipts[slot]=(struct t_loom_service_buffer_receipt){.pool={7,0,seq+91},.member=slot,.lease=seq+1};atomic_store_explicit(&m->header[5],cursor+1,memory_order_release);}
static void *producer(void *arg){struct memory *m=arg;for(uint32_t n=0;n<100000;++n){uint32_t cursor=UINT32_MAX-50+n;while(cursor-atomic_load_explicit(&m->header[4],memory_order_acquire)==4)sched_yield();publish(m,cursor,n);}return NULL;}
int main(void){struct memory m;struct t_paired_cq q;struct t_paired_record r,kept[4];init(&m,UINT32_MAX-1);assert(!t_paired_init(&q,&m,sizeof(m),0,64,128,4));assert(!t_paired_reap(&q,&r));
 for(uint32_t i=0;i<4;++i){publish(&m,UINT32_MAX-1+i,i);assert(t_paired_reap(&q,&kept[i])==1);}
 publish(&m,2,99);for(unsigned i=0;i<4;++i){assert(kept[i].cqe.user_data==i);assert(kept[i].receipt.lease==i+1);assert(kept[i].receipt.pool.incarnation==i+91);}
 assert(atomic_load(&m.header[4])==2);assert(t_paired_reap(&q,&r)==1&&r.cqe.user_data==99);
 init(&m,0);assert(!t_paired_init(&q,&m,sizeof(m),0,64,128,4));atomic_store(&m.header[5],5);assert(t_paired_reap(&q,&r)==-2&&atomic_load(&m.header[4])==0);atomic_store(&m.header[5],1);atomic_store(&m.header[4],1);assert(t_paired_reap(&q,&r)==-2);
 size_t cases[][5]={{256,0,64,128,0},{256,0,64,128,3},{256,0,64,128,16384},{255,0,64,128,4},{256,0,60,128,4},{256,0,64,64,4},{256,0,64,0,4},{256,SIZE_MAX-7,64,128,4},{SIZE_MAX,0,64,128,4}};
 for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i)assert(t_paired_init(&q,&m,cases[i][0],cases[i][1],cases[i][2],cases[i][3],(uint32_t)cases[i][4])==-1);
 init(&m,UINT32_MAX-50);assert(!t_paired_init(&q,&m,sizeof(m),0,64,128,4));pthread_t p;assert(!pthread_create(&p,NULL,producer,&m));for(uint64_t n=0;n<100000;++n){int rc;while((rc=t_paired_reap(&q,&r))==0)sched_yield();assert(rc==1);assert(r.cqe.user_data==n&&r.cqe.result==(int32_t)(n%4096)&&r.cqe.flags==4);assert(r.receipt.lease==n+1&&r.receipt.pool.incarnation==n+91);}assert(!pthread_join(p,NULL));assert(!t_paired_reap(&q,&r));puts("C paired CQ geometry/wrap/concurrent100000 PASS");return 0;}
