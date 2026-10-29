// A host double for the magazine shared-set race, run under ThreadSanitizer.
//
// WHAT QUESTION THIS ANSWERS. The owning dossier already states the mechanism:
// "magazines_drain_all is quiescent-only -- it walks peer CPUs' sets with no
// coordination" (sub-kernel-mm-phys.md:113), and its Prosecution section
// requires any peer-CPU toucher of g_percpu to prove quiescence (:336). So
// demonstrating "it races" would demonstrate scripture. The OPEN question is
// SEVERITY: mm/magazines.c carries an explicit "#807 regression guard" --
// ASSERT_OR_DIE(count <= MAGAZINE_SIZE) in mag_alloc, whose comment says it
// trips "LOUDLY instead of silently double-allocating". Does it? In the
// interleaving at issue the count stays IN RANGE, so the prediction under test
// is that the guard does NOT fire and the failure is a SILENT double
// allocation. Whoever relies on that guard as the backstop needs to know.
//
// THE REAL BODIES RUN HERE. mag_free, mag_alloc, mag_drain, mag_refill,
// magazines_drain_all and the two index helpers are EXTRACTED from
// mm/magazines.c into extracted.inc by the runner, never retyped, and the
// runner asserts each extracted body is a verbatim substring of the source. The
// shims below supply only what the kernel would: a page, a zone, a CPU id, and
// the two lock primitives.
//
// THE ONE MODELLING DECISION THAT MATTERS, stated because the result rests on
// it: the real fast path's exclusion is spin_lock_irqsave(NULL), a BARE IRQ
// MASK (spinlock.h; the dossier calls it "the MASK, not a lock"). An IRQ mask
// pins the local CPU against its own interrupts and excludes NOTHING running on
// a peer CPU. A pthread has no IRQ analogue, so the faithful model of that
// primitive is: no cross-thread exclusion. This does not WEAKEN the real
// locking -- it models exactly what the real primitive does and does not do.
// Leg 3 exists to prove that claim rather than assert it.
//
// WHAT IT DOES NOT ESTABLISH: nothing about ARM weak memory (TSan models the C11
// memory model on the host), nothing about real timing or probability, nothing
// about whether the kernel suite is actually quiescent at its 24 call sites
// (that needs the guest), and nothing about production reachability -- today
// every caller is a test.
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- the shims: what the kernel supplies to these bodies ----
#define MAGAZINE_SIZE       16
#define NUM_MAG_ORDERS      2
#define MAG_IDX_ORDER_4K    0
#define MAG_IDX_ORDER_2M    1
#define NCPUS               4          // the suite's default -smp 4
#define PG_KERNEL           0x4u

typedef int irq_state_t;

struct page { struct page *next, *prev; unsigned order, flags, refcount; int id; };
struct magazine { int count; struct page *entries[MAGAZINE_SIZE]; };
struct percpu_data { struct magazine mags[NUM_MAG_ORDERS]; };
struct percpu_data g_percpu[NCPUS];

// The #807 guard. The real one extincts; here it COUNTS, because "would it have
// fired" is the measurement. Reported per leg.
static int g_assert_fires;
#define ASSERT_OR_DIE(c, m) do { if (!(c)) { __atomic_fetch_add(&g_assert_fires, 1, __ATOMIC_RELAXED); } } while (0)

// The zone double. The REAL buddy_free/buddy_alloc take zone->lock with IRQs
// off (mm/buddy.c), so a real mutex is the faithful model -- the zone side is
// NOT the unguarded part and must not be modelled as if it were.
#define POOL 512
struct zone { pthread_mutex_t lock; struct page *free[POOL]; int nfree; } g_zone0;
static int g_double_alloc;     // a page handed out while ALSO in the buddy
static int g_double_free;      // a page freed to the buddy while already there

static bool zone_holds(struct page *p) {               // caller holds the lock
    for (int i = 0; i < g_zone0.nfree; i++) if (g_zone0.free[i] == p) return true;
    return false;
}
static void buddy_free(struct zone *z, struct page *p, unsigned order) {
    (void)order;
    pthread_mutex_lock(&z->lock);
    if (zone_holds(p)) __atomic_fetch_add(&g_double_free, 1, __ATOMIC_RELAXED);
    else if (z->nfree < POOL) z->free[z->nfree++] = p;
    pthread_mutex_unlock(&z->lock);
}
static struct page *buddy_alloc(struct zone *z, unsigned order) {
    (void)order;
    struct page *p = NULL;
    pthread_mutex_lock(&z->lock);
    if (z->nfree > 0) p = z->free[--z->nfree];
    pthread_mutex_unlock(&z->lock);
    return p;
}

// Current CPU: thread-local, so each worker IS a CPU.
static _Thread_local int t_cpu;
static unsigned smp_cpu_idx_self(void) { return (unsigned)t_cpu; }

// The fast path's primitive. LEG_GLOBAL_LOCK is the ATTRIBUTION CONTROL: it
// pretends the mask excluded peer CPUs, which it does not.
static pthread_mutex_t g_pretend_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_global_lock_mode;
static irq_state_t spin_lock_irqsave(void *l) {
    (void)l; if (g_global_lock_mode) pthread_mutex_lock(&g_pretend_lock); return 0;
}
static void spin_unlock_irqrestore(void *l, irq_state_t s) {
    (void)l; (void)s; if (g_global_lock_mode) pthread_mutex_unlock(&g_pretend_lock);
}

