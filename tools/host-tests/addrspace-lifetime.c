#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#define CHECK(x, why) do { if (!(x)) { fputs(why "\n", stderr); exit(1); } } while (0)
typedef uint64_t u64, paddr_t;
typedef uint32_t u32;
typedef struct { u32 value; } spin_lock_t;
static unsigned allocations, frees, tables, destroys, drains;
static bool fail_alloc, fail_table, pause_drain, entered_drain, release_drain;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static void *kzalloc(size_t n, unsigned flags) {
    (void)flags; if (fail_alloc) return NULL;
    allocations++; return calloc(1, n);
}
static void kfree(void *p) { frees++; free(p); }
static paddr_t proc_pgtable_create(void) { if (fail_table) return 0; tables++; return 4096; }
static void proc_pgtable_destroy(paddr_t p) { CHECK(p == 4096, "page table identity"); destroys++; }
static void spin_lock_init(spin_lock_t *p) { p->value = 0; }
static _Noreturn void extinction(const char *s) { fprintf(stderr, "extinction: %s\n", s); exit(1); }

/* ACTUAL_ADDRSPACE_DECL */
static void vma_drain_in(struct AddrSpace *as) {
    drains++;
    pthread_mutex_lock(&mu);
    entered_drain = true;
    pthread_cond_broadcast(&cv);
    while (pause_drain && !release_drain) pthread_cond_wait(&cv, &mu);
    as->vmas = NULL;
    pthread_mutex_unlock(&mu);
}

/* ACTUAL_ADDRSPACE_LIFECYCLE */
// Actual AddrSpace declaration/lifecycle are inserted before this fixture.
// Allocation, page-table destruction and mapping drain are controlled doubles.
// This checks C lifetime ordering, not guest MMU/TLB or real device behavior.
static void *drop_owner(void *p) { addrspace_unref(p); return NULL; }
static void *drop_pin(void *p) { addrspace_unpin(p); return NULL; }
static void reset_fixture(void) {
    allocations = frees = tables = destroys = drains = 0;
    fail_alloc = fail_table = pause_drain = entered_drain = release_drain = false;
}
static void serial_cases(void) {
    reset_fixture();
    CHECK(addrspace_alloc(0) == NULL && allocations == 0, "zero budget refused");
    fail_alloc = true; CHECK(addrspace_alloc(1) == NULL, "allocator failure");
    fail_alloc = false; fail_table = true;
    CHECK(addrspace_alloc(1) == NULL && frees == 1, "page table failure rolls allocation back");
    reset_fixture();
    struct AddrSpace *a = addrspace_alloc(32);
    CHECK(a && a->page_budget == 32 && a->id != 0, "fresh account shape");
    CHECK(addrspace_ref_count(a) == 1 && addrspace_owner_count(a) == 1, "fresh owner counts");
    a->vmas = (void *)1;
    addrspace_pin(a);
    CHECK(addrspace_ref_count(a) == 2 && addrspace_owner_count(a) == 1, "pin is not an owner");
    addrspace_ref(a);
    CHECK(addrspace_ref_count(a) == 3 && addrspace_owner_count(a) == 2, "owner increments both counts");
    addrspace_unref(a);
    CHECK(drains == 0 && destroys == 0 && a->vmas, "nonfinal owner preserves mappings");
    addrspace_unref(a);
    CHECK(drains == 1 && destroys == 0 && frees == 0 && a->vmas == NULL, "last owner drains before last pin");
    CHECK(addrspace_ref_count(a) == 1 && addrspace_owner_count(a) == 0, "ownerless descriptor remains pinned");
    addrspace_pin(a);
    addrspace_unpin(a);
    CHECK(frees == 0 && drains == 1, "kernel pins may transfer descriptor lifetime");
    addrspace_unpin(a);
    CHECK(frees == 1 && destroys == 1 && drains == 1, "final pin frees once without another drain");

    reset_fixture(); a = addrspace_alloc(32); a->vmas = (void *)1;
    addrspace_pin(a); addrspace_unpin(a);
    CHECK(drains == 0 && frees == 0 && addrspace_owner_count(a) == 1, "early pin release preserves owner");
    addrspace_unref(a);
    CHECK(drains == 1 && frees == 1 && destroys == 1, "ordinary last owner still frees");
    addrspace_unref(NULL); addrspace_unpin(NULL);
    CHECK(addrspace_ref_count(NULL) == 0 && addrspace_owner_count(NULL) == 0, "null lifetime helpers");
}
static void during_drain(void) {
    reset_fixture(); struct AddrSpace *a = addrspace_alloc(32); a->vmas = (void *)1;
    addrspace_pin(a); pause_drain = true;
    pthread_t t;
    CHECK(pthread_create(&t, NULL, drop_owner, a) == 0, "create owner teardown");
    pthread_mutex_lock(&mu);
    while (!entered_drain) pthread_cond_wait(&cv, &mu);
    CHECK(addrspace_owner_count(a) == 0 && addrspace_ref_count(a) == 2, "draining owner retains lifetime");
    pthread_mutex_unlock(&mu);
    addrspace_unpin(a);
    CHECK(frees == 0 && destroys == 0, "unpin cannot free during mapping drain");
    pthread_mutex_lock(&mu); release_drain = true; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    CHECK(pthread_join(t, NULL) == 0, "join owner teardown");
    CHECK(drains == 1 && destroys == 1 && frees == 1, "drain completion frees exactly once");
}
static void simultaneous_drops(void) {
    for (unsigned iteration = 0; iteration < 100; iteration++) {
        reset_fixture(); struct AddrSpace *a = addrspace_alloc(32); a->vmas = (void *)1;
        pthread_t t[8];
        for (unsigned i = 0; i < 8; i++) addrspace_pin(a);
        for (unsigned i = 0; i < 8; i++) CHECK(pthread_create(&t[i], NULL, drop_pin, a) == 0, "create pin teardown");
        addrspace_unref(a);
        for (unsigned i = 0; i < 8; i++) CHECK(pthread_join(t[i], NULL) == 0, "join pin teardown");
        CHECK(drains == 1 && destroys == 1 && frees == 1, "concurrent final drops free exactly once");
    }
}
int main(void) {
    serial_cases(); during_drain(); simultaneous_drops();
    puts("PASS actual AddrSpace lifecycle: serial, paused drain, concurrent final drops");
    return 0;
}
