#include "fixed_requests.h"
#include <stdlib.h>
#include <assert.h>
struct t_fixed_driver{struct t_ring_owner *owner;struct t_ring_client *client;struct t_request_book *book;size_t refs;unsigned failed,alive;};
struct t_fixed_read_result{struct t_fixed_request *allocation;struct t_ring_fixed *fixed;struct t_owned_cqe cqe;};
struct t_fixed_request{struct t_fixed_driver *driver;struct t_ring_fixed *fixed;uint64_t cookie;unsigned published,write;struct t_fixed_read_result result;};
static void retire(struct t_fixed_driver *d){if(d->client){t_ring_client_drop(d->client);d->client=NULL;}}
static void fail(struct t_fixed_driver *d){d->failed=1;retire(d);t_ring_close(d->owner);}
static void unref(struct t_fixed_driver *d){assert(d->refs);if(--d->refs==0){retire(d);t_ring_owner_drop(d->owner);t_request_destroy(d->book);free(d);}}
static void consume_failed(struct t_ring_backend b){b.begin_retire(b.context);b.close(b.context);b.destroy(b.context);}
int t_fixed_create(struct t_ring_backend b,size_t capacity,struct t_fixed_driver **out){
 assert(out);*out=NULL;struct t_fixed_driver *d=calloc(1,sizeof(*d));if(!d){consume_failed(b);return -12;}
 int rc=t_request_create(capacity,&d->book);if(rc){consume_failed(b);free(d);return rc;}
 rc=t_ring_create(b,&d->owner,&d->client);if(rc){t_request_destroy(d->book);free(d);return rc;}
 d->refs=1;d->alive=1;*out=d;return 0;
}
static int prepare(struct t_fixed_driver *d,struct t_loom_service_ref scope,struct t_loom_service_pool_member range,const void *input,size_t len,unsigned write,struct t_fixed_request **out){
 assert(d->alive&&out);*out=NULL;if(d->failed||!d->client)return T_FIXED_CLOSING;
 if(!range.length||range.length>INT32_MAX)return T_LEASE_GEOMETRY;
 struct t_fixed_request *r=calloc(1,sizeof(*r));if(!r)return -12;
 struct t_request_spec spec={.kind=write?T_REQ_FIXED_WRITE:T_REQ_FIXED_READ,.limit=(uint32_t)range.length};
 int rc=t_request_reserve(d->book,scope,spec,&r->cookie);if(rc){free(r);return rc;}
 rc=t_ring_fixed_reserve(d->client,r->cookie,scope,range,write,&r->fixed);
 if(!rc&&write)rc=t_ring_fixed_write(r->fixed,input,len);
 if(rc){if(t_request_cancel_unpublished(d->book,r->cookie))abort();if(r->fixed)t_ring_fixed_drop(r->fixed);free(r);return rc;}
 r->driver=d;r->write=write;++d->refs;*out=r;return 0;
}
int t_fixed_read(struct t_fixed_driver *d,struct t_loom_service_ref scope,struct t_loom_service_pool_member range,struct t_fixed_request **out){return prepare(d,scope,range,NULL,0,0,out);}
int t_fixed_write(struct t_fixed_driver *d,struct t_loom_service_ref scope,struct t_loom_service_pool_member range,const void *input,size_t len,struct t_fixed_request **out){return prepare(d,scope,range,input,len,1,out);}
int t_fixed_publish(struct t_fixed_request *r,uint64_t *cookie){
 assert(r->driver&&cookie);*cookie=0;struct t_fixed_driver *d=r->driver;if(d->failed||!d->client)return T_FIXED_CLOSING;if(r->published)return T_REQ_STATE;
 int rc=t_ring_fixed_publish(r->fixed);if(rc)return rc;
 if(t_request_publish(d->book,r->cookie))abort();r->published=1;*cookie=r->cookie;return 0;
}
int t_fixed_receive(struct t_fixed_driver *d,const struct t_paired_record *record){
 assert(d->alive&&record);if(d->failed)return T_FIXED_CLOSING;struct t_request_delivery delivery;
 int rc=t_request_accept(d->book,record,&delivery);if(rc){fail(d);return rc;}
 rc=0;if(rc){fail(d);return rc;}
 if(delivery.abandoned){struct t_owned_cqe ignored;if(t_request_take_terminal(d->book,delivery.cookie,&ignored))abort();}return 0;
}
static int take(struct t_fixed_request *r,struct t_owned_cqe *out){
 struct t_fixed_driver *d=r->driver;assert(d);if(d->failed)return T_FIXED_CLOSING;if(!r->published)return d->client?0:T_FIXED_CLOSING;
 int rc=t_request_take_terminal(d->book,r->cookie,out);if(rc==T_REQ_STATE)return d->client?0:T_FIXED_CLOSING;return rc?rc:1;
}
int t_fixed_read_complete(struct t_fixed_request *r,struct t_fixed_read_result **out){
 assert(out);*out=NULL;if(r->write)return T_REQ_STATE;struct t_owned_cqe cqe;int rc=take(r,&cqe);if(rc!=1)return rc;
 r->result=(struct t_fixed_read_result){r,r->fixed,cqe};r->fixed=NULL;struct t_fixed_driver *d=r->driver;r->driver=NULL;*out=&r->result;unref(d);return 1;
}
int t_fixed_write_complete(struct t_fixed_request *r,struct t_owned_cqe *out){
 assert(out);if(!r->write)return T_REQ_STATE;int rc=take(r,out);if(rc!=1)return rc;
 struct t_fixed_driver *d=r->driver;t_ring_fixed_drop(r->fixed);free(r);unref(d);return 1;
}
int32_t t_fixed_read_status(const struct t_fixed_read_result *r){return r->cqe.result;}
int64_t t_fixed_read_bytes(const struct t_fixed_read_result *r,const unsigned char **out,size_t *len){return t_ring_fixed_bytes(r->fixed,out,len);}
void t_fixed_read_drop(struct t_fixed_read_result *r){t_ring_fixed_drop(r->fixed);free(r->allocation);}
void t_fixed_request_drop(struct t_fixed_request *r){
 struct t_fixed_driver *d=r->driver;assert(d);
 if(!r->published){if(t_request_cancel_unpublished(d->book,r->cookie))abort();}
 else{struct t_owned_cqe ignored;int rc=t_request_take_terminal(d->book,r->cookie,&ignored);if(rc==T_REQ_STATE){struct t_loom_service_ref scope;if(t_request_abandon(d->book,r->cookie,&scope)<0)abort();}else if(rc)fail(d);}
 /* Fixed drop dispatches the single scope abort; book abandonment does not. */
 t_ring_fixed_drop(r->fixed);free(r);unref(d);
}
void t_fixed_begin_retire(struct t_fixed_driver *d){assert(d->alive);retire(d);}
int t_fixed_progress(struct t_fixed_driver *d){assert(d->alive);if(t_ring_progress(d->owner)){t_request_clear_retired(d->book);return 1;}return 0;}
size_t t_fixed_occupied(const struct t_fixed_driver *d){assert(d->alive);return t_request_occupied(d->book);}
void t_fixed_driver_drop(struct t_fixed_driver *d){assert(d->alive);d->alive=0;retire(d);unref(d);}
