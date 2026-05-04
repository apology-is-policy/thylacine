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