#include "extracted.inc"            // the REAL bodies, extracted by the runner

// ---- the experiment ----
// The owner thread works ITS OWN magazine through the real fast path, which is
// the sanctioned, documented-safe use. The drainer is the only rule-breaker.
static int g_stop;
static int g_iters = 20000;

static void *owner_thread(void *arg) {
    t_cpu = (int)(long)arg;
    for (int i = 0; i < g_iters && !__atomic_load_n(&g_stop, __ATOMIC_RELAXED); i++) {
        struct page *p = mag_alloc(0);
        if (!p) continue;
        // THE SILENT-CORRUPTION TEST: a page just handed to this caller must not
        // also sit in the buddy's free list. If it does, two owners hold one
        // page -- and the #807 guard's count stayed in range throughout.
        pthread_mutex_lock(&g_zone0.lock);
        if (zone_holds(p)) __atomic_fetch_add(&g_double_alloc, 1, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&g_zone0.lock);
        mag_free(p, 0);
    }
    return NULL;
}

static int g_lock_drainer;   // the ATTRIBUTION CONTROL excludes the drainer too
static void *drainer_cross(void *arg) {
    t_cpu = (int)(long)arg;
    for (int i = 0; i < g_iters && !__atomic_load_n(&g_stop, __ATOMIC_RELAXED); i++) {
        if (g_lock_drainer) pthread_mutex_lock(&g_pretend_lock);
        magazines_drain_all();
        if (g_lock_drainer) pthread_mutex_unlock(&g_pretend_lock);
    }
    return NULL;
}

// LEG 2, the FIX CANDIDATE: each CPU drains its OWN set, which is what an
// IPI-per-CPU drain means -- the same work, under the discipline the rest of
// the file already relies on.
static void drain_local_only(void) {
    irq_state_t s = spin_lock_irqsave(NULL);
    for (int idx = 0; idx < NUM_MAG_ORDERS; idx++) {
        struct magazine *m = &g_percpu[smp_cpu_idx_self()].mags[idx];
        unsigned order = mag_idx_to_order(idx);
        while (m->count > 0) buddy_free(&g_zone0, m->entries[--m->count], order);
    }
    spin_unlock_irqrestore(NULL, s);
}
static void *owner_thread_selfdrain(void *arg) {
    t_cpu = (int)(long)arg;
    for (int i = 0; i < g_iters && !__atomic_load_n(&g_stop, __ATOMIC_RELAXED); i++) {
        struct page *p = mag_alloc(0);
        if (p) {
            pthread_mutex_lock(&g_zone0.lock);
            if (zone_holds(p)) __atomic_fetch_add(&g_double_alloc, 1, __ATOMIC_RELAXED);
            pthread_mutex_unlock(&g_zone0.lock);
            mag_free(p, 0);
        }
        if ((i & 0x3f) == 0) drain_local_only();
    }
    return NULL;
}

static struct page g_pages[POOL];

static void reset(void) {
    memset(g_percpu, 0, sizeof g_percpu);
    pthread_mutex_init(&g_zone0.lock, NULL);
    g_zone0.nfree = 0;
    for (int i = 0; i < POOL; i++) { g_pages[i].id = i; g_zone0.free[g_zone0.nfree++] = &g_pages[i]; }
    g_assert_fires = g_double_alloc = g_double_free = g_stop = 0;
}

int main(int argc, char **argv) {
    const char *leg = argc > 1 ? argv[1] : "cross";
    if (argc > 2) g_iters = atoi(argv[2]);
    reset();
    pthread_t a, b;
    if (!strcmp(leg, "cross")) {              // faithful: no exclusion anywhere
        g_global_lock_mode = 0; g_lock_drainer = 0;
        pthread_create(&a, NULL, owner_thread, (void *)0L);
        pthread_create(&b, NULL, drainer_cross, (void *)1L);
    } else if (!strcmp(leg, "owner-lock")) {  // faithful too: the owner's mask is
        // modelled as a lock the DRAINER does not take, which is exactly what an
        // IRQ mask is against a peer CPU. Kept as its own leg because its timing
        // surfaces the corruption readily where the tight loop does not.
        g_global_lock_mode = 1; g_lock_drainer = 0;
        pthread_create(&a, NULL, owner_thread, (void *)0L);
        pthread_create(&b, NULL, drainer_cross, (void *)1L);
    } else if (!strcmp(leg, "both-locked")) { // THE ATTRIBUTION CONTROL
        g_global_lock_mode = 1; g_lock_drainer = 1;
        pthread_create(&a, NULL, owner_thread, (void *)0L);
        pthread_create(&b, NULL, drainer_cross, (void *)1L);
    } else {                                   // the fix candidate
        g_global_lock_mode = 0;
        pthread_create(&a, NULL, owner_thread_selfdrain, (void *)0L);
        pthread_create(&b, NULL, owner_thread_selfdrain, (void *)1L);
    }
    pthread_join(a, NULL); pthread_join(b, NULL);
    printf("leg=%-13s iters=%d  #807-guard-fires=%d  silent-double-alloc=%d  double-free=%d\n",
           leg, g_iters, g_assert_fires, g_double_alloc, g_double_free);
    return 0;
}
