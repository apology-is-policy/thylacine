#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#define CHECK(x, why) do { if (!(x)) { fputs(why "\n", stderr); exit(1); } } while (0)
typedef uint64_t u64, paddr_t, caps_t;
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
static void spin_lock(spin_lock_t *p) {
    while (__atomic_exchange_n(&p->value, 1, __ATOMIC_ACQUIRE)) sched_yield();
}
static void spin_unlock(spin_lock_t *p) { __atomic_store_n(&p->value, 0, __ATOMIC_RELEASE); }

static _Noreturn void extinction(const char *s) { fprintf(stderr, "extinction: %s\n", s); exit(1); }

// Fixture shapes of the mapping list the actual device quiesce walks: the
// fields it reads. The device resets themselves are hardware, so doubles that
// record when each ran.
#define BURROW_TYPE_MMIO 2
struct KObj_MMIO { u64 pa; size_t size; };
struct KObj_PCI { u32 hostmem_burrows; unsigned full, dma_only; };
struct Burrow { int type; struct KObj_MMIO *kobj_mmio; struct KObj_PCI *kobj_pci; };
struct Vma { struct Vma *next; struct Burrow *burrow; };
static struct Vma plain_vma;
static unsigned stamp, resets, last_reset, drained_at;
static int virtio_mmio_reset_in_range(u64 pa, size_t size) {
    CHECK(pa == 0x0a000000u && size == 0x200, "mapped device identity");
    resets++; last_reset = ++stamp; return 1;
}
static bool kobj_pci_quiesce(struct KObj_PCI *k) { k->full++; resets++; last_reset = ++stamp; return true; }
static bool kobj_pci_quiesce_dma_only(struct KObj_PCI *k) { k->dma_only++; resets++; last_reset = ++stamp; return true; }

/* ACTUAL_ADDRSPACE_DECL */
// External, as vma.h declares it: a mutant that removes destroy's drain must still compile.
void vma_drain_in(struct AddrSpace *as) {
    drains++; drained_at = ++stamp;
    pthread_mutex_lock(&mu);
    entered_drain = true;
    pthread_cond_broadcast(&cv);
    while (pause_drain && !release_drain) pthread_cond_wait(&cv, &mu);
    as->vmas = NULL;
    pthread_mutex_unlock(&mu);
}
/* ACTUAL_QUIESCE */

