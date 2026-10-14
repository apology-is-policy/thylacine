#include <thyla/loom_client/fixed_requests.h>
static void retire(struct t_fixed_driver *d){if(d->client){t_ring_client_drop(d->client);d->client=NULL;}}
static void fail(struct t_fixed_driver *d){d->failed=1;retire(d);t_ring_close(d->owner);}
static void unref(struct t_fixed_driver *d){T_LOOM_CHECK(d->refs);if(--d->refs==0){retire(d);t_ring_owner_drop(d->owner);t_request_destroy(d->book);}}
/* A free cell exists: each claimed one holds a fixed handle of its own, and the
 * caller has just reserved another; a ring has 64 fixed cells. */
static struct t_fixed_request *claim(struct t_fixed_driver *d){for(size_t i=0;i<64;++i){struct t_fixed_request *r=&d->storage->requests[i];if(!r->used){r->used=1;return r;}}t_loom_client_fatal(__func__,__LINE__);}
static void release_request(struct t_fixed_request *r){t_bytes_zero(r,sizeof(*r));}
static void consume_failed(struct t_ring_backend b){b.begin_retire(b.context);b.close(b.context);b.destroy(b.context);}
int t_fixed_create(struct t_ring_backend b,struct t_fixed_storage *s,struct t_request_entry *entries,size_t capacity,struct t_fixed_driver **out){
 T_LOOM_CHECK(s&&out);*out=NULL;T_LOOM_CHECK(t_fixed_storage_idle(s));struct t_fixed_driver *d=&s->driver;
 t_bytes_zero(d,sizeof(*d));t_bytes_zero(s->requests,sizeof(s->requests));d->storage=s;d->book=&s->book;
 int rc=t_request_create(d->book,entries,capacity);if(rc){consume_failed(b);return rc;}
 rc=t_ring_create(b,&s->ring,&d->owner,&d->client);if(rc){t_request_destroy(d->book);return rc;}
 d->refs=1;d->alive=1;*out=d;return 0;
}
int t_fixed_storage_idle(const struct t_fixed_storage *s){
 if(s->driver.refs||!t_ring_idle(&s->ring))return 0;
 for(size_t i=0;i<64;++i)if(s->requests[i].used)return 0;return 1;
}
static int prepare(struct t_fixed_driver *d,struct t_loom_service_ref scope,struct t_loom_service_pool_member range,const void *input,size_t len,unsigned write,struct t_fixed_request **out){
 T_LOOM_CHECK(d->alive&&out);*out=NULL;if(d->failed||!d->client)return T_FIXED_CLOSING;
 if(!range.length||range.length>INT32_MAX)return T_LEASE_GEOMETRY;
 struct t_request_spec spec={.kind=write?T_REQ_FIXED_WRITE:T_REQ_FIXED_READ,.limit=(uint32_t)range.length};
 uint64_t cookie=0;int rc=t_request_reserve(d->book,scope,spec,&cookie);if(rc)return rc;
 struct t_ring_fixed *fixed=NULL;rc=t_ring_fixed_reserve(d->client,cookie,scope,range,write,&fixed);
 if(!rc&&write)rc=t_ring_fixed_write(fixed,input,len);
 if(rc){T_LOOM_CHECK(!t_request_cancel_unpublished(d->book,cookie));if(fixed)t_ring_fixed_drop(fixed);return rc;}
 struct t_fixed_request *r=claim(d);r->driver=d;r->fixed=fixed;r->cookie=cookie;r->write=write;++d->refs;*out=r;return 0;
}
int t_fixed_read(struct t_fixed_driver *d,struct t_loom_service_ref scope,struct t_loom_service_pool_member range,struct t_fixed_request **out){return prepare(d,scope,range,NULL,0,0,out);}
int t_fixed_write(struct t_fixed_driver *d,struct t_loom_service_ref scope,struct t_loom_service_pool_member range,const void *input,size_t len,struct t_fixed_request **out){return prepare(d,scope,range,input,len,1,out);}
int t_fixed_publish(struct t_fixed_request *r,uint64_t *cookie){
 T_LOOM_CHECK(r->driver&&cookie);*cookie=0;struct t_fixed_driver *d=r->driver;if(d->failed||!d->client)return T_FIXED_CLOSING;if(r->published)return T_REQ_STATE;
 int rc=t_ring_fixed_publish(r->fixed);if(rc)return rc;
 T_LOOM_CHECK(!t_request_publish(d->book,r->cookie));r->published=1;*cookie=r->cookie;return 0;
}
int t_fixed_route(struct t_fixed_driver *d,const struct t_paired_record *record,struct t_fixed_routed *out){
 T_LOOM_CHECK(d->alive&&record&&out);if(d->failed)return T_FIXED_CLOSING;struct t_request_delivery delivery;
 int rc=t_request_accept(d->book,record,&delivery);if(rc){fail(d);return rc;}
 struct t_owned_cqe ignored;unsigned take=delivery.abandoned;enum t_fixed_route_kind kind;
 switch(delivery.spec.kind){
 case T_REQ_FIXED_READ:case T_REQ_FIXED_WRITE:
  rc=t_ring_fixed_finish(d->owner,delivery.cookie,delivery.record.cqe.result);if(rc){fail(d);return rc;}
  kind=T_ROUTED_FIXED;break;
 case T_REQ_CONTROL:case T_REQ_SLOT:kind=T_ROUTED_CONTROL;break;
 default:take=delivery.terminal&&delivery.abandoned;kind=T_ROUTED_POOLED;break;
 }
 if(take)T_LOOM_CHECK(!t_request_take_terminal(d->book,delivery.cookie,&ignored));
 *out=(struct t_fixed_routed){.kind=kind,.cookie=delivery.cookie,.delivery=delivery};return 0;
}
int t_fixed_receive(struct t_fixed_driver *d,const struct t_paired_record *record){struct t_fixed_routed routed;return t_fixed_route(d,record,&routed);}
int t_fixed_book_reserve(struct t_fixed_driver *d,struct t_loom_service_ref scope,struct t_request_spec spec,struct t_fixed_book *out){
 T_LOOM_CHECK(d->alive&&out);*out=(struct t_fixed_book){0};if(d->failed||!d->client)return T_FIXED_CLOSING;
 if(spec.kind==T_REQ_FIXED_READ||spec.kind==T_REQ_FIXED_WRITE)return T_REQ_GEOMETRY;
 uint64_t cookie=0;int rc=t_request_reserve(d->book,scope,spec,&cookie);if(rc)return rc;
 out->driver=d;out->cookie=cookie;++d->refs;return 0;
}
int t_fixed_book_publish(struct t_fixed_book *q,uint64_t *cookie){
 T_LOOM_CHECK(q->driver&&cookie);*cookie=0;struct t_fixed_driver *d=q->driver;if(d->failed||!d->client)return T_FIXED_CLOSING;if(q->published)return T_REQ_STATE;
 T_LOOM_CHECK(!t_request_publish(d->book,q->cookie));q->published=1;*cookie=q->cookie;return 0;
}
int t_fixed_book_take(struct t_fixed_book *q,struct t_owned_cqe *out){
 struct t_fixed_driver *d=q->driver;T_LOOM_CHECK(d&&out);if(d->failed)return T_FIXED_CLOSING;if(!q->published)return d->client?0:T_FIXED_CLOSING;
 int rc=t_request_take_terminal(d->book,q->cookie,out);if(rc==T_REQ_STATE)return d->client?0:T_FIXED_CLOSING;if(rc)return rc;
 q->driver=NULL;unref(d);return 1;
}
void t_fixed_book_drop(struct t_fixed_book *q){
 struct t_fixed_driver *d=q->driver;T_LOOM_CHECK(d);q->driver=NULL;
 if(!q->published){T_LOOM_CHECK(!t_request_cancel_unpublished(d->book,q->cookie));}
 else{struct t_owned_cqe ignored;int rc=t_request_take_terminal(d->book,q->cookie,&ignored);
  if(rc==T_REQ_STATE){struct t_loom_service_ref scope;int first=t_request_abandon(d->book,q->cookie,&scope);T_LOOM_CHECK(first>=0);if(first==1)t_ring_owner_abort_scope(d->owner,scope);}
  else if(rc)fail(d);}
 unref(d);
}
int64_t t_fixed_register_pool(struct t_fixed_driver *d,const struct t_loom_service_pool_member *ranges,size_t n,struct t_ring_pool **out,int64_t *native_rc){
 T_LOOM_CHECK(d->alive&&out&&native_rc);*out=NULL;*native_rc=0;if(d->failed||!d->client)return T_FIXED_CLOSING;
 return t_ring_register_pool(d->client,ranges,n,out,native_rc);
}
static int take(struct t_fixed_request *r,struct t_owned_cqe *out){
 struct t_fixed_driver *d=r->driver;T_LOOM_CHECK(d);if(d->failed)return T_FIXED_CLOSING;if(!r->published)return d->client?0:T_FIXED_CLOSING;
 int rc=t_request_take_terminal(d->book,r->cookie,out);if(rc==T_REQ_STATE)return d->client?0:T_FIXED_CLOSING;return rc?rc:1;
}
int t_fixed_read_complete(struct t_fixed_request *r,struct t_fixed_read_result **out){
 T_LOOM_CHECK(out);*out=NULL;if(r->write)return T_REQ_STATE;struct t_owned_cqe cqe;int rc=take(r,&cqe);if(rc!=1)return rc;
 r->result=(struct t_fixed_read_result){r,r->fixed,cqe};r->fixed=NULL;struct t_fixed_driver *d=r->driver;r->driver=NULL;*out=&r->result;unref(d);return 1;
}
int t_fixed_write_complete(struct t_fixed_request *r,struct t_owned_cqe *out){
 T_LOOM_CHECK(out);if(!r->write)return T_REQ_STATE;int rc=take(r,out);if(rc!=1)return rc;
 struct t_fixed_driver *d=r->driver;t_ring_fixed_drop(r->fixed);release_request(r);unref(d);return 1;
}
int32_t t_fixed_read_status(const struct t_fixed_read_result *r){return r->cqe.result;}
int64_t t_fixed_read_bytes(const struct t_fixed_read_result *r,const unsigned char **out,size_t *len){return t_ring_fixed_bytes(r->fixed,out,len);}
void t_fixed_read_drop(struct t_fixed_read_result *r){struct t_fixed_request *cell=r->allocation;T_LOOM_CHECK(cell&&cell->used&&&cell->result==r);t_ring_fixed_drop(r->fixed);release_request(cell);}
void t_fixed_request_drop(struct t_fixed_request *r){
 struct t_fixed_driver *d=r->driver;T_LOOM_CHECK(d);
 if(!r->published){T_LOOM_CHECK(!t_request_cancel_unpublished(d->book,r->cookie));}
 else{struct t_owned_cqe ignored;int rc=t_request_take_terminal(d->book,r->cookie,&ignored);if(rc==T_REQ_STATE){struct t_loom_service_ref scope;T_LOOM_CHECK(t_request_abandon(d->book,r->cookie,&scope)>=0);}else if(rc)fail(d);}
 /* Fixed drop dispatches the single scope abort; book abandonment does not. */
 t_ring_fixed_drop(r->fixed);release_request(r);unref(d);
}
void t_fixed_close(struct t_fixed_driver *d){T_LOOM_CHECK(d->alive);fail(d);}
void t_fixed_begin_retire(struct t_fixed_driver *d){T_LOOM_CHECK(d->alive);retire(d);}
int t_fixed_progress(struct t_fixed_driver *d){T_LOOM_CHECK(d->alive);if(t_ring_progress(d->owner)){t_request_clear_retired(d->book);return 1;}return 0;}
size_t t_fixed_occupied(const struct t_fixed_driver *d){T_LOOM_CHECK(d->alive);return t_request_occupied(d->book);}
void t_fixed_driver_drop(struct t_fixed_driver *d){T_LOOM_CHECK(d->alive);d->alive=0;retire(d);unref(d);}

int t_fixed_set_deadline(struct t_fixed_request *r,int enabled,uint64_t deadline){
 struct t_fixed_driver *d=r->driver;T_LOOM_CHECK(d);if(d->failed||!d->client)return T_FIXED_CLOSING;
 return t_ring_fixed_deadline(r->fixed,enabled,deadline);
}
int t_fixed_cancel(struct t_fixed_request *r){T_LOOM_CHECK(r->driver);return t_ring_fixed_cancel(r->fixed);}
int t_fixed_next_deadline(const struct t_fixed_driver *d,uint64_t *out){T_LOOM_CHECK(d->alive);return t_ring_next_deadline(d->owner,out);}
size_t t_fixed_expire(struct t_fixed_driver *d,uint64_t now,size_t budget){T_LOOM_CHECK(d->alive);return t_ring_expire(d->owner,now,budget);}
