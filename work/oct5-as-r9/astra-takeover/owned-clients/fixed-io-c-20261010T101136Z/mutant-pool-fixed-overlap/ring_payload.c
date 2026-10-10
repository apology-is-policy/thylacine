#include "ring_payload.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
struct ring_core;struct pool_state;
struct t_ring_owner{struct ring_core *core;};struct t_ring_client{struct ring_core *core;};struct t_ring_pool{struct pool_state *pool;};
struct t_ring_payload{struct pool_state *pool;struct t_lease_token token;unsigned active,pending;};
struct member{struct t_loom_service_pool_member range;struct t_ring_payload lease;};
struct pool_state{struct ring_core *core;struct t_loom_service_ref identity;struct t_lease_ledger ledger;size_t count,live;unsigned handle_alive,closing;struct t_ring_pool handle;struct member members[];};
enum fixed_phase{FIX_FREE,FIX_PREPARED,FIX_ACTIVE,FIX_TERMINAL};
struct t_ring_fixed{struct ring_core *core;struct t_loom_service_pool_member range;struct t_loom_service_ref scope;uint64_t cookie;enum fixed_phase phase;int32_t result;uint64_t deadline;unsigned write,alive,abandoned,has_deadline,abort_requested;};
struct ring_core{struct t_ring_fixed fixed[64];size_t fixed_handles,deadline_cursor;struct t_ring_backend b;struct pool_state *pools[64],*quarantine;uint64_t last[64];size_t members,live,pool_handles;unsigned closing,closed,root_alive,client_alive;struct t_ring_owner root;struct t_ring_client client;};
static void retire(struct ring_core *c){if(!c->closing){c->closing=1;c->b.begin_retire(c->b.context);}}
static void close_owner(struct ring_core *c){retire(c);if(!c->closed){c->closed=1;c->b.close(c->b.context);}}
static void maybe_free(struct ring_core *c){if(!c->root_alive&&!c->client_alive&&!c->pool_handles&&!c->fixed_handles&&!c->live){close_owner(c);for(size_t i=0;i<64;++i)free(c->pools[i]);free(c->quarantine);c->b.destroy(c->b.context);free(c);}}
static void failed_create(struct t_ring_backend b){b.begin_retire(b.context);b.close(b.context);b.destroy(b.context);}
int t_ring_create(struct t_ring_backend b,struct t_ring_owner **root,struct t_ring_client **client){assert(root&&client&&b.count&&b.length&&b.buffer&&b.return_buffer&&b.retire_pool&&b.destroy_pool&&b.begin_retire&&b.retired&&b.close&&b.destroy&&b.register_pool&&b.abort_scope);*root=NULL;*client=NULL;size_t n=b.count(b.context);if(!n||n>64){failed_create(b);return T_LEASE_GEOMETRY;}for(size_t i=0;i<n;++i){size_t len=b.length(b.context,i);if(!len||len>PTRDIFF_MAX){failed_create(b);return T_LEASE_GEOMETRY;}}struct ring_core *c=calloc(1,sizeof(*c));if(!c){failed_create(b);return -12;}c->b=b;c->root_alive=1;c->client_alive=1;c->root.core=c;c->client.core=c;*root=&c->root;*client=&c->client;return 0;}
static int identity_ok(struct ring_core *c,struct t_loom_service_ref identity){
 if(identity.reserved||!identity.incarnation||identity.slot>=64)return T_LEASE_GEOMETRY;
 unsigned slot=identity.slot;if(c->pools[slot])return T_LEASE_BUSY;
 if(identity.incarnation<=c->last[slot])return T_LEASE_STALE;
 return 0;
}
static int fixed_overlap(struct t_loom_service_pool_member a,struct t_loom_service_pool_member b){return a.buffer_index==b.buffer_index&&a.offset<b.offset+b.length&&b.offset<a.offset+a.length;}
static int prepare(struct ring_core *c,const struct t_loom_service_pool_member *ranges,size_t n,struct pool_state **out){
 if(c->closing)return T_LEASE_STATE;
 if(!ranges||!n||n>64)return T_LEASE_GEOMETRY;
 if(n>64-c->members)return T_LEASE_BUSY;
 struct t_lease_ledger ledger;uint64_t lengths[64];size_t count=c->b.count(c->b.context);for(size_t i=0;i<count;++i)lengths[i]=c->b.length(c->b.context,i);
 int rc=t_lease_prepare(&ledger,ranges,n,lengths,count);if(rc)return rc;
 for(size_t i=0;i<64;++i){struct pool_state *p=c->pools[i];if(!p)continue;for(size_t j=0;j<p->count;++j){struct t_loom_service_pool_member a=p->members[j].range;for(size_t k=0;k<n;++k){struct t_loom_service_pool_member b=ranges[k];if(a.buffer_index==b.buffer_index&&a.offset<b.offset+b.length&&b.offset<a.offset+a.length)return T_LEASE_GEOMETRY;}}}
 for(size_t i=0;i<64;++i){if(c->fixed[i].phase==FIX_FREE)continue;for(size_t j=0;j<n;++j){if(fixed_overlap(c->fixed[i].range,ranges[j]))return T_LEASE_GEOMETRY;}}
 struct pool_state *p=calloc(1,sizeof(*p)+n*sizeof(struct member));if(!p)return -12;
 p->core=c;p->ledger=ledger;p->count=n;p->handle.pool=p;for(size_t i=0;i<n;++i){p->members[i].range=ranges[i];p->members[i].lease.pool=p;}*out=p;return 0;
}
static void publish(struct ring_core *c,struct pool_state *p,struct t_loom_service_ref identity,struct t_ring_pool **out){
 if(t_lease_bind(&p->ledger,identity))abort();
 p->identity=identity;p->handle_alive=1;c->pools[identity.slot]=p;c->last[identity.slot]=identity.incarnation;++c->pool_handles;*out=&p->handle;
}
int64_t t_ring_register_pool(struct t_ring_client *client,const struct t_loom_service_pool_member *ranges,size_t n,struct t_ring_pool **out,int64_t *native_rc){
 struct ring_core *c=client->core;assert(c->client_alive&&out&&native_rc);*out=NULL;*native_rc=0;
 struct pool_state *p=NULL;int rc=prepare(c,ranges,n,&p);if(rc)return rc;
 c->members+=n;struct t_loom_service_ref identity={0};
 /* Pass an immutable contiguous copy; interleaved lease cells are not ABI input. */
 struct t_loom_service_pool_member copy[64];for(size_t i=0;i<n;++i)copy[i]=p->members[i].range;
 int64_t result=c->b.register_pool(c->b.context,copy,n,&identity);*native_rc=result;
 if(result<0&&!identity.slot&&!identity.reserved&&!identity.incarnation){c->members-=n;free(p);return result;}
 if(result!=0||identity_ok(c,identity)){
  p->identity=identity;p->closing=1;c->quarantine=p;close_owner(c);return T_RING_UNCERTAIN;
 }
 publish(c,p,identity,out);return 0;
}
int t_ring_attach(struct t_ring_client *client,struct t_loom_service_ref identity,const struct t_loom_service_pool_member *ranges,size_t n,struct t_ring_pool **out){
 struct ring_core *c=client->core;assert(c->client_alive&&out);*out=NULL;if(c->closing)return T_LEASE_STATE;
 int rc=identity_ok(c,identity);if(rc)return rc;struct pool_state *p=NULL;rc=prepare(c,ranges,n,&p);if(rc)return rc;
 c->members+=n;publish(c,p,identity,out);return 0;
}
int t_ring_accept(struct t_ring_pool *h,int32_t result,uint32_t flags,struct t_loom_service_buffer_receipt receipt,struct t_ring_payload **out){struct pool_state *p=h->pool;struct ring_core *c=p->core;assert(p->handle_alive&&out);*out=NULL;if(c->closing||p->closing)return T_LEASE_STATE;struct t_lease_token token;int rc=t_lease_accept_read(&p->ledger,result,flags,receipt,&token);if(rc<=0)return rc;struct t_ring_payload *l=&p->members[receipt.member].lease;assert(!l->active&&!l->pending);l->token=token;l->active=1;++p->live;++c->live;*out=l;return 1;}
const unsigned char *t_ring_bytes(const struct t_ring_payload *l,size_t *n){assert(l->active&&n);struct pool_state *p=l->pool;struct t_loom_service_pool_member r=p->members[l->token.receipt.member].range;*n=l->token.length;return p->core->b.buffer(p->core->b.context,r.buffer_index)+(size_t)r.offset;}
static int64_t attempt(struct t_ring_payload *l){struct pool_state *p=l->pool;struct ring_core *c=p->core;if(t_lease_begin_return(&p->ledger,&l->token))abort();int64_t rc=c->b.return_buffer(c->b.context,l->token.receipt);if(t_lease_finish_return(&p->ledger,&l->token,rc))abort();return rc;}
int64_t t_ring_return(struct t_ring_payload *l){assert(l->active);struct pool_state *p=l->pool;struct ring_core *c=p->core;int64_t rc=attempt(l);if(!rc){l->active=0;--p->live;--c->live;maybe_free(c);}return rc;}
void t_ring_abandon(struct t_ring_payload *l){assert(l->active);struct pool_state *p=l->pool;struct ring_core *c=p->core;int64_t rc=attempt(l);l->active=0;l->pending=rc!=0;--p->live;--c->live;maybe_free(c);}
void t_ring_pool_drop(struct t_ring_pool *h){struct pool_state *p=h->pool;struct ring_core *c=p->core;assert(p->handle_alive);p->handle_alive=0;p->closing=1;--c->pool_handles;c->b.retire_pool(c->b.context,p->identity);maybe_free(c);}
void t_ring_client_drop(struct t_ring_client *h){struct ring_core *c=h->core;assert(c->client_alive);c->client_alive=0;retire(c);maybe_free(c);}
int t_ring_progress(struct t_ring_owner *h){struct ring_core *c=h->core;assert(c->root_alive);for(size_t i=0;i<64;++i){struct pool_state *p=c->pools[i];if(!p)continue;for(size_t j=0;j<p->count;++j){struct t_ring_payload *l=&p->members[j].lease;if(l->pending&&!attempt(l))l->pending=0;}if(p->closing&&!p->live&&!p->ledger.outstanding){if(c->b.destroy_pool(c->b.context,p->identity)==0){c->members-=p->count;c->pools[i]=NULL;free(p);}}}int done=c->b.retired(c->b.context);size_t occupied=0;for(size_t i=0;i<64;++i){struct t_ring_fixed *f=&c->fixed[i];if(done&&f->abandoned)f->phase=FIX_FREE;occupied+=f->phase!=FIX_FREE;}return c->closing&&!c->live&&!occupied&&done;}
size_t t_ring_members(const struct t_ring_owner *h){assert(h->core->root_alive);return h->core->members;}
size_t t_ring_live(const struct t_ring_owner *h){assert(h->core->root_alive);return h->core->live;}
size_t t_ring_pending(const struct t_ring_owner *h){struct ring_core *c=h->core;assert(c->root_alive);size_t n=0;for(size_t i=0;i<64;++i){struct pool_state *p=c->pools[i];if(p)for(size_t j=0;j<p->count;++j)n+=p->members[j].lease.pending;}return n;}
void t_ring_owner_drop(struct t_ring_owner *h){struct ring_core *c=h->core;assert(c->root_alive);c->root_alive=0;close_owner(c);maybe_free(c);}

