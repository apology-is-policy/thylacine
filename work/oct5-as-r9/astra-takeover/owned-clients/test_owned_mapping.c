#undef calloc
#include "owned_mapping.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct probe{unsigned closed,freed;};struct backing{unsigned char *base;size_t len;struct probe *p;};
static int fail_alloc;
void *t_test_calloc(size_t n,size_t s){if(fail_alloc){fail_alloc=0;return NULL;}return calloc(n,s);}
static void *base(void *v){return ((struct backing *)v)->base;}static size_t length(void *v){return ((struct backing *)v)->len;}
static void close_owner(void *v){++((struct backing *)v)->p->closed;}
static void destroy(void *v){struct backing *b=v;assert(b->p->closed==1);++b->p->freed;free(b->base);free(b);}
static uint32_t align_up(uint32_t v,uint32_t a){return (v+a-1)&~(a-1);}
static struct t_mapping_backend fixture(struct probe *q,uint32_t sq,uint32_t cq,struct t_owned_setup *p){
 uint32_t sqe=align_up(64+sq*4,64),cqe=align_up(sqe+sq*64,64),receipt=align_up(cqe+cq*16,64),len=align_up(receipt+cq*32,4096);
 struct backing *b=calloc(1,sizeof(*b));assert(b);b->base=aligned_alloc(4096,len);assert(b->base);memset(b->base,0,len);b->len=len;b->p=q;
 *p=(struct t_owned_setup){.flags=12,.sq_entries=sq,.cq_entries=cq,.ring_size=len,.ring_va=(uintptr_t)b->base,.sq_array_off=64,.sqe_off=sqe,.cqe_off=cqe,.sq_array_size=sq*4,.sqe_size=sq*64,.cqe_size=cq*16,.receipt={receipt,cq*32,32,0}};
 _Atomic uint32_t *h=(_Atomic uint32_t *)(void *)b->base;atomic_store(&h[2],sq-1);atomic_store(&h[3],sq);atomic_store(&h[6],cq-1);atomic_store(&h[7],cq);
 return (struct t_mapping_backend){b,base,length,close_owner,destroy};
}
int main(void){
 uint32_t sqs[]={1,2,4096},cqs[]={1,2,8192};struct t_owned_mapping *m;struct t_paired_record r;
 for(unsigned i=0;i<3;++i){struct probe q={0};struct t_owned_setup p;struct t_mapping_backend b=fixture(&q,sqs[i],cqs[i],&p);unsigned char *raw=base(b.context);assert(!t_mapping_create(b,p,&m));assert(!t_mapping_reap(m,&r));
  *(struct t_owned_cqe *)(void *)(raw+p.cqe_off)=(struct t_owned_cqe){77,8,4};*(struct t_loom_service_buffer_receipt *)(void *)(raw+p.receipt[0])=(struct t_loom_service_buffer_receipt){.pool={3,0,9},.member=2,.lease=42};_Atomic uint32_t *h=(_Atomic uint32_t *)(void *)raw;atomic_store_explicit(&h[5],1,memory_order_release);
  assert(t_mapping_reap(m,&r)==1&&r.cqe.user_data==77&&r.receipt.lease==42&&atomic_load(&h[4])==1);t_mapping_close(m);t_mapping_close(m);assert(t_mapping_reap(m,&r)==T_MAP_CLOSED&&q.closed==1&&!q.freed);t_mapping_destroy(m);assert(q.freed==1&&r.receipt.lease==42);
 }
 for(unsigned i=0;i<23;++i){struct probe q={0};struct t_owned_setup p;struct t_mapping_backend b=fixture(&q,4,8,&p);switch(i){case 0:p.flags=4;break;case 1:p.reserved=1;break;case 2:p.receipt[3]=1;break;case 3:p.receipt[2]=16;break;case 4:p.ring_va+=4096;break;case 5:--p.ring_size;break;case 6:p.sq_entries=3;break;case 7:p.sq_entries=8192;break;case 8:p.cq_entries=2;break;case 9:p.cq_entries=7;break;case 10:p.cq_entries=16384;break;case 11:p.sq_array_size+=4;break;case 12:p.sqe_size+=64;break;case 13:p.cqe_size+=16;break;case 14:p.receipt[1]+=32;break;case 15:p.hdr_off=1;break;case 16:p.sq_array_off=1;break;case 17:p.sqe_off=p.ring_size;break;case 18:p.cqe_off=p.ring_size;break;case 19:p.receipt[0]=UINT64_MAX-31;break;case 20:p.sqe_off=p.cqe_off;break;case 21:p.sq_array_off=p.cqe_off;break;case 22:p.receipt[0]=0;break;}
  assert(t_mapping_create(b,p,&m)==T_MAP_GEOMETRY&&!m&&q.closed==1&&q.freed==1);
 }
 for(unsigned i=1;i<5;++i){for(unsigned j=0;j<i;++j){struct probe q={0};struct t_owned_setup p;struct t_mapping_backend b=fixture(&q,4,8,&p);uint32_t starts[]={p.hdr_off,p.sq_array_off,p.sqe_off,p.cqe_off,(uint32_t)p.receipt[0]};switch(i){case 1:p.sq_array_off=starts[j];break;case 2:p.sqe_off=starts[j];break;case 3:p.cqe_off=starts[j];break;case 4:p.receipt[0]=starts[j];break;}assert(t_mapping_create(b,p,&m)==T_MAP_GEOMETRY&&!m&&q.freed==1);}}
 for(unsigned i=0;i<11;++i){struct probe q={0};struct t_owned_setup p;struct t_mapping_backend b=fixture(&q,4,8,&p);_Atomic uint32_t *h=base(b.context);atomic_fetch_xor(&h[i],i==8?2:1);assert(t_mapping_create(b,p,&m)==T_MAP_HEADER&&!m&&q.freed==1);}
 {struct probe q={0};struct t_owned_setup p;struct t_mapping_backend b=fixture(&q,4,8,&p);_Atomic uint32_t *h=base(b.context);atomic_store(&h[8],1);assert(!t_mapping_create(b,p,&m));t_mapping_destroy(m);assert(q.freed==1);}
 {struct probe q={0};struct t_owned_setup p;struct t_mapping_backend b=fixture(&q,4,8,&p);_Atomic uint32_t *h=base(b.context);assert(!t_mapping_create(b,p,&m));atomic_store(&h[5],9);assert(t_mapping_reap(m,&r)==T_MAP_CORRUPT);t_mapping_destroy(m);assert(q.freed==1);}
 {struct probe q={0};struct t_owned_setup p;struct t_mapping_backend b=fixture(&q,4,8,&p);fail_alloc=1;assert(t_mapping_create(b,p,&m)==T_MAP_NOMEM&&!m&&q.closed==1&&q.freed==1);}
 puts("C six owned mapping scenarios PASS");return 0;
}
