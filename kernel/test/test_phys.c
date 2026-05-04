// phys.c smoke test — refactored from boot_main's inline version.
//
// Exercises the magazine fast path (orders 0 and 9), the magazine
// refill / drain machinery, AND a non-magazine order (>= 10 bypasses
// magazines and hits buddy direct). Verifies the free-page count
// returns to baseline after `magazines_drain_all`.

#include "test.h"

#include "../../mm/phys.h"
#include "../../mm/magazines.h"
#include <thylacine/page.h>
#include <thylacine/types.h>

#define SMOKE_N 256

void test_phys_alloc_smoke(void) {
    static struct page *pages[SMOKE_N];   // static so we don't crowd boot stack

    u64 baseline = phys_free_pages();

    // Order-0 (4 KiB) allocations — exercises magazine[0] refill/drain.
    for (int i = 0; i < SMOKE_N; i++) {
        pages[i] = alloc_pages(0, KP_ZERO);
        TEST_ASSERT(pages[i] != NULL, "alloc_pages(0) returned NULL");
    }
    for (int i = 0; i < SMOKE_N; i++) {
        free_pages(pages[i], 0);
    }

    // Order-9 (2 MiB) round-trip — exercises magazine[1].
    struct page *big2 = alloc_pages(9, KP_ZERO);
    TEST_ASSERT(big2 != NULL, "alloc_pages(9) returned NULL");
    free_pages(big2, 9);

    // Order-10 (4 MiB) — bypasses magazines, hits buddy direct.
    struct page *big10 = alloc_pages(10, 0);
    TEST_ASSERT(big10 != NULL, "alloc_pages(10) returned NULL");
    free_pages(big10, 10);

    // Drain magazines so the comparison is exact (otherwise the
    // refill residency holds 7-8 pages per magazine).
    magazines_drain_all();

    u64 after = phys_free_pages();
    TEST_ASSERT(after == baseline,
        "phys_free_pages drift: free count not restored after smoke");
}
