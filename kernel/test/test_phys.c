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

    // Drain magazines BEFORE taking the baseline. Phase 1 didn't need
    // this — no SLUB allocations happened pre-test so the magazine was
    // empty. P2-A's proc_init + thread_init each kmem_cache_create +
    // kmem_cache_alloc, which trigger first-time slab_new calls, which
    // refill the order-0 magazine to half-full (8) and consume one or
    // two pages, leaving ~5 pages resident at test entry. Without a
    // pre-drain, the post-drain comparison reads HIGHER than baseline
    // by the resident count and the test reports false drift.
    magazines_drain_all();
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

// 10 000-iteration alloc/free leak check (ROADMAP §4.2 exit criterion).
// Cycles a single order-0 page through alloc → free 10 000 times,
// drains magazines, verifies the free-page count exactly matches the
// baseline. Catches per-cycle leaks of any size that compound over a
// realistic kernel uptime.
//
// Why a SINGLE pointer (not 10000 simultaneous): the 10K array would
// overflow the boot stack (10000 × 8 = 80 KiB; boot stack is 16 KiB).
// Sequential single-allocation is the right test for "leak per
// alloc/free pair" — a leak would manifest as free-count drift that
// compounds; the magazine refill/drain hysteresis is exercised
// implicitly across iterations.
//
// Test runs in <100 ms on QEMU virt; fits inside tools/test.sh's
// 10-second timeout with margin.
void test_phys_leak_10k(void) {
    // Same robustness rule as alloc_smoke: drain pre-baseline so the
    // post-drain comparison is exact regardless of resident magazine
    // pages at test entry.
    magazines_drain_all();
    u64 baseline = phys_free_pages();

    for (unsigned i = 0; i < 10000; i++) {
        struct page *p = alloc_pages(0, KP_ZERO);
        TEST_ASSERT(p != NULL, "alloc_pages(0) returned NULL mid-10k");
        free_pages(p, 0);
    }

    magazines_drain_all();

    u64 after = phys_free_pages();
    TEST_ASSERT(after == baseline,
        "phys_free_pages drift after 10000-iter alloc/free");
}
