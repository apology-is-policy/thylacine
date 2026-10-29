#include "payload_owner.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct probe{int64_t rc;size_t returns,retires,closes,freed;int retired;};
struct backing{unsigned char bytes[512];struct probe *p;};
static size_t count(void *b){(void)b;return 1;}
static size_t length(void *b,size_t i){(void)b;(void)i;return 512;}
static const unsigned char *buffer(void *b,size_t i){(void)i;return ((struct backing *)b)->bytes;}
static int64_t ret(void *v,struct t_loom_service_buffer_receipt r){struct backing *b=v;++b->p->returns;int64_t rc=b->p->rc;if(!rc)memset(b->bytes+r.member*8,0xdd,8);return rc;}
static void retire(void *v){++((struct backing *)v)->p->retires;}
static int retired(void *v){return ((struct backing *)v)->p->retired;}
static void close_owner(void *v){++((struct backing *)v)->p->closes;}
static void destroy(void *v){struct backing *b=v;assert(b->p->closes==1);++b->p->freed;free(b);}
static struct t_payload_backend backend(struct probe *p){struct backing *b=calloc(1,sizeof(*b));assert(b);b->p=p;for(unsigned i=0;i<64;++i)memset(b->bytes+8*i,65+i,8);return (struct t_payload_backend){b,count,length,buffer,ret,retire,retired,close_owner,destroy};}
static struct t_loom_service_ref poolref(void){return (struct t_loom_service_ref){2,0,31};}
static struct t_loom_service_buffer_receipt receipt(unsigned i){return (struct t_loom_service_buffer_receipt){.pool={2,0,31},.member=i,.lease=i+1};}
static void ranges(struct t_loom_service_pool_member *r){for(unsigned i=0;i<64;++i)r[i]=(struct t_loom_service_pool_member){0,0,i*8,8};}
static void fixture(struct probe *p,struct t_payload_owner **r,struct t_payload_pool **c){struct t_loom_service_pool_member m[64];ranges(m);assert(!t_payload_create(backend(p),poolref(),m,64,r,c));}
static struct t_payload_lease *accept(struct t_payload_pool *c,unsigned i){struct t_payload_lease *l=NULL;assert(t_payload_accept(c,8,4,receipt(i),&l)==1&&l);return l;}
static void view(struct t_payload_lease *l,unsigned char byte){size_t n=0;const unsigned char *p=t_payload_bytes(l,&n);assert(n==8);for(size_t i=0;i<n;++i)assert(p[i]==byte);}
int main(void){
 struct probe p={0};struct t_payload_owner *r;struct t_payload_pool *c;fixture(&p,&r,&c);
 struct t_payload_lease *a=accept(c,0),*b=accept(c,1),*none=NULL;view(a,'A');view(b,'B');assert(!t_payload_return(b));view(a,'A');
 struct t_loom_service_buffer_receipt zero={0};assert(!t_payload_accept(c,0,0,zero,&none)&&!none);assert(t_payload_outstanding(r)==1);assert(!t_payload_progress(r));t_payload_pool_drop(c);p.retired=1;assert(!t_payload_progress(r));view(a,'A');t_payload_abandon(a);assert(t_payload_progress(r));t_payload_owner_drop(r);assert(p.freed==1&&p.closes==1);
 p=(struct probe){.rc=-2};fixture(&p,&r,&c);a=accept(c,0);assert(t_payload_return(a)==-2);view(a,'A');assert(t_payload_outstanding(r)==1);t_payload_abandon(a);assert(t_payload_pending(r)==1);assert(!t_payload_progress(r));assert(t_payload_pending(r)==1&&!p.freed);p.rc=1;assert(!t_payload_progress(r));assert(t_payload_pending(r)==1);p.rc=0;assert(!t_payload_progress(r));assert(!t_payload_pending(r)&&!t_payload_outstanding(r));t_payload_pool_drop(c);assert(!t_payload_progress(r));p.retired=1;assert(t_payload_progress(r));t_payload_owner_drop(r);assert(p.freed==1);
 p=(struct probe){.rc=-11};fixture(&p,&r,&c);struct t_payload_lease *all[64];for(unsigned i=0;i<64;++i)all[i]=accept(c,i);assert(t_payload_outstanding(r)==64);for(unsigned i=0;i<64;++i)t_payload_abandon(all[i]);assert(t_payload_pending(r)==64);t_payload_pool_drop(c);size_t before=p.returns;assert(!t_payload_progress(r)&&p.returns-before==64);assert(!p.freed);p.retired=1;assert(t_payload_progress(r)&&t_payload_outstanding(r)==64);t_payload_owner_drop(r);assert(p.freed==1);
 p=(struct probe){0};fixture(&p,&r,&c);a=accept(c,0);t_payload_owner_drop(r);assert(p.closes==1&&!p.freed);assert(t_payload_accept(c,8,4,receipt(1),&none)==T_LEASE_STATE&&!none);view(a,'A');t_payload_pool_drop(c);assert(!p.freed);t_payload_abandon(a);assert(p.freed==1&&p.closes==1);
 p=(struct probe){0};struct t_loom_service_pool_member m[64];ranges(m);m[0].offset=512;assert(t_payload_create(backend(&p),poolref(),m,64,&r,&c)==T_LEASE_GEOMETRY);assert(!r&&!c&&p.retires==1&&p.closes==1&&p.freed==1);
 p=(struct probe){0};fixture(&p,&r,&c);a=accept(c,0);assert(t_payload_accept(c,8,4,receipt(0),&none)<0&&!none);assert(t_payload_accept(c,9,4,receipt(1),&none)<0&&!none);assert(t_payload_outstanding(r)==1);view(a,'A');t_payload_abandon(a);t_payload_pool_drop(c);t_payload_owner_drop(r);assert(p.freed==1);
 puts("C owned payload six lifetime/retirement scenarios PASS");return 0;
}
