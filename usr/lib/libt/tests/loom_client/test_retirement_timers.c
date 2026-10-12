#include "test_fixed_fixture.c"

#include <thyla/loom_client/retirement_executor.h>
static struct t_retirement_executor es;
struct factory_context{struct probe *p;size_t cap,calls;};
static int make_driver(void *arg,struct t_fixed_driver **out){struct factory_context *f=arg;++f->calls;*out=driver(f->p,f->cap);return *out?0:-22;}
static struct t_managed_client *managed(struct t_retirement_executor *e,struct factory_context *f){struct t_managed_client *c=NULL;assert(!t_retirement_admit(e,make_driver,f,&c)&&c);return c;}
static struct t_fixed_request *timed(struct t_managed_client *c,uint64_t deadline,uint64_t *cookie){
 struct t_fixed_request *r;assert(!t_managed_read(c,id(1,2),range(8),&r));assert(!t_fixed_set_deadline(r,1,deadline));assert(!t_fixed_publish(r,cookie));return r;
}
int main(void){
 struct t_retirement_executor *e;struct probe p={0},q={0},z={0};struct factory_context f={&p,1,0},g={&q,1,0},h={&z,1,0};uint64_t a,b,when;
 /* Attached and retiring owners both contribute; zero budget is not progress. */
 assert(!t_retirement_create(&es,3,&e));struct t_managed_client *c=managed(e,&f),*d=managed(e,&g),*empty=managed(e,&h);
 struct t_fixed_request *r=timed(c,0,&a),*s=timed(d,5,&b);t_managed_drop(d);
 assert(t_retirement_next_deadline(e,&when)&&when==0);assert(!t_retirement_expire(e,5,0,64)&&!p.aborts&&!q.aborts);
 assert(!t_retirement_expire(e,5,3,0)&&!p.aborts&&!q.aborts);
 /* Cleanup must not move the timer cursor. */
 assert(t_retirement_progress(e,1).visited==1);
 assert(t_retirement_expire(e,0,1,1)==1&&p.aborts==1&&!q.aborts);
 assert(t_retirement_next_deadline(e,&when)&&when==5);assert(t_retirement_expire(e,5,1,1)==1&&q.aborts==1);
 assert(!t_retirement_expire(e,5,1,64)&&!t_retirement_next_deadline(e,&when));
 assert(t_retirement_occupied(e)==3&&!p.freed&&!q.freed);t_fixed_request_drop(r);t_fixed_request_drop(s);assert(p.aborts==1&&q.aborts==1);
 t_managed_drop(c);t_managed_drop(empty);assert(!t_retirement_progress(e,SIZE_MAX).reclaimed);p.retired=q.retired=z.retired=1;
 assert(t_retirement_progress(e,SIZE_MAX).reclaimed==3);assert(!t_retirement_next_deadline(e,&when)&&!t_retirement_expire(e,UINT64_MAX,SIZE_MAX,SIZE_MAX));t_retirement_drop(e);
 /* Root shutdown with a pending timed request closes once and retains backing. */
 p=(struct probe){0};assert(!t_retirement_create(&es,1,&e));c=managed(e,&f);r=timed(c,10,&a);t_retirement_drop(e);assert(p.closes==1&&!p.freed);assert(!t_fixed_cancel(r)&&!p.aborts);t_managed_drop(c);assert(!p.freed);t_fixed_request_drop(r);assert(p.freed==1&&p.closes==1);
 /* Terminal READ removed from timer set still prevents reclaim while viewed. */
 p=(struct probe){0};assert(!t_retirement_create(&es,1,&e));c=managed(e,&f);r=timed(c,9,&a);struct t_paired_record cq=record(a,3,0);assert(!t_managed_receive(c,&cq));struct t_fixed_read_result *done;assert(t_fixed_read_complete(r,&done)==1);t_managed_drop(c);p.retired=1;
 assert(!t_retirement_next_deadline(e,&when)&&!t_retirement_expire(e,9,1,64));assert(!t_retirement_progress(e,1).reclaimed);const unsigned char *bytes;size_t n;assert(!t_fixed_read_bytes(done,&bytes,&n)&&n==3&&bytes[0]==1);t_fixed_read_drop(done);assert(t_retirement_progress(e,1).reclaimed==1);t_retirement_drop(e);
 /* Reused manager cell has a new driver's timer state; no early abort/reclaim. */
 p=(struct probe){0};assert(!t_retirement_create(&es,1,&e));c=managed(e,&f);r=timed(c,1,&a);assert(t_retirement_expire(e,1,1,64)==1);t_fixed_request_drop(r);t_managed_drop(c);p.retired=1;assert(t_retirement_progress(e,1).reclaimed==1);
 p=(struct probe){0};c=managed(e,&f);r=timed(c,UINT64_MAX,&b);assert(a!=b&&t_retirement_next_deadline(e,&when)&&when==UINT64_MAX);assert(!t_retirement_expire(e,UINT64_MAX-1,1,64));assert(t_retirement_expire(e,UINT64_MAX,1,64)==1&&!p.freed);t_fixed_request_drop(r);t_managed_drop(c);p.retired=1;assert(t_retirement_progress(e,1).reclaimed==1);t_retirement_drop(e);
 /* Scan bounds, independent cursors, fairness and minimum across all64drivers. */
 struct probe probes[64]={0};struct factory_context factories[64];struct t_managed_client *clients[64];struct t_fixed_request *requests[64];uint64_t cookies[64];assert(!t_retirement_create(&es,64,&e));
 for(unsigned i=0;i<64;++i){factories[i]=(struct factory_context){&probes[i],1,0};clients[i]=managed(e,&factories[i]);requests[i]=timed(clients[i],64-i,&cookies[i]);}
 assert(t_retirement_next_deadline(e,&when)&&when==1);
 for(unsigned i=0;i<64;++i){assert(t_retirement_progress(e,1).visited==1);assert(t_retirement_expire(e,64,1,1)==1&&probes[i].aborts==1);for(unsigned j=i+1;j<64;++j)assert(!probes[j].aborts);}
 assert(!t_retirement_expire(e,UINT64_MAX,SIZE_MAX,SIZE_MAX)&&!t_retirement_next_deadline(e,&when));
 for(unsigned i=0;i<64;++i){t_fixed_request_drop(requests[i]);t_managed_drop(clients[i]);assert(probes[i].aborts==1);probes[i].retired=1;}
 assert(t_retirement_progress(e,64).reclaimed==64);t_retirement_drop(e);
 fixeds_idle();assert(!es.refs);puts("C six retirement timer scenarios PASS");return 0;
}
