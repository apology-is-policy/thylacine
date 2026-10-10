#define main historical_ring_main
#include "test_ring_payload.c"
#undef main
#include "fixed_requests.h"
static struct t_fixed_driver *driver(struct probe *p,size_t cap){
 struct backing *b=calloc(1,sizeof(*b));assert(b);b->p=p;for(unsigned i=0;i<128;++i)memset(b->bytes+8*i,i,8);
 struct t_ring_backend backend={b,count,length,buffer,ret,pool_retire,pool_destroy,retire,retired,close_owner,destroy,register_pool,abort_scope};struct t_fixed_driver *d=NULL;int rc=t_fixed_create(backend,cap,&d);if(!cap){assert(rc<0&&!d);return NULL;}assert(!rc&&d);return d;
}
static struct t_paired_record record(uint64_t cookie,int32_t n,uint32_t flags){return (struct t_paired_record){.cqe={cookie,n,flags}};}

#include "retirement_executor.h"
struct factory_context{struct probe *p;size_t cap,calls;};
static int make_driver(void *arg,struct t_fixed_driver **out){struct factory_context *f=arg;++f->calls;*out=driver(f->p,f->cap);return *out?0:-22;}
static struct t_managed_client *managed(struct t_retirement_executor *e,struct factory_context *f){struct t_managed_client *c=NULL;assert(!t_retirement_admit(e,make_driver,f,&c)&&c);return c;}
int main(void){
 struct t_retirement_executor *e;struct t_managed_client *c,*extra;struct probe p={0};struct factory_context f={&p,1,0};
 assert(t_retirement_create(0,&e)==T_RETIREMENT_CAPACITY&&!e);assert(t_retirement_create(65,&e)==T_RETIREMENT_CAPACITY&&!e);assert(!t_retirement_create(1,&e));c=managed(e,&f);assert(t_retirement_admit(e,make_driver,&f,&extra)==T_RETIREMENT_FULL&&!extra&&f.calls==1);t_managed_drop(c);assert(p.retires==1);for(unsigned i=0;i<3;++i)assert(!t_retirement_progress(e,64).reclaimed);assert(t_retirement_occupied(e)==1&&!p.freed);p.retired=1;assert(t_retirement_progress(e,1).reclaimed==1&&p.closes==1&&p.freed==1);p=(struct probe){0};c=managed(e,&f);t_managed_drop(c);p.retired=1;assert(t_retirement_progress(e,1).reclaimed==1);t_retirement_drop(e);
 p=(struct probe){0};assert(!t_retirement_create(1,&e));c=managed(e,&f);struct t_fixed_request *r;struct t_fixed_read_result *done;uint64_t cookie;assert(!t_managed_read(c,id(2,3),range(0),&r));assert(!t_fixed_publish(r,&cookie));t_managed_drop(c);p.retired=1;assert(!t_retirement_progress(e,1).reclaimed);assert(t_fixed_read_complete(r,&done)==T_FIXED_CLOSING);t_fixed_request_drop(r);assert(p.aborts==1);assert(t_retirement_progress(e,1).reclaimed==1&&p.freed==1);t_retirement_drop(e);
 p=(struct probe){0};assert(!t_retirement_create(1,&e));c=managed(e,&f);assert(!t_managed_read(c,id(0,1),range(8),&r));assert(!t_fixed_publish(r,&cookie));struct t_paired_record cq=record(cookie,4,0);assert(!t_managed_receive(c,&cq));assert(t_fixed_read_complete(r,&done)==1);t_managed_drop(c);p.retired=1;assert(!t_retirement_progress(e,1).reclaimed);const unsigned char *bytes;size_t n;assert(!t_fixed_read_bytes(done,&bytes,&n)&&n==4&&bytes[0]==1);t_fixed_read_drop(done);assert(t_retirement_progress(e,1).reclaimed==1&&p.freed==1);t_retirement_drop(e);
 struct probe q={0},z={0};struct factory_context g={&q,1,0},h={&z,1,0};p=(struct probe){0};assert(!t_retirement_create(3,&e));c=managed(e,&f);struct t_managed_client *b=managed(e,&g),*a=managed(e,&h);t_managed_drop(c);t_managed_drop(b);t_managed_drop(a);q.retired=z.retired=1;assert(!t_retirement_progress(e,0).visited);assert(!t_retirement_progress(e,1).reclaimed);assert(t_retirement_progress(e,1).reclaimed==1&&q.freed==1);assert(t_retirement_progress(e,1).reclaimed==1&&z.freed==1&&!p.freed);p.retired=1;struct t_retirement_progress progress=t_retirement_progress(e,SIZE_MAX);assert(progress.visited==3&&progress.reclaimed==1&&!progress.occupied);t_retirement_drop(e);
 p=(struct probe){0};assert(!t_retirement_create(1,&e));c=managed(e,&f);p.retired=1;assert(!t_retirement_progress(e,1).reclaimed&&t_retirement_occupied(e)==1);t_managed_drop(c);assert(t_retirement_progress(e,1).reclaimed==1);t_retirement_drop(e);
 p=(struct probe){0};assert(!t_retirement_create(1,&e));c=managed(e,&f);assert(!t_managed_read(c,id(0,1),range(8),&r));assert(!t_fixed_publish(r,&cookie));cq=record(cookie,2,0);assert(!t_managed_receive(c,&cq));assert(t_fixed_read_complete(r,&done)==1);t_retirement_drop(e);assert(p.closes==1&&t_managed_read(c,id(0,1),range(0),&r)==T_FIXED_CLOSING&&!r);t_managed_drop(c);assert(!p.freed&&!t_fixed_read_bytes(done,&bytes,&n)&&n==2&&bytes[0]==1);t_fixed_read_drop(done);assert(p.freed==1&&p.closes==1);
 p=(struct probe){0};f.cap=0;assert(!t_retirement_create(1,&e));assert(t_retirement_admit(e,make_driver,&f,&c)==-22&&!c&&!t_retirement_occupied(e)&&p.closes==1&&p.freed==1);f.cap=1;p=(struct probe){0};size_t calls=f.calls;fail_alloc=1;assert(t_retirement_admit(e,make_driver,&f,&c)==-12&&!c&&f.calls==calls);c=managed(e,&f);t_managed_drop(c);p.retired=1;assert(t_retirement_progress(e,1).reclaimed==1);t_retirement_drop(e);
 p=(struct probe){0};assert(!t_retirement_create(1,&e));c=managed(e,&f);fail_alloc=1;t_managed_drop(c);assert(!t_retirement_progress(e,1).reclaimed);p.retired=1;assert(t_retirement_progress(e,1).reclaimed==1&&fail_alloc==1);fail_alloc=0;t_retirement_drop(e);
 puts("C eight retirement owner scenarios PASS");return 0;
}
