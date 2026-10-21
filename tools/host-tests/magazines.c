// Host model of the per-CPU magazines under ThreadSanitizer. The bodies and
// structs are mm/magazines.{c,h}'s own, extracted by tools/test-magazines.py.
// Shims supply what the kernel would: a page, a mutex-backed zone (the real
// buddy takes zone->lock), a thread-local CPU id (each thread IS a CPU) and
// spinlocks. spin_lock_irqsave(NULL) is an IRQ mask, which excludes nothing
// running on a peer CPU, so its model is no exclusion; only a real lock
// argument excludes. Not a model of ARM weak memory or of guest timing.
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NCPUS 4
#define PG_KERNEL 0x4u
/* MAGAZINE_DEFINES */
typedef int irq_state_t;
typedef struct { pthread_mutex_t m; } spin_lock_t;
// Inline so a body that no longer takes one of them (a mutant, an older
// magazines.c) still builds under -Werror.
static inline void spin_lock_init(spin_lock_t *l) {pthread_mutex_init(&l->m,NULL);}
static inline void spin_lock(spin_lock_t *l) {pthread_mutex_lock(&l->m);}
static inline void spin_unlock(spin_lock_t *l) {pthread_mutex_unlock(&l->m);}
static inline irq_state_t spin_lock_irqsave(spin_lock_t *l) {if(l)spin_lock(l);return 0;}
static inline void spin_unlock_irqrestore(spin_lock_t *l,irq_state_t s) {(void)s;if(l)spin_unlock(l);}
struct page { struct page *next,*prev; unsigned order,flags,refcount; int id; };
/* MAGAZINE_STRUCTS */
struct percpu_data g_percpu[NCPUS];
// The #807 count guard extincts in the kernel; here it counts.
static int g_guard;
#define ASSERT_OR_DIE(c,m) do { if(!(c)) __atomic_fetch_add(&g_guard,1,__ATOMIC_RELAXED); } while (0)
#define POOL 512
struct zone { pthread_mutex_t lock; struct page *free[POOL]; int nfree; } g_zone0;
static struct page g_pages[POOL];
static int g_alias,g_double_free;
static bool zone_holds(struct page *p) {
    for(int i=0;i<g_zone0.nfree;i++) if(g_zone0.free[i]==p) return true;
    return false;
}
static void buddy_free(struct zone *z,struct page *p,unsigned order) {
    (void)order;pthread_mutex_lock(&z->lock);
    if(zone_holds(p))__atomic_fetch_add(&g_double_free,1,__ATOMIC_RELAXED);
    else if(z->nfree<POOL)z->free[z->nfree++]=p;
    pthread_mutex_unlock(&z->lock);
}
static struct page *buddy_alloc(struct zone *z,unsigned order) {
    (void)order;struct page *p=NULL;pthread_mutex_lock(&z->lock);
    if(z->nfree>0)p=z->free[--z->nfree];
    pthread_mutex_unlock(&z->lock);return p;
}
static _Thread_local int t_cpu;
static unsigned smp_cpu_idx_self(void) {return (unsigned)t_cpu;}
/* MAGAZINE_BODIES */
static int g_iters=4000;
// An owner works its own sets through the real fast path, holding up to 24
// pages at once so refill (count 0) and drain-on-full (count 16) both run.
// A page just handed out must not also be in the zone: that is two owners.
static void *owner(void *arg) {
    t_cpu=(int)(long)arg;struct page *held[24];
    for(int i=0;i<g_iters;i++) {
        int n=1+(i*7+t_cpu)%24,k=0;
        while(k<n) {
            struct page *p=mag_alloc(0);if(!p)break;
            pthread_mutex_lock(&g_zone0.lock);
            if(zone_holds(p))__atomic_fetch_add(&g_alias,1,__ATOMIC_RELAXED);
            pthread_mutex_unlock(&g_zone0.lock);
            held[k++]=p;
        }
        while(k)mag_free(held[--k],0);
    }
    return NULL;
}
static void *drainer(void *arg) {
    t_cpu=(int)(long)arg;
    for(int i=0;i<g_iters;i++)magazines_drain_all();
    return NULL;
}
int main(int argc,char **argv) {
    if(argc>1)g_iters=atoi(argv[1]);
    memset(g_percpu,0,sizeof g_percpu);magazines_init();
    pthread_mutex_init(&g_zone0.lock,NULL);
    for(int i=0;i<POOL;i++){g_pages[i].id=i;g_zone0.free[g_zone0.nfree++]=&g_pages[i];}
    pthread_t t[NCPUS];
    for(long c=0;c<NCPUS-1;c++)pthread_create(&t[c],NULL,owner,(void *)c);
    pthread_create(&t[NCPUS-1],NULL,drainer,(void *)(long)(NCPUS-1));
    for(int c=0;c<NCPUS;c++)pthread_join(t[c],NULL);
    // Conservation: after a final drain every page is in the zone exactly once.
    magazines_drain_all();
    int seen[POOL]={0},dup=0;
    for(int i=0;i<g_zone0.nfree;i++)if(seen[g_zone0.free[i]->id]++)dup++;
    printf("summary iters=%d alias=%d double_free=%d guard=%d zone=%d/%d dup=%d\n",
           g_iters,g_alias,g_double_free,g_guard,g_zone0.nfree,POOL,dup);
    return g_alias||g_double_free||g_guard||dup||g_zone0.nfree!=POOL ? 3 : 0;
}
