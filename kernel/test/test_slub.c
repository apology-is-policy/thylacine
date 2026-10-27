// slub.c smoke test — refactored from boot_main's inline version.
//
// Exercises:
//   - Many small allocations from kmalloc-8 (forces multiple slab
//     pages — each 4 KiB slab holds 512 8-byte objects).
//   - Mixed-size kmalloc round-trips, one per cache size.
//   - Large allocation (8 KiB → bypasses slab, hits alloc_pages
//     directly via the kmalloc large-request path).
//   - Custom typed cache via kmem_cache_create / alloc / free /
//     destroy.
// Then verifies phys_free_pages() returns to baseline post-drain.

#include "test.h"

#include "../../mm/phys.h"
#include "../../mm/magazines.h"
#include "../../mm/slub.h"
#include <thylacine/page.h>
#include <thylacine/types.h>

#define KMEM_SMOKE_SMALL_N 1500

void test_slub_kmem_smoke(void) {
    static void *smalls[KMEM_SMOKE_SMALL_N];

    u64 baseline = phys_free_pages();

    // Many kmalloc(8) — exercises slab-fill across multiple slab
    // pages. 1500 / 512 = 2.93 slab pages allocated and drained.
    for (int i = 0; i < KMEM_SMOKE_SMALL_N; i++) {
        smalls[i] = kmalloc(8, KP_ZERO);
        TEST_ASSERT(smalls[i] != NULL, "kmalloc(8) returned NULL");
    }
    for (int i = 0; i < KMEM_SMOKE_SMALL_N; i++) {
        kfree(smalls[i]);
    }

    // Mixed-size round-trip — each cache exercised once.
    size_t sizes[] = { 16, 64, 128, 512, 2048 };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        void *p = kmalloc(sizes[i], 0);
        TEST_ASSERT(p != NULL, "kmalloc returned NULL for mixed-size case");
        kfree(p);
    }

    // Large allocation: bypasses slab, hits alloc_pages directly.
    void *big = kzalloc(8192, 0);
    TEST_ASSERT(big != NULL, "kzalloc(8192) returned NULL");
    kfree(big);

    // Dynamic cache via kmem_cache_create.
    struct kmem_cache *c = kmem_cache_create("smoke-typed", 100, 16, 0);
    TEST_ASSERT(c != NULL, "kmem_cache_create returned NULL");

    void *t1 = kmem_cache_alloc(c, KP_ZERO);
    void *t2 = kmem_cache_alloc(c, KP_ZERO);
    TEST_ASSERT(t1 != NULL, "kmem_cache_alloc returned NULL (t1)");
    TEST_ASSERT(t2 != NULL, "kmem_cache_alloc returned NULL (t2)");
    kmem_cache_free(c, t1);
    kmem_cache_free(c, t2);
    kmem_cache_destroy(c);

    magazines_drain_all();
    u64 after = phys_free_pages();
    TEST_ASSERT(after == baseline,
        "phys_free_pages drift: free count not restored after kmem smoke");
}

// RW-1 A-F1 regression: a near-SIZE_MAX kmalloc must fail, not wrap.
// Pre-fix, `(n + PAGE_SIZE - 1) >> PAGE_SHIFT` wrapped for n within
// PAGE_SIZE-1 of SIZE_MAX, so the large path computed pages ~ 0,
// order 0, and returned ONE page for the request — a heap-overflow
// primitive that also defeated kcalloc's own n*size guard (size==1
// admits exactly this range). Each leg below returns non-NULL on the
// pre-fix code, failing the assert.
void test_slub_kmalloc_overflow_guard(void) {
    size_t size_max = ~(size_t)0;

    TEST_ASSERT(kmalloc(size_max, 0) == NULL,
                "kmalloc(SIZE_MAX) must return NULL, not wrap");
    TEST_ASSERT(kmalloc(size_max - (PAGE_SIZE - 2), 0) == NULL,
                "kmalloc just inside the wrap range must return NULL");
    TEST_ASSERT(kcalloc(size_max - 100, 1, 0) == NULL,
                "kcalloc(SIZE_MAX-100, 1) must return NULL "
                "(its n*size guard alone does not catch size==1)");

    // Contract pin (passes pre-fix too): a huge but non-wrapping
    // request fails via alloc_locked's order > MAX_ORDER reject.
    TEST_ASSERT(kmalloc((size_t)1 << 40, 0) == NULL,
                "kmalloc(1 TiB) must return NULL via the order bound");
}