int t_ring_fixed_reserve(struct t_ring_client *client,uint64_t cookie,struct t_loom_service_ref scope,struct t_loom_service_pool_member range,int write,struct t_ring_fixed **out){
 struct ring_core *c=client->core;assert(c->client_alive&&out);*out=NULL;if(c->closing)return T_LEASE_STATE;
 size_t i=range.buffer_index;
 if(!cookie||scope.slot>=64||scope.reserved||!scope.incarnation||range.reserved||!range.length||range.length>INT32_MAX||i>=c->b.count(c->b.context)||range.length>UINT64_MAX-range.offset||range.offset+range.length>c->b.length(c->b.context,i))return T_LEASE_GEOMETRY;
 if(c->fixed[i].phase!=FIX_FREE)return T_LEASE_BUSY;
 for(size_t j=0;j<64;++j){if(c->fixed[j].phase!=FIX_FREE&&c->fixed[j].cookie==cookie)return T_LEASE_BUSY;struct pool_state *p=c->pools[j];if(p)for(size_t k=0;k<p->count;++k){if(0 && fixed_overlap(p->members[k].range,range))return T_LEASE_GEOMETRY;}}
 struct t_ring_fixed *f=&c->fixed[i];*f=(struct t_ring_fixed){.core=c,.range=range,.scope=scope,.cookie=cookie,.phase=FIX_PREPARED,.write=!!write,.alive=1};++c->fixed_handles;*out=f;return 0;
}
int t_ring_fixed_write(struct t_ring_fixed *f,const void *bytes,size_t len){
 assert(f->alive);struct ring_core *c=f->core;if(c->closing)return T_LEASE_STATE;
 if(!f->write||f->phase!=FIX_PREPARED)return T_LEASE_BUSY;
 if(!bytes||len!=f->range.length)return T_LEASE_GEOMETRY;
 memcpy((unsigned char *)c->b.buffer(c->b.context,f->range.buffer_index)+(size_t)f->range.offset,bytes,len);return 0;
}
int t_ring_fixed_publish(struct t_ring_fixed *f){assert(f->alive);if(f->core->closing)return T_LEASE_STATE;if(f->phase!=FIX_PREPARED)return T_LEASE_BUSY;f->phase=FIX_ACTIVE;return 0;}
/* Cancellation requests scope abort; only CQ/retirement proves reuse safe. */
static int cancel_fixed(struct t_ring_fixed *f){
 struct ring_core *c=f->core;if(c->closed)return 0;
 if(f->phase==FIX_PREPARED)return T_LEASE_BUSY;
 if(f->phase!=FIX_ACTIVE||f->abort_requested)return 0;
 f->abort_requested=1;c->b.abort_scope(c->b.context,f->scope);return 1;
}
int t_ring_fixed_cancel(struct t_ring_fixed *f){assert(f->alive);return cancel_fixed(f);}
int t_ring_fixed_deadline(struct t_ring_fixed *f,int enabled,uint64_t deadline){
 assert(f->alive);if(f->core->closing)return T_LEASE_STATE;
 if(f->phase!=FIX_PREPARED)return T_LEASE_BUSY;
 f->has_deadline=!!enabled;f->deadline=deadline;return 0;
}
int t_ring_next_deadline(const struct t_ring_owner *root,uint64_t *out){
 const struct ring_core *c=root->core;assert(c->root_alive&&out);*out=0;if(c->closed)return 0;
 int found=0;for(size_t i=0;i<64;++i){const struct t_ring_fixed *f=&c->fixed[i];
  if(f->phase==FIX_ACTIVE&&f->has_deadline&&!f->abort_requested&&(!found||f->deadline<*out)){*out=f->deadline;found=1;}
 }return found;
}
size_t t_ring_expire(struct t_ring_owner *root,uint64_t now,size_t budget){
 struct ring_core *c=root->core;assert(c->root_alive);if(c->closed)return 0;
 size_t requested=0,visits=budget<64?budget:64;
 for(size_t n=0;n<visits;++n){size_t i=c->deadline_cursor;c->deadline_cursor=(i+1)%64;
  struct t_ring_fixed *f=&c->fixed[i];
  if(f->phase==FIX_ACTIVE&&f->has_deadline&&!f->abort_requested&&now>=f->deadline){requested+=(cancel_fixed(f)==1);}
 }return requested;
}
int t_ring_fixed_finish(struct t_ring_owner *root,uint64_t cookie,int32_t result){
 struct ring_core *c=root->core;assert(c->root_alive);
 for(size_t i=0;i<64;++i){struct t_ring_fixed *f=&c->fixed[i];if(f->phase==FIX_FREE||f->cookie!=cookie)continue;
  if(f->phase!=FIX_ACTIVE)return T_LEASE_BUSY;
  if(result>0&&(uint64_t)result>f->range.length)return T_LEASE_GEOMETRY;
  if(f->abandoned){f->phase=FIX_FREE;}else{f->phase=FIX_TERMINAL;f->result=result;}return 0;
 }return T_LEASE_STALE;
}
int64_t t_ring_fixed_bytes(const struct t_ring_fixed *f,const unsigned char **out,size_t *len){
 assert(f->alive&&out&&len);*out=NULL;*len=0;if(f->write||f->phase!=FIX_TERMINAL)return T_LEASE_BUSY;if(f->result<0)return f->result;
 *out=f->core->b.buffer(f->core->b.context,f->range.buffer_index)+(size_t)f->range.offset;*len=(size_t)f->result;return 0;
}
void t_ring_fixed_drop(struct t_ring_fixed *f){
 assert(f->alive);struct ring_core *c=f->core;f->alive=0;--c->fixed_handles;
 if(f->phase==FIX_ACTIVE){f->abandoned=1;(void)cancel_fixed(f);}else{f->phase=FIX_FREE;}maybe_free(c);
}
size_t t_ring_fixed_count(const struct t_ring_owner *root){struct ring_core *c=root->core;assert(c->root_alive);size_t n=0;for(size_t i=0;i<64;++i)n+=c->fixed[i].phase!=FIX_FREE;return n;}

void t_ring_close(struct t_ring_owner *root){assert(root->core->root_alive);close_owner(root->core);}