/* ACTUAL_ADDRSPACE_LIFECYCLE */
// Actual AddrSpace declaration/lifecycle are inserted before this fixture.
// Allocation, page-table destruction, mapping drain and device resets are
// controlled doubles; the device quiesce walk is actual.
// This checks C lifetime ordering, not guest MMU/TLB or real device behavior.
static void *drop_owner(void *p) { addrspace_unref(p); return NULL; }
static void *drop_pin(void *p) { addrspace_unpin(p); return NULL; }
static void reset_fixture(void) {
    allocations = frees = tables = destroys = drains = 0;
    stamp = resets = last_reset = drained_at = 0;
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
    a->vmas = &plain_vma;
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

    reset_fixture(); a = addrspace_alloc(32); a->vmas = &plain_vma;
    addrspace_pin(a); addrspace_unpin(a);
    CHECK(drains == 0 && frees == 0 && addrspace_owner_count(a) == 1, "early pin release preserves owner");
    addrspace_unref(a);
    CHECK(drains == 1 && frees == 1 && destroys == 1, "ordinary last owner still frees");
    addrspace_unref(NULL); addrspace_unpin(NULL);
    CHECK(addrspace_ref_count(NULL) == 0 && addrspace_owner_count(NULL) == 0, "null lifetime helpers");
}
static void during_drain(void) {
    reset_fixture(); struct AddrSpace *a = addrspace_alloc(32); a->vmas = &plain_vma;
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
        reset_fixture(); struct AddrSpace *a = addrspace_alloc(32); a->vmas = &plain_vma;
        pthread_t t[8];
        for (unsigned i = 0; i < 8; i++) addrspace_pin(a);
        for (unsigned i = 0; i < 8; i++) CHECK(pthread_create(&t[i], NULL, drop_pin, a) == 0, "create pin teardown");
        addrspace_unref(a);
        for (unsigned i = 0; i < 8; i++) CHECK(pthread_join(t[i], NULL) == 0, "join pin teardown");
        CHECK(drains == 1 && destroys == 1 && frees == 1, "concurrent final drops free exactly once");
    }
}
static void private_guards(void) {
    reset_fixture(); struct AddrSpace *a = addrspace_alloc(32);
    CHECK(addrspace_private_begin(a), "sole owner admits private guard");
    CHECK(addrspace_ref_count(a) == 2 && addrspace_owner_count(a) == 1,
          "guard pins lifetime without adding owner");
    CHECK(!addrspace_try_ref(a), "private guard refuses sharing");
    CHECK(addrspace_private_begin(a) && a->private_rings == 2, "nested private guards");
    addrspace_private_end(a);
    CHECK(!addrspace_try_ref(a), "one remaining guard still refuses sharing");
    addrspace_private_end(a);
    CHECK(a->private_rings == 0 && addrspace_ref_count(a) == 1, "final guard releases pin and exclusion");
    CHECK(addrspace_try_ref(a), "sharing resumes after retirement");
    CHECK(!addrspace_private_begin(a), "shared image refuses private guard");
    addrspace_unref(a); addrspace_unref(a);
    CHECK(frees == 1, "private guard serial balance");
    reset_fixture(); a = addrspace_alloc(32);
    CHECK(addrspace_private_begin(a), "exit guard");
    addrspace_unref(a);
    CHECK(drains == 1 && frees == 0 && addrspace_owner_count(a) == 0,
          "guard outlives final process owner");
    CHECK(!addrspace_private_begin(a), "guard cannot resurrect ownerless image");
    addrspace_private_end(a);
    CHECK(frees == 1 && destroys == 1, "retired guard releases ownerless descriptor");
}
struct race_arg { struct AddrSpace *as; bool private, admitted; };
static unsigned race_ready;
static bool race_go;
static void *race_admit(void *arg) {
    struct race_arg *a = arg;
    __atomic_fetch_add(&race_ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&race_go, __ATOMIC_ACQUIRE)) sched_yield();
    a->admitted = a->private ? addrspace_private_begin(a->as) : addrspace_try_ref(a->as);
    return NULL;
}
static void private_vs_share(void) {
    for (unsigned i = 0; i < 200; i++) {
        reset_fixture(); race_ready = 0; race_go = false;
        struct AddrSpace *a = addrspace_alloc(32);
        struct race_arg args[2] = {{a,true,false},{a,false,false}};
        pthread_t t[2];
        for (unsigned j = 0; j < 2; j++) CHECK(pthread_create(&t[j], NULL, race_admit, &args[j]) == 0, "race create");
        while (__atomic_load_n(&race_ready, __ATOMIC_ACQUIRE) != 2) sched_yield();
        __atomic_store_n(&race_go, true, __ATOMIC_RELEASE);
        for (unsigned j = 0; j < 2; j++) CHECK(pthread_join(t[j], NULL) == 0, "race join");
        CHECK(args[0].admitted != args[1].admitted, "private and shared admissions are mutually exclusive");
        if (args[0].admitted) addrspace_private_end(a);
        if (args[1].admitted) addrspace_unref(a);
        addrspace_unref(a); CHECK(frees == 1 && drains == 1, "race ownership balanced");
    }
}
static void mapped_devices(void) {
    // The last owner stops every device mapped in the space, once, before the
    // drain frees the buffers they were handed; an owner that leaves while
    // another remains stops none.
    reset_fixture();
    struct KObj_MMIO mmio = {0x0a000000u, 0x200}, bar = {0x10000000u, 0x1000};
    struct KObj_PCI full = {0, 0, 0}, dma = {1, 0, 0};
    struct Burrow virtio = {BURROW_TYPE_MMIO, &mmio, NULL}, anon = {0, NULL, NULL};
    struct Burrow pci_full = {BURROW_TYPE_MMIO, &bar, &full}, pci_dma = {BURROW_TYPE_MMIO, &bar, &dma};
    struct Vma v[4] = {{&v[1], &virtio}, {&v[2], &anon}, {&v[3], &pci_full}, {NULL, &pci_dma}};
    struct AddrSpace *a = addrspace_alloc(32); a->vmas = v;
    addrspace_ref(a); addrspace_unref(a);
    CHECK(resets == 0 && drains == 0 && a->vmas == v, "nonfinal owner stops no mapped device");
    addrspace_unref(a);
    CHECK(resets == 3 && full.full == 1 && !full.dma_only && dma.dma_only == 1 && !dma.full &&
          drains == 1 && last_reset < drained_at, "last owner stops each mapped device once, before the drain");
    CHECK(frees == 1 && destroys == 1, "device teardown frees once");
}
static void split_interval(void) {
    // Between the last owner's release and its destroy (exec resets the handle
    // table there) the space is ownerless but still held by that owner's
    // lifetime reference, and nothing of it has been torn down.
    reset_fixture();
    struct KObj_MMIO mmio = {0x0a000000u, 0x200};
    struct Burrow virtio = {BURROW_TYPE_MMIO, &mmio, NULL};
    struct Vma v = {NULL, &virtio};
    struct AddrSpace *a = addrspace_alloc(32); a->vmas = &v;
    addrspace_pin(a);
    CHECK(addrspace_release(a), "the last owner's release reports last");
    CHECK(addrspace_owner_count(a) == 0 && addrspace_ref_count(a) == 2,
          "between release and destroy the last owner's lifetime is held");
    CHECK(a->vmas == &v && drains == 0 && resets == 0 && frees == 0 && destroys == 0,
          "release tears nothing down before destroy");
    addrspace_unpin(a);
    CHECK(addrspace_ref_count(a) == 1 && frees == 0 && destroys == 0 && a->vmas == &v,
          "the retained owner reference outlives a pin dropped between release and destroy");
    addrspace_destroy(a);
    CHECK(resets == 1 && drains == 1 && last_reset < drained_at, "destroy stops the mapped device before the drain");
    CHECK(frees == 1 && destroys == 1, "destroy frees exactly once");
}
int main(int argc, char **argv) {
    // "split" runs the release/destroy interval alone, so a control can reach
    // it past the earlier cases that would otherwise stop the same mutant.
    if (argc > 1 && strcmp(argv[1], "split") == 0) {
        split_interval();
        puts("PASS actual AddrSpace release/destroy interval");
        return 0;
    }
    serial_cases(); during_drain(); simultaneous_drops(); private_guards(); private_vs_share(); mapped_devices();
    split_interval();
    puts("PASS actual AddrSpace lifecycle: serial, paused drain, concurrent final drops, private/shared exclusion, mapped devices, release/destroy interval");
    return 0;
}
