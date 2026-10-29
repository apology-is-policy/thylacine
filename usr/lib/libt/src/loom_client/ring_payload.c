#include <thyla/loom_client/ring_payload.h>
enum fixed_phase{FIX_FREE,FIX_PREPARED,FIX_ACTIVE,FIX_TERMINAL};
static void retire(struct t_ring_core *c){if(!c->closing){c->closing=1;c->b.begin_retire(c->b.context);}}
static void close_owner(struct t_ring_core *c){retire(c);if(!c->closed){c->closed=1;c->b.close(c->b.context);}}
/* Zeroing returns every pool header, bank cell and lease with the storage. */
static void maybe_free(struct t_ring_core *c){if(!c->root_alive&&!c->client_alive&&!c->pool_handles&&!c->fixed_handles&&!c->live){close_owner(c);c->b.destroy(c->b.context);t_bytes_zero(c,sizeof(*c));}}
/* A free header exists: each claimed one holds at least one of the members
 * counted in c->members, and prepare admits only n <= 64-members. */
static struct t_ring_pool_state *claim_pool(struct t_ring_core *c){for(size_t i=0;i<64;++i){struct t_ring_pool_state *p=&c->pool_cells[i];if(!p->used){p->used=1;p->core=c;return p;}}t_loom_client_fatal(__func__,__LINE__);}
static void release_pool(struct t_ring_pool_state *p){struct t_ring_core *c=p->core;for(size_t i=0;i<p->ledger.count;++i)c->leases[t_lease_cell(&p->ledger,(uint32_t)i)]=(struct t_ring_payload){0};t_lease_release(&p->ledger);t_bytes_zero(p,sizeof(*p));}
static struct t_loom_service_pool_member member_range(const struct t_ring_pool_state *p,size_t i){return p->core->bank.cells[t_lease_cell(&p->ledger,(uint32_t)i)].range;}
static void failed_create(struct t_ring_backend b){b.begin_retire(b.context);b.close(b.context);b.destroy(b.context);}
int t_ring_create(struct t_ring_backend b,struct t_ring_core *c,struct t_ring_owner **root,struct t_ring_client **client){T_LOOM_CHECK(c&&root&&client&&b.count&&b.length&&b.buffer&&b.return_buffer&&b.retire_pool&&b.destroy_pool&&b.begin_retire&&b.retired&&b.close&&b.destroy&&b.register_pool&&b.abort_scope);T_LOOM_CHECK(!c->in_use);*root=NULL;*client=NULL;size_t n=b.count(b.context);if(!n||n>64){failed_create(b);return T_LEASE_GEOMETRY;}for(size_t i=0;i<n;++i){size_t len=b.length(b.context,i);if(!len||len>PTRDIFF_MAX){failed_create(b);return T_LEASE_GEOMETRY;}}t_bytes_zero(c,sizeof(*c));c->in_use=1;c->b=b;c->root_alive=1;c->client_alive=1;c->root.core=c;c->client.core=c;*root=&c->root;*client=&c->client;return 0;}
int t_ring_idle(const struct t_ring_core *c){return !c->in_use;}
static int identity_ok(struct t_ring_core *c,struct t_loom_service_ref identity){
 if(identity.reserved||!identity.incarnation||identity.slot>=64)return T_LEASE_GEOMETRY;
 unsigned slot=identity.slot;if(c->pools[slot])return T_LEASE_BUSY;
 if(identity.incarnation<=c->last[slot])return T_LEASE_STALE;
 return 0;
}
static int fixed_overlap(struct t_loom_service_pool_member a,struct t_loom_service_pool_member b){return a.buffer_index==b.buffer_index&&a.offset<b.offset+b.length&&b.offset<a.offset+a.length;}
static int prepare(struct t_ring_core *c,const struct t_loom_service_pool_member *ranges,size_t n,struct t_ring_pool_state **out){
 if(c->closing)return T_LEASE_STATE;
 if(!ranges||!n||n>64)return T_LEASE_GEOMETRY;
 if(n>64-c->members)return T_LOOM_CLIENT_ENOSPC;
 uint64_t lengths[64];size_t count=c->b.count(c->b.context);for(size_t i=0;i<count;++i)lengths[i]=c->b.length(c->b.context,i);
 struct t_ring_pool_state *p=claim_pool(c);
 int rc=t_lease_prepare(&p->ledger,&c->bank,ranges,n,lengths,count);if(rc){T_LOOM_CHECK(rc!=T_LOOM_CLIENT_ENOSPC);t_bytes_zero(p,sizeof(*p));return rc;}
 for(size_t i=0;i<64;++i){struct t_ring_pool_state *q=c->pools[i];if(!q)continue;for(size_t j=0;j<q->count;++j){struct t_loom_service_pool_member a=member_range(q,j);for(size_t k=0;k<n;++k){struct t_loom_service_pool_member b=ranges[k];if(a.buffer_index==b.buffer_index&&a.offset<b.offset+b.length&&b.offset<a.offset+a.length){release_pool(p);return T_LEASE_GEOMETRY;}}}}
 for(size_t i=0;i<64;++i){if(c->fixed[i].phase==FIX_FREE)continue;for(size_t j=0;j<n;++j){if(fixed_overlap(c->fixed[i].range,ranges[j])){release_pool(p);return T_LEASE_GEOMETRY;}}}
 p->count=n;p->handle.pool=p;for(size_t i=0;i<n;++i)c->leases[t_lease_cell(&p->ledger,(uint32_t)i)].pool=p;*out=p;return 0;
}
static void publish(struct t_ring_core *c,struct t_ring_pool_state *p,struct t_loom_service_ref identity,struct t_ring_pool **out){
 T_LOOM_CHECK(!t_lease_bind(&p->ledger,identity));
 p->identity=identity;p->handle_alive=1;c->pools[identity.slot]=p;c->last[identity.slot]=identity.incarnation;++c->pool_handles;*out=&p->handle;
}
int64_t t_ring_register_pool(struct t_ring_client *client,const struct t_loom_service_pool_member *ranges,size_t n,struct t_ring_pool **out,int64_t *native_rc){
 struct t_ring_core *c=client->core;T_LOOM_CHECK(c->client_alive&&out&&native_rc);*out=NULL;*native_rc=0;
 struct t_ring_pool_state *p=NULL;int rc=prepare(c,ranges,n,&p);if(rc)return rc;
 c->members+=n;struct t_loom_service_ref identity={0};
 /* Pass an immutable contiguous copy; bank cells are not ABI input. */
 struct t_loom_service_pool_member copy[64];for(size_t i=0;i<n;++i)copy[i]=member_range(p,i);
 int64_t result=c->b.register_pool(c->b.context,copy,n,&identity);*native_rc=result;
 if(result<0&&!identity.slot&&!identity.reserved&&!identity.incarnation){c->members-=n;release_pool(p);return result;}
 if(result!=0||identity_ok(c,identity)){
  p->identity=identity;p->closing=1;c->quarantine=p;close_owner(c);return T_RING_UNCERTAIN;
 }
 publish(c,p,identity,out);return 0;
}
int t_ring_attach(struct t_ring_client *client,struct t_loom_service_ref identity,const struct t_loom_service_pool_member *ranges,size_t n,struct t_ring_pool **out){
 struct t_ring_core *c=client->core;T_LOOM_CHECK(c->client_alive&&out);*out=NULL;if(c->closing)return T_LEASE_STATE;
 int rc=identity_ok(c,identity);if(rc)return rc;struct t_ring_pool_state *p=NULL;rc=prepare(c,ranges,n,&p);if(rc)return rc;
 c->members+=n;publish(c,p,identity,out);return 0;
}
int t_ring_accept(struct t_ring_pool *h,int32_t result,uint32_t flags,struct t_loom_service_buffer_receipt receipt,struct t_ring_payload **out){struct t_ring_pool_state *p=h->pool;struct t_ring_core *c=p->core;T_LOOM_CHECK(p->handle_alive&&out);*out=NULL;if(c->closing||p->closing)return T_LEASE_STATE;struct t_lease_token token;int rc=t_lease_accept_read(&p->ledger,result,flags,receipt,&token);if(rc<=0)return rc;struct t_ring_payload *l=&c->leases[t_lease_cell(&p->ledger,receipt.member)];T_LOOM_CHECK(!l->active&&!l->pending);l->token=token;l->active=1;++p->live;++c->live;*out=l;return 1;}
int t_ring_refuse(struct t_ring_pool *h,int32_t result,uint32_t flags,struct t_loom_service_buffer_receipt receipt,struct t_loom_service_buffer_receipt *out){struct t_ring_pool_state *p=h->pool;T_LOOM_CHECK(p->handle_alive&&out);return t_lease_refuse_read(&p->ledger,result,flags,receipt,out);}
struct t_loom_service_ref t_ring_pool_identity(const struct t_ring_pool *h){T_LOOM_CHECK(h->pool->handle_alive);return h->pool->identity;}
void t_ring_owner_abort_scope(struct t_ring_owner *root,struct t_loom_service_ref scope){struct t_ring_core *c=root->core;T_LOOM_CHECK(c->root_alive);if(!c->closed)c->b.abort_scope(c->b.context,scope);}
const unsigned char *t_ring_bytes(const struct t_ring_payload *l,size_t *n){T_LOOM_CHECK(l->active&&n);struct t_ring_core *c=l->pool->core;struct t_loom_service_pool_member r=c->bank.cells[l-c->leases].range;*n=l->token.length;return c->b.buffer(c->b.context,r.buffer_index)+(size_t)r.offset;}
static int64_t attempt(struct t_ring_payload *l){struct t_ring_pool_state *p=l->pool;struct t_ring_core *c=p->core;T_LOOM_CHECK(!t_lease_begin_return(&p->ledger,&l->token));int64_t rc=c->b.return_buffer(c->b.context,l->token.receipt);T_LOOM_CHECK(!t_lease_finish_return(&p->ledger,&l->token,rc));return rc;}
int64_t t_ring_return(struct t_ring_payload *l){T_LOOM_CHECK(l->active);struct t_ring_pool_state *p=l->pool;struct t_ring_core *c=p->core;int64_t rc=attempt(l);if(!rc){l->active=0;--p->live;--c->live;maybe_free(c);}return rc;}
void t_ring_abandon(struct t_ring_payload *l){T_LOOM_CHECK(l->active);struct t_ring_pool_state *p=l->pool;struct t_ring_core *c=p->core;int64_t rc=attempt(l);l->active=0;l->pending=rc!=0;--p->live;--c->live;maybe_free(c);}
void t_ring_pool_drop(struct t_ring_pool *h){struct t_ring_pool_state *p=h->pool;struct t_ring_core *c=p->core;T_LOOM_CHECK(p->handle_alive);p->handle_alive=0;p->closing=1;--c->pool_handles;c->b.retire_pool(c->b.context,p->identity);maybe_free(c);}
void t_ring_client_drop(struct t_ring_client *h){struct t_ring_core *c=h->core;T_LOOM_CHECK(c->client_alive);c->client_alive=0;retire(c);maybe_free(c);}
int t_ring_progress(struct t_ring_owner *h){struct t_ring_core *c=h->core;T_LOOM_CHECK(c->root_alive);for(size_t i=0;i<64;++i){struct t_ring_pool_state *p=c->pools[i];if(!p)continue;for(size_t j=0;j<p->count;++j){struct t_ring_payload *l=&c->leases[t_lease_cell(&p->ledger,(uint32_t)j)];if(l->pending&&!attempt(l))l->pending=0;}if(p->closing&&!p->live&&!p->ledger.outstanding){if(c->b.destroy_pool(c->b.context,p->identity)==0){c->members-=p->count;c->pools[i]=NULL;release_pool(p);}}}int done=c->b.retired(c->b.context);size_t occupied=0;for(size_t i=0;i<64;++i){struct t_ring_fixed *f=&c->fixed[i];if(done&&f->abandoned)f->phase=FIX_FREE;occupied+=f->phase!=FIX_FREE;}return c->closing&&!c->live&&!occupied&&done;}
size_t t_ring_members(const struct t_ring_owner *h){T_LOOM_CHECK(h->core->root_alive);return h->core->members;}
size_t t_ring_live(const struct t_ring_owner *h){T_LOOM_CHECK(h->core->root_alive);return h->core->live;}
size_t t_ring_pending(const struct t_ring_owner *h){struct t_ring_core *c=h->core;T_LOOM_CHECK(c->root_alive);size_t n=0;for(size_t i=0;i<64;++i){struct t_ring_pool_state *p=c->pools[i];if(p)for(size_t j=0;j<p->count;++j)n+=c->leases[t_lease_cell(&p->ledger,(uint32_t)j)].pending;}return n;}
void t_ring_owner_drop(struct t_ring_owner *h){struct t_ring_core *c=h->core;T_LOOM_CHECK(c->root_alive);c->root_alive=0;close_owner(c);maybe_free(c);}

