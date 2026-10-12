#include <thyla/loom_client/request_book.h>
#include <stdatomic.h>
static _Atomic uint64_t next_cookie=1;
enum { FREE,PREPARED,ACTIVE,TERMINAL };
static int cookie(_Atomic uint64_t *counter,uint64_t *out){
 uint64_t n=atomic_load_explicit(counter,memory_order_relaxed);
 do{if(n==UINT64_MAX)return T_REQ_EXHAUSTED;}while(!atomic_compare_exchange_weak_explicit(counter,&n,n+1,memory_order_relaxed,memory_order_relaxed));
 *out=n;return 0;
}
static int valid(struct t_loom_service_ref r){return r.slot<64&&!r.reserved&&r.incarnation;}
static int same(struct t_loom_service_ref a,struct t_loom_service_ref b){return a.slot==b.slot&&a.reserved==b.reserved&&a.incarnation==b.incarnation;}
static int zero(struct t_loom_service_buffer_receipt r){return !r.pool.slot&&!r.pool.reserved&&!r.pool.incarnation&&!r.member&&!r.reserved&&!r.lease;}
int t_request_create(struct t_request_book *b,struct t_request_entry *entries,size_t n){
 if(!b)return T_REQ_GEOMETRY;b->count=0;b->entries=NULL;
 if(!entries||!n||n>8192||(n&(n-1)))return T_REQ_GEOMETRY;
 t_bytes_zero(entries,n*sizeof(*entries));b->entries=entries;b->count=n;return 0;
}
void t_request_destroy(struct t_request_book *b){b->count=0;b->entries=NULL;}
int t_request_reserve(struct t_request_book *b,struct t_loom_service_ref scope,struct t_request_spec spec,uint64_t *out){
 if(!b||!b->entries||!out||!valid(scope)||spec.kind<T_REQ_CONTROL||spec.kind>T_REQ_POOLED_READ)return T_REQ_GEOMETRY;
 if(spec.kind==T_REQ_POOLED_READ){if(!valid(spec.pool)||!spec.limit||spec.multi>1)return T_REQ_GEOMETRY;}
 else if(spec.multi||spec.pool.slot||spec.pool.reserved||spec.pool.incarnation)return T_REQ_GEOMETRY;
 for(size_t i=0;i<b->count;++i)if(b->entries[i].phase==FREE){
  uint64_t id;int rc=cookie(&next_cookie,&id);if(rc)return rc;
  b->entries[i]=(struct t_request_entry){.cookie=id,.scope=scope,.spec=spec,.phase=PREPARED};*out=id;return 0;
 }
 return T_REQ_FULL;
}
static struct t_request_entry *find(struct t_request_book *b,uint64_t id){
 for(size_t i=0;i<b->count;++i)if(b->entries[i].phase!=FREE&&b->entries[i].cookie==id)return &b->entries[i];
 return NULL;
}
int t_request_cancel_unpublished(struct t_request_book *b,uint64_t id){struct t_request_entry *e=find(b,id);if(!e)return T_REQ_STALE;if(e->phase!=PREPARED)return T_REQ_STATE;e->phase=FREE;return 0;}
int t_request_publish(struct t_request_book *b,uint64_t id){struct t_request_entry *e=find(b,id);if(!e)return T_REQ_STALE;if(e->phase!=PREPARED)return T_REQ_STATE;e->phase=ACTIVE;return 0;}
int t_request_accept(struct t_request_book *b,const struct t_paired_record *r,struct t_request_delivery *out){
 if(!r||!out)return T_REQ_RECORD;
 struct t_request_entry *e=find(b,r->cqe.user_data);if(!e)return T_REQ_STALE;if(e->phase!=ACTIVE)return T_REQ_STATE;
 uint32_t flags=r->cqe.flags;int32_t result=r->cqe.result;
 if(e->spec.kind==T_REQ_POOLED_READ&&result>0){
  if((flags&~5u)||!(flags&4u)||(!e->spec.multi&&(flags&1u))||(uint32_t)result>e->spec.limit||
     !same(r->receipt.pool,e->spec.pool)||r->receipt.reserved||r->receipt.member>=64||!r->receipt.lease)return T_REQ_RECORD;
 }else{
  if(flags||!zero(r->receipt))return T_REQ_RECORD;
  if(e->spec.kind==T_REQ_CONTROL&&result>0)return T_REQ_RECORD;
  if((e->spec.kind==T_REQ_FIXED_READ||e->spec.kind==T_REQ_FIXED_WRITE)&&result>0&&(uint32_t)result>e->spec.limit)return T_REQ_RECORD;
 }
 unsigned terminal=!(flags&1u);
 *out=(struct t_request_delivery){e->cookie,e->scope,e->spec,terminal,e->abandoned,*r};
 if(terminal){e->phase=TERMINAL;e->terminal=r->cqe;}return 0;
}
int t_request_abandon(struct t_request_book *b,uint64_t id,struct t_loom_service_ref *scope){
 if(!scope)return T_REQ_GEOMETRY;*scope=(struct t_loom_service_ref){0};
 struct t_request_entry *e=find(b,id);if(!e)return T_REQ_STALE;if(e->phase==PREPARED)return T_REQ_STATE;
 unsigned abort=!e->abandoned&&e->phase==ACTIVE;e->abandoned=1;if(abort)*scope=e->scope;return (int)abort;
}
int t_request_take_terminal(struct t_request_book *b,uint64_t id,struct t_owned_cqe *out){
 if(!out)return T_REQ_GEOMETRY;struct t_request_entry *e=find(b,id);if(!e)return T_REQ_STALE;if(e->phase!=TERMINAL)return T_REQ_STATE;*out=e->terminal;e->phase=FREE;return 0;
}
size_t t_request_occupied(const struct t_request_book *b){size_t n=0;for(size_t i=0;i<b->count;++i)n+=b->entries[i].phase!=FREE;return n;}
#ifdef T_REQUEST_TEST
int t_request_exhausted_test(void){_Atomic uint64_t n=UINT64_MAX-1;uint64_t out=0;
 return cookie(&n,&out)==0&&out==UINT64_MAX-1&&cookie(&n,&out)==T_REQ_EXHAUSTED&&cookie(&n,&out)==T_REQ_EXHAUSTED;
}
#endif
void t_request_clear_retired(struct t_request_book *b){for(size_t i=0;i<b->count;++i)b->entries[i].phase=FREE;}
