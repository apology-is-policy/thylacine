#include <thyla/loom_client/paired_cq.h>
int t_paired_init(struct t_paired_cq *r,void *base,size_t len,size_t hdr,size_t cq,size_t receipts,uint32_t entries){
 if(!r||!base||(uintptr_t)base%8||len>PTRDIFF_MAX||len>UINTPTR_MAX-(uintptr_t)base||
    !entries||(entries&(entries-1))||entries>8192)return T_CQ_GEOMETRY;
 size_t starts[3]={hdr,cq,receipts},sizes[3]={64,(size_t)entries*16,(size_t)entries*32};
 for(unsigned i=0;i<3;++i){
  if(starts[i]%8||starts[i]>len||sizes[i]>len-starts[i])return T_CQ_GEOMETRY;
  for(unsigned j=0;j<i;++j)if(starts[i]<starts[j]+sizes[j]&&starts[j]<starts[i]+sizes[i])return T_CQ_GEOMETRY;
 }
 unsigned char *p=base;
 *r=(struct t_paired_cq){.head=(_Atomic uint32_t *)(p+hdr+16),.tail=(_Atomic uint32_t *)(p+hdr+20),
   .cq=(const struct t_owned_cqe *)(p+cq),.receipts=(const struct t_loom_service_buffer_receipt *)(p+receipts),.entries=entries};
 r->next=atomic_load_explicit(r->head,memory_order_relaxed);return 0;
}
int t_paired_reap(struct t_paired_cq *r,struct t_paired_record *out){
 if(!r||!out)return T_CQ_CORRUPT;
 if(atomic_load_explicit(r->head,memory_order_relaxed)!=r->next)return T_CQ_CORRUPT;
 uint32_t tail=atomic_load_explicit(r->tail,memory_order_acquire),pending=tail-r->next;
 if(pending>r->entries)return T_CQ_CORRUPT;
 if(!pending)return 0;
 uint32_t slot=r->next&(r->entries-1);
 struct t_paired_record record={.cqe=r->cq[slot],.receipt=r->receipts[slot]};
 *out=record;
 ++r->next;
 atomic_store_explicit(r->head,r->next,memory_order_release);
 return 1;
}
