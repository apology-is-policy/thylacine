/* Fixed-request fixture shared by the controller, deadline and executor tests:
 * the ring fixture's backend over one storage object per driver. */
#define main historical_ring_main
#include "test_ring_payload.c"
#undef main
#include <thyla/loom_client/fixed_requests.h>
typedef T_FIXED_STORAGE(64) fixed_storage;
static fixed_storage *fixeds[256];static size_t nfixeds;
static fixed_storage *new_storage(struct backing *b){fixed_storage *s=calloc(1,sizeof(*s));assert(s&&nfixeds<256);fixeds[nfixeds++]=s;b->core=&s->base.ring;return s;}
__attribute__((unused)) static void fixeds_idle(void){for(size_t i=0;i<nfixeds;++i){assert(t_fixed_storage_idle(&fixeds[i]->base));free(fixeds[i]);}nfixeds=0;}
__attribute__((unused)) static size_t used_requests(const struct t_fixed_driver *d){size_t n=0;for(size_t i=0;i<64;++i)n+=d->storage->requests[i].used;return n;}
static struct t_fixed_driver *driver(struct probe *p,size_t cap){
 struct backing *b=calloc(1,sizeof(*b));assert(b&&cap<=64);b->p=p;for(unsigned i=0;i<128;++i)memset(b->bytes+8*i,i,8);fixed_storage *s=new_storage(b);
 struct t_ring_backend backend={b,count,length,buffer,ret,pool_retire,pool_destroy,retire,retired,close_owner,destroy,register_pool,abort_scope};struct t_fixed_driver *d=NULL;int rc=t_fixed_create(backend,&s->base,s->book,cap,&d);if(!cap){assert(rc<0&&!d);return NULL;}assert(!rc&&d);return d;
}
static struct t_paired_record record(uint64_t cookie,int32_t n,uint32_t flags){return (struct t_paired_record){.cqe={cookie,n,flags}};}
