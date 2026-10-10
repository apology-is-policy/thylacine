#include "payload_owner.h"
#include <stdlib.h>
#include <assert.h>
struct core;
struct t_payload_owner {struct core *core;};
struct t_payload_pool {struct core *core;};
struct t_payload_lease {struct core *core;struct t_lease_token token;unsigned active,pending;};
struct core {
 struct t_payload_backend backend;struct t_lease_ledger ledger;
 struct t_loom_service_pool_member ranges[64];struct t_payload_lease leases[64];
 size_t live;unsigned closing,closed,root_alive,pool_alive;
 struct t_payload_owner root;struct t_payload_pool pool;
};
static void retire(struct core *c){if(!c->closing){c->closing=1;c->backend.begin_retire(c->backend.context);}}
static void close_owner(struct core *c){retire(c);if(!c->closed){c->closed=1;c->backend.close(c->backend.context);}}
static void maybe_free(struct core *c){if(!c->root_alive&&!c->pool_alive&&!c->live){close_owner(c);c->backend.destroy(c->backend.context);free(c);}}
static void failed_create(struct t_payload_backend b){b.begin_retire(b.context);b.close(b.context);b.destroy(b.context);}
int t_payload_create(struct t_payload_backend b,struct t_loom_service_ref pool,
 const struct t_loom_service_pool_member *ranges,size_t n,struct t_payload_owner **root,struct t_payload_pool **client){
 assert(root&&client&&b.count&&b.length&&b.buffer&&b.return_buffer&&b.begin_retire&&b.retired&&b.close&&b.destroy);
 *root=NULL;*client=NULL;
 size_t count=b.count(b.context);uint64_t lengths[64];
 if(!count||count>64||!ranges||!n||n>64){failed_create(b);return T_LEASE_GEOMETRY;}
 for(size_t i=0;i<count;++i){size_t len=b.length(b.context,i);if(!len||len>PTRDIFF_MAX){failed_create(b);return T_LEASE_GEOMETRY;}lengths[i]=len;}
 struct core *c=calloc(1,sizeof(*c));if(!c){failed_create(b);return -12;}
 c->backend=b;int rc=t_lease_init(&c->ledger,pool,ranges,n,lengths,count);
 if(rc){failed_create(b);free(c);return rc;}
 for(size_t i=0;i<n;++i){c->ranges[i]=ranges[i];c->leases[i].core=c;}
 c->root_alive=1;c->pool_alive=1;c->root.core=c;c->pool.core=c;
 *root=&c->root;*client=&c->pool;return 0;
}
int t_payload_accept(struct t_payload_pool *pool,int32_t result,uint32_t flags,struct t_loom_service_buffer_receipt receipt,struct t_payload_lease **out){
 struct core *c=pool->core;assert(c->pool_alive&&out);*out=NULL;if(c->closing)return T_LEASE_STATE;
 struct t_lease_token token;int rc=t_lease_accept_read(&c->ledger,result,flags,receipt,&token);if(rc<=0)return rc;
 struct t_payload_lease *l=&c->leases[receipt.member];assert(!l->active&&!l->pending);l->token=token;l->active=1;++c->live;*out=l;return 1;
}
const unsigned char *t_payload_bytes(const struct t_payload_lease *l,size_t *len){assert(l->active&&len);struct core *c=l->core;struct t_loom_service_pool_member r=c->ranges[l->token.receipt.member];*len=l->token.length;return c->backend.buffer(c->backend.context,r.buffer_index)+(size_t)r.offset;}
static int64_t attempt(struct t_payload_lease *l){struct core *c=l->core;if(t_lease_begin_return(&c->ledger,&l->token))abort();int64_t rc=c->backend.return_buffer(c->backend.context,l->token.receipt);if(t_lease_finish_return(&c->ledger,&l->token,rc))abort();return rc;}
int64_t t_payload_return(struct t_payload_lease *l){assert(l->active);struct core *c=l->core;int64_t rc=attempt(l);if(!rc){l->active=0;--c->live;maybe_free(c);}return rc;}
void t_payload_abandon(struct t_payload_lease *l){assert(l->active);struct core *c=l->core;int64_t rc=attempt(l);l->active=0;l->pending=rc!=0;--c->live;maybe_free(c);}
void t_payload_pool_drop(struct t_payload_pool *p){struct core *c=p->core;assert(c->pool_alive);c->pool_alive=0;retire(c);maybe_free(c);}
int t_payload_progress(struct t_payload_owner *r){struct core *c=r->core;assert(c->root_alive);for(size_t i=0;i<64;++i){struct t_payload_lease *l=&c->leases[i];if(l->pending&&!attempt(l))l->pending=0;}return c->closing&&!c->live&&c->backend.retired(c->backend.context);}
size_t t_payload_outstanding(const struct t_payload_owner *r){assert(r->core->root_alive);return r->core->ledger.outstanding;}
size_t t_payload_pending(const struct t_payload_owner *r){struct core *c=r->core;assert(c->root_alive);size_t n=0;for(size_t i=0;i<64;++i)n+=c->leases[i].pending;return n;}
void t_payload_owner_drop(struct t_payload_owner *r){struct core *c=r->core;assert(c->root_alive);c->root_alive=0;close_owner(c);maybe_free(c);}