// RW-1 F-S1 / F-S3 regression: the kmem_cache_destroy liveness guard +
// the create-time too-large-align reject.
//
// F-S1 pins the quantity the destroy guard tests (kmem_cache_live_count =
// alloc_count - free_count) across a partial slab -- the case the old
// nr_full-only guard missed. The extinction arm itself is not unit-testable
// (it halts the kernel by design, like the nr_full guard), so we verify the
// guard's INPUT is exact and that a balanced cache destroys cleanly.
//
// F-S3 verifies an alignment too large for a single-page slab is rejected
// at create (pre-fix it built a cache with objects_per_slab == 0 whose
// first alloc NULL-deref'd the empty freelist).
void test_slub_cache_destroy_guards(void) {
    u64 baseline = phys_free_pages();

    // F-S1: a fresh cache; one partial slab; live-count tracks exactly.
    struct kmem_cache *c = kmem_cache_create("guard-typed", 48, 8, 0);
    TEST_ASSERT(c != NULL, "kmem_cache_create(48,8) returned NULL");
    TEST_EXPECT_EQ((int)kmem_cache_live_count(c), 0, "fresh cache: 0 live");

    void *a = kmem_cache_alloc(c, 0);
    void *b = kmem_cache_alloc(c, 0);
    void *d = kmem_cache_alloc(c, 0);
    TEST_ASSERT(a && b && d, "three allocs from guard cache");
    TEST_EXPECT_EQ((int)kmem_cache_live_count(c), 3, "3 live after 3 allocs");

    kmem_cache_free(c, b);
    TEST_EXPECT_EQ((int)kmem_cache_live_count(c), 2,
                   "2 live after 1 free (partial slab, still live)");
    kmem_cache_free(c, a);
    kmem_cache_free(c, d);
    TEST_EXPECT_EQ((int)kmem_cache_live_count(c), 0, "0 live after all freed");

    // Now destroy is provably safe (live == 0) -- must not extinct.
    kmem_cache_destroy(c);

    // F-S3: an alignment whose rounded object exceeds one page -> reject.
    TEST_ASSERT(kmem_cache_create("toobig-align", 64, 8192, 0) == NULL,
                "align 8192 (> PAGE_SIZE) must be rejected, not yield a "
                "0-object cache");

    magazines_drain_all();
    TEST_ASSERT(phys_free_pages() == baseline,
                "phys_free_pages drift after destroy-guard test");
}

// 10 000-iteration kmalloc/kfree leak check (ROADMAP §4.2 exit
// criterion). Cycles a single 64-byte allocation through kmalloc →
// kfree 10 000 times. Single pointer (10K array would overflow the
// boot stack). Drains magazines between sub-runs to flush slab-page
// residency. Verifies phys_free_pages returns to baseline.
//
// 64 bytes is in the kmalloc-64 cache; one slab page holds 64 such
// objects, so the test exercises slab-page acquire/release ~156
// times across 10 000 iterations (with magazine residency dampening).
//
// Test runs in <50 ms on QEMU virt; well within the 10-s test
// timeout.
void test_slub_leak_10k(void) {
    u64 baseline = phys_free_pages();

    for (unsigned i = 0; i < 10000; i++) {
        void *p = kmalloc(64, 0);
        TEST_ASSERT(p != NULL, "kmalloc(64) returned NULL mid-10k");
        kfree(p);
    }

    magazines_drain_all();

    u64 after = phys_free_pages();
    TEST_ASSERT(after == baseline,
        "phys_free_pages drift after 10000-iter kmalloc/kfree");
}
