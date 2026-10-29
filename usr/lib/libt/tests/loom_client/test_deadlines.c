#include "test_fixed_fixture.c"
static size_t timer_count(void *b){(void)b;return 64;}
static size_t timer_length(void *b,size_t i){(void)b;(void)i;return 16;}
static const unsigned char *timer_buffer(void *b,size_t i){return ((struct backing *)b)->bytes+16*i;}
static struct t_fixed_driver *timer_driver(struct probe *p){
 struct backing *b=calloc(1,sizeof(*b));assert(b);b->p=p;fixed_storage *s=new_storage(b);
 struct t_ring_backend backend={b,timer_count,timer_length,timer_buffer,ret,pool_retire,pool_destroy,retire,retired,close_owner,destroy,register_pool,abort_scope};struct t_fixed_driver *d=NULL;assert(!T_FIXED_CREATE(backend,s,&d));return d;
}
static void deadline_cases(void){
 struct probe p={0};struct t_fixed_driver *d=driver(&p,1);struct t_fixed_request *r,*other;uint64_t c,when;struct t_fixed_read_result *done;struct t_paired_record cq;
 /* Prepared timers are inactive; zero is a real absolute deadline. */
 assert(!t_fixed_read(d,id(3,7),range(0),&r));assert(t_fixed_cancel(r)<0&&!p.aborts);
 assert(!t_fixed_set_deadline(r,1,12));assert(!t_fixed_next_deadline(d,&when));assert(!t_fixed_expire(d,UINT64_MAX,64));
 assert(!t_fixed_set_deadline(r,0,0));assert(!t_fixed_set_deadline(r,1,0));assert(!t_fixed_publish(r,&c));
 assert(t_fixed_set_deadline(r,1,99)<0);assert(t_fixed_next_deadline(d,&when)&&when==0);
 assert(!t_fixed_expire(d,0,0)&&!p.aborts);assert(t_fixed_expire(d,0,1)==1&&p.aborts==1);
 assert(!t_fixed_next_deadline(d,&when)&&!t_fixed_cancel(r));assert(!t_fixed_read_complete(r,&done));
 assert(t_fixed_read(d,id(3,7),range(8),&other)<0&&t_fixed_occupied(d)==1);
 t_fixed_request_drop(r);assert(p.aborts==1&&t_fixed_occupied(d)==1);cq=record(c,-2,0);assert(!t_fixed_receive(d,&cq));
 /* Reuse clears both cancellation and timer state. */
 assert(!t_fixed_read(d,id(3,8),range(0),&r));assert(!t_fixed_publish(r,&c));assert(!t_fixed_next_deadline(d,&when));assert(t_fixed_cancel(r)==1&&p.aborts==2&&p.last_abort.incarnation==8);t_fixed_request_drop(r);assert(p.aborts==2);t_fixed_driver_drop(d);
 /* Cancellation preserves a later successful WRITE. */
 p=(struct probe){0};d=driver(&p,1);unsigned char input[8]={1};assert(!t_fixed_write(d,id(4,9),range(0),input,8,&r));assert(!t_fixed_set_deadline(r,1,10));assert(!t_fixed_publish(r,&c));
 assert(t_fixed_cancel(r)==1&&!t_fixed_cancel(r));assert(!t_fixed_expire(d,UINT64_MAX,SIZE_MAX));cq=record(c,8,0);assert(!t_fixed_receive(d,&cq));assert(!t_fixed_cancel(r));struct t_owned_cqe result;assert(t_fixed_write_complete(r,&result)==1&&result.result==8);assert(p.aborts==1);t_fixed_driver_drop(d);
 /* Terminal READ is no longer timed; its view still retains storage. */
 p=(struct probe){0};d=driver(&p,1);assert(!t_fixed_read(d,id(0,1),range(8),&r));assert(!t_fixed_set_deadline(r,1,5));assert(!t_fixed_publish(r,&c));cq=record(c,3,0);assert(!t_fixed_receive(d,&cq));assert(!t_fixed_expire(d,5,64)&&!t_fixed_next_deadline(d,&when)&&!p.aborts);assert(t_fixed_read_complete(r,&done)==1);t_fixed_driver_drop(d);const unsigned char *bytes;size_t n;assert(!t_fixed_read_bytes(done,&bytes,&n)&&n==3&&bytes[0]==1&&!p.freed);t_fixed_read_drop(done);assert(p.freed==1);
 /* Closing suppresses abort; retiring retains ownership until local proof. */
 p=(struct probe){0};d=driver(&p,1);assert(!t_fixed_read(d,id(0,1),range(0),&r));assert(!t_fixed_set_deadline(r,1,1));assert(!t_fixed_publish(r,&c));t_fixed_close(d);assert(!t_fixed_cancel(r)&&!t_fixed_expire(d,1,64)&&!t_fixed_next_deadline(d,&when));t_fixed_request_drop(r);assert(!p.aborts&&!t_fixed_progress(d));p.retired=1;assert(t_fixed_progress(d));t_fixed_driver_drop(d);
 /* Deadline exact boundary, rotating fairness, bounded visits and max integer. */
 p=(struct probe){0};d=timer_driver(&p);struct t_fixed_request *rs[64];uint64_t cookies[64];
 for(unsigned i=0;i<64;++i){struct t_loom_service_pool_member m={i,0,0,8};assert(!t_fixed_read(d,id(i,1),m,&rs[i])&&used_requests(d)==i+1);assert(!t_fixed_set_deadline(rs[i],1,i==63?UINT64_MAX:100));assert(!t_fixed_publish(rs[i],&cookies[i]));}
 assert(t_fixed_next_deadline(d,&when)&&when==100);assert(!t_fixed_expire(d,99,64));
 for(unsigned i=0;i<63;++i){assert(t_fixed_expire(d,100,1)==1&&p.aborts==i+1&&p.last_abort.slot==i);}
 assert(!t_fixed_expire(d,100,1)&&p.aborts==63);assert(t_fixed_next_deadline(d,&when)&&when==UINT64_MAX);assert(t_fixed_expire(d,UINT64_MAX,SIZE_MAX)==1&&p.aborts==64);assert(!t_fixed_next_deadline(d,&when));
 for(unsigned i=0;i<64;++i){t_fixed_request_drop(rs[i]);cq=record(cookies[i],-2,0);assert(!t_fixed_receive(d,&cq));}assert(p.aborts==64&&!t_fixed_occupied(d));t_fixed_driver_drop(d);
 /* Clearing a prepared deadline remains clear after publication. */
 p=(struct probe){0};d=driver(&p,1);assert(!t_fixed_read(d,id(0,1),range(0),&r));assert(!t_fixed_set_deadline(r,1,3));assert(!t_fixed_set_deadline(r,0,0));assert(!t_fixed_publish(r,&c));assert(!t_fixed_next_deadline(d,&when)&&!t_fixed_expire(d,UINT64_MAX,64));t_fixed_request_drop(r);assert(p.aborts==1);t_fixed_driver_drop(d);
 /* Normal retirement does not suppress deadline work or refund early. */
 p=(struct probe){0};d=driver(&p,1);assert(!t_fixed_read(d,id(0,1),range(0),&r));assert(!t_fixed_set_deadline(r,1,3));assert(!t_fixed_publish(r,&c));t_fixed_begin_retire(d);assert(t_fixed_next_deadline(d,&when)&&when==3);assert(t_fixed_expire(d,3,64)==1);assert(!t_fixed_progress(d));t_fixed_request_drop(r);assert(p.aborts==1&&!t_fixed_progress(d));p.retired=1;assert(t_fixed_progress(d));t_fixed_driver_drop(d);
}
int main(void){deadline_cases();fixeds_idle();puts("C eight deadline cancellation scenarios PASS");return 0;}
