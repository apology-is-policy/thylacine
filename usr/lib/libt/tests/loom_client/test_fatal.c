/* A broken invariant ends the process at the check that names it: each misuse
 * below runs in a child, which must reach t_loom_client_fatal in the expected
 * function. Every other test is the positive control: there the same calls,
 * without the misuse, return. */
#include "test_fixed_fixture.c"
#include <thyla/loom_client/owned_mapping.h>
#include <thyla/loom_client/retirement_executor.h>
#include <sys/wait.h>
#include <unistd.h>
extern const char *t_test_expected_fatal;
static void expect_fatal(const char *func,void (*misuse)(void)){
 fflush(NULL);pid_t pid=fork();assert(pid>=0);
 if(!pid){t_test_expected_fatal=func;misuse();_exit(0);}
 int status=0;assert(waitpid(pid,&status,0)==pid);if(!WIFEXITED(status)||WEXITSTATUS(status)!=77){fprintf(stderr,"%s: no fatal there (status %d)\n",func,status);assert(0);}
}
static struct t_ring_backend backend_of(struct probe *p){struct backing *b=calloc(1,sizeof(*b));assert(b);b->p=p;return (struct t_ring_backend){b,count,length,buffer,ret,pool_retire,pool_destroy,retire,retired,close_owner,destroy,register_pool,abort_scope};}
static void ring_storage_reused(void){struct probe p={0};struct t_ring_owner *r;struct t_ring_client *c;fixture(&p,&r,&c);(void)t_ring_create(backend_of(&p),r->core,&r,&c);}
static void fixed_storage_reused(void){struct probe p={0};struct t_fixed_driver *d=driver(&p,1);struct t_fixed_request *r;assert(!t_fixed_read(d,id(0,1),range(0),&r));uint64_t cookie;assert(!t_fixed_publish(r,&cookie));struct t_paired_record cq=record(cookie,3,0);assert(!t_fixed_receive(d,&cq));struct t_fixed_read_result *done;assert(t_fixed_read_complete(r,&done)==1);struct t_fixed_storage *s=d->storage;t_fixed_driver_drop(d);assert(!t_fixed_storage_idle(s));(void)t_fixed_create(backend_of(&p),s,fixeds[nfixeds-1]->book,1,&d);}
static void *map_base(void *v){return v;}static size_t map_length(void *v){(void)v;return 4096;}static void map_close(void *v){(void)v;}static void map_destroy(void *v){(void)v;}
static void mapping_storage_reused(void){static _Alignas(4096) unsigned char ring[4096];static struct t_owned_mapping m;m.live=1;struct t_owned_setup p={0};(void)t_mapping_create((struct t_mapping_backend){ring,map_base,map_length,map_close,map_destroy},p,&m);}
static void executor_storage_reused(void){static struct t_retirement_executor e;struct t_retirement_executor *h;assert(!t_retirement_create(&e,1,&h));(void)t_retirement_create(&e,1,&h);}
static void owner_dropped_twice(void){struct probe p={0};struct t_ring_owner *r;struct t_ring_client *c;fixture(&p,&r,&c);t_ring_owner_drop(r);t_ring_owner_drop(r);}
static void consumed_request_dropped(void){struct probe p={0};struct t_fixed_driver *d=driver(&p,1);struct t_fixed_request *r;assert(!t_fixed_read(d,id(0,1),range(0),&r));uint64_t cookie;assert(!t_fixed_publish(r,&cookie));struct t_paired_record cq=record(cookie,3,0);assert(!t_fixed_receive(d,&cq));struct t_fixed_read_result *done;assert(t_fixed_read_complete(r,&done)==1);t_fixed_request_drop(r);}
int main(void){
 expect_fatal("t_ring_create",ring_storage_reused);
 expect_fatal("t_fixed_create",fixed_storage_reused);
 expect_fatal("t_mapping_create",mapping_storage_reused);
 expect_fatal("t_retirement_create",executor_storage_reused);
 expect_fatal("t_ring_owner_drop",owner_dropped_twice);
 expect_fatal("t_fixed_request_drop",consumed_request_dropped);
 puts("C six fatal invariant scenarios PASS");return 0;
}
