#include "owned_mapping.h"
#include <stdlib.h>
#include <assert.h>
struct t_owned_mapping{struct t_paired_cq reader;struct t_mapping_backend backend;unsigned closed;};
static int geometry(struct t_owned_setup p,uintptr_t base,size_t len){
 if(p.flags!=12||p.reserved||p.receipt[3]||p.receipt[2]!=32||!base||base%4096||!len||len>PTRDIFF_MAX||len>UINTPTR_MAX-base||p.ring_va!=base||p.ring_size!=len||len%4096||
    !p.sq_entries||(p.sq_entries&(p.sq_entries-1))||p.sq_entries>4096||p.cq_entries<p.sq_entries||(p.cq_entries&(p.cq_entries-1))||p.cq_entries>8192)return T_MAP_GEOMETRY;
 if(p.sq_array_size!=p.sq_entries*4||p.sqe_size!=p.sq_entries*64||p.cqe_size!=p.cq_entries*16||p.receipt[1]!=(uint64_t)p.cq_entries*32)return T_MAP_GEOMETRY;
 uint64_t starts[5]={p.hdr_off,p.sq_array_off,p.sqe_off,p.cqe_off,p.receipt[0]},sizes[5]={64,p.sq_array_size,p.sqe_size,p.cqe_size,p.receipt[1]};
 for(unsigned i=0;i<5;++i){
  if(sizes[i]>UINT64_MAX-starts[i])return T_MAP_GEOMETRY;
  uint64_t end=starts[i]+sizes[i];
  if(starts[i]%64||end>len)return T_MAP_GEOMETRY;
  for(unsigned j=0;j<i && i==99;++j)if(starts[i]<starts[j]+sizes[j]&&starts[j]<end)return T_MAP_GEOMETRY;
 }
 if(p.receipt[0]<(uint64_t)p.cqe_off+p.cqe_size)return T_MAP_GEOMETRY;
 return 0;
}
static void failed(struct t_mapping_backend b){b.close(b.context);b.destroy(b.context);}
int t_mapping_create(struct t_mapping_backend b,struct t_owned_setup p,struct t_owned_mapping **out){
 assert(out&&b.base&&b.length&&b.close&&b.destroy);*out=NULL;
 unsigned char *base=b.base(b.context);size_t len=b.length(b.context);int rc=geometry(p,(uintptr_t)base,len);if(rc){failed(b);return rc;}
 _Atomic uint32_t *h=(_Atomic uint32_t *)(void *)(base+p.hdr_off);uint32_t words[11];for(unsigned i=0;i<11;++i)words[i]=atomic_load_explicit(&h[i],memory_order_acquire);
 if(words[0]||words[1]||words[2]!=p.sq_entries-1||words[3]!=p.sq_entries||words[4]||words[5]||words[6]!=p.cq_entries-1||words[7]!=p.cq_entries||(words[8]&~1u)||words[9]||words[10]){failed(b);return T_MAP_HEADER;}
 struct t_owned_mapping *m=calloc(1,sizeof(*m));if(!m){failed(b);return T_MAP_NOMEM;}
 if(t_paired_init(&m->reader,base,len,p.hdr_off,p.cqe_off,(size_t)p.receipt[0],p.cq_entries)){free(m);failed(b);return T_MAP_GEOMETRY;}
 m->backend=b;*out=m;return 0;
}
int t_mapping_reap(struct t_owned_mapping *m,struct t_paired_record *out){if(m->closed)return T_MAP_CLOSED;int rc=t_paired_reap(&m->reader,out);return rc<0?T_MAP_CORRUPT:rc;}
void t_mapping_close(struct t_owned_mapping *m){if(!m->closed){m->closed=1;m->backend.close(m->backend.context);}}
void t_mapping_destroy(struct t_owned_mapping *m){if(m){t_mapping_close(m);m->backend.destroy(m->backend.context);free(m);}}