int t_ring_fixed_reserve(struct t_ring_client *client,uint64_t cookie,struct t_loom_service_ref scope,struct t_loom_service_pool_member range,int write,struct t_ring_fixed **out){
 struct t_ring_core *c=client->core;T_LOOM_CHECK(c->client_alive&&out);*out=NULL;if(c->closing)return T_LEASE_STATE;
 size_t i=range.buffer_index;
 if(!cookie||scope.slot>=64||scope.reserved||!scope.incarnation||range.reserved||!range.length||range.length>INT32_MAX||i>=c->b.count(c->b.context)||range.length>UINT64_MAX-range.offset||range.offset+range.length>c->b.length(c->b.context,i))return T_LEASE_GEOMETRY;
 if(c->fixed[i].phase!=FIX_FREE)return T_LEASE_BUSY;
 for(size_t j=0;j<64;++j){if(c->fixed[j].phase!=FIX_FREE&&c->fixed[j].cookie==cookie)return T_LEASE_BUSY;struct t_ring_pool_state *p=c->pools[j];if(p)for(size_t k=0;k<p->count;++k){if(fixed_overlap(member_range(p,k),range))return T_LEASE_GEOMETRY;}}
 struct t_ring_fixed *f=&c->fixed[i];*f=(struct t_ring_fixed){.core=c,.range=range,.scope=scope,.cookie=cookie,.phase=FIX_PREPARED,.write=!!write,.alive=1};++c->fixed_handles;*out=f;return 0;
}
int t_ring_fixed_write(struct t_ring_fixed *f,const void *bytes,size_t len){
 T_LOOM_CHECK(f->alive);struct t_ring_core *c=f->core;if(c->closing)return T_LEASE_STATE;
 if(!f->write||f->phase!=FIX_PREPARED)return T_LEASE_BUSY;
 if(!bytes||len!=f->range.length)return T_LEASE_GEOMETRY;
 t_bytes_copy((unsigned char *)c->b.buffer(c->b.context,f->range.buffer_index)+(size_t)f->range.offset,bytes,len);return 0;
}
int t_ring_fixed_publish(struct t_ring_fixed *f){T_LOOM_CHECK(f->alive);if(f->core->closing)return T_LEASE_STATE;if(f->phase!=FIX_PREPARED)return T_LEASE_BUSY;f->phase=FIX_ACTIVE;return 0;}
/* Cancellation requests scope abort; only CQ/retirement proves reuse safe. */
static int cancel_fixed(struct t_ring_fixed *f){
 struct t_ring_core *c=f->core;if(c->closed)return 0;
 if(f->phase==FIX_PREPARED)return T_LEASE_BUSY;
 if(f->phase!=FIX_ACTIVE||f->abort_requested)return 0;
 f->abort_requested=1;c->b.abort_scope(c->b.context,f->scope);return 1;
}
int t_ring_fixed_cancel(struct t_ring_fixed *f){T_LOOM_CHECK(f->alive);return cancel_fixed(f);}
int t_ring_fixed_deadline(struct t_ring_fixed *f,int enabled,uint64_t deadline){
 T_LOOM_CHECK(f->alive);if(f->core->closing)return T_LEASE_STATE;
 if(f->phase!=FIX_PREPARED)return T_LEASE_BUSY;
 f->has_deadline=!!enabled;f->deadline=deadline;return 0;
}
int t_ring_next_deadline(const struct t_ring_owner *root,uint64_t *out){
 const struct t_ring_core *c=root->core;T_LOOM_CHECK(c->root_alive&&out);*out=0;if(c->closed)return 0;
 int found=0;for(size_t i=0;i<64;++i){const struct t_ring_fixed *f=&c->fixed[i];
  if(f->phase==FIX_ACTIVE&&f->has_deadline&&!f->abort_requested&&(!found||f->deadline<*out)){*out=f->deadline;found=1;}
 }return found;
}
size_t t_ring_expire(struct t_ring_owner *root,uint64_t now,size_t budget){
 struct t_ring_core *c=root->core;T_LOOM_CHECK(c->root_alive);if(c->closed)return 0;
 size_t requested=0,visits=budget<64?budget:64;
 for(size_t n=0;n<visits;++n){size_t i=c->deadline_cursor;c->deadline_cursor=(i+1)%64;
  struct t_ring_fixed *f=&c->fixed[i];
  if(f->phase==FIX_ACTIVE&&f->has_deadline&&!f->abort_requested&&now>=f->deadline){requested+=(cancel_fixed(f)==1);}
 }return requested;
}
int t_ring_fixed_finish(struct t_ring_owner *root,uint64_t cookie,int32_t result){
 struct t_ring_core *c=root->core;T_LOOM_CHECK(c->root_alive);
 for(size_t i=0;i<64;++i){struct t_ring_fixed *f=&c->fixed[i];if(f->phase==FIX_FREE||f->cookie!=cookie)continue;
  if(f->phase!=FIX_ACTIVE)return T_LEASE_BUSY;
  if(result>0&&(uint64_t)result>f->range.length)return T_LEASE_GEOMETRY;
  if(f->abandoned){f->phase=FIX_FREE;}else{f->phase=FIX_TERMINAL;f->result=result;}return 0;
 }return T_LEASE_STALE;
}
int64_t t_ring_fixed_bytes(const struct t_ring_fixed *f,const unsigned char **out,size_t *len){
 T_LOOM_CHECK(f->alive&&out&&len);*out=NULL;*len=0;if(f->write||f->phase!=FIX_TERMINAL)return T_LEASE_BUSY;if(f->result<0)return f->result;
 *out=f->core->b.buffer(f->core->b.context,f->range.buffer_index)+(size_t)f->range.offset;*len=(size_t)f->result;return 0;
}
void t_ring_fixed_drop(struct t_ring_fixed *f){
 T_LOOM_CHECK(f->alive);struct t_ring_core *c=f->core;f->alive=0;--c->fixed_handles;
 if(f->phase==FIX_ACTIVE){f->abandoned=1;(void)cancel_fixed(f);}else{f->phase=FIX_FREE;}maybe_free(c);
}
size_t t_ring_fixed_count(const struct t_ring_owner *root){struct t_ring_core *c=root->core;T_LOOM_CHECK(c->root_alive);size_t n=0;for(size_t i=0;i<64;++i)n+=c->fixed[i].phase!=FIX_FREE;return n;}

void t_ring_close(struct t_ring_owner *root){T_LOOM_CHECK(root->core->root_alive);close_owner(root->core);}
