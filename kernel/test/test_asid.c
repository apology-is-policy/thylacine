// P3-Ba: ASID allocator tests.
//
// Three smoke tests:
//
//   asid.alloc_unique
//     Allocate a small batch of ASIDs, verify they are pairwise distinct
//     and all in [ASID_USER_FIRST, ASID_USER_LAST]. Free them.
//
//   asid.free_reuses
//     Allocate, free, allocate. Verify the second allocation reuses the
//     freed ASID (LIFO free-list discipline). The TLB-flush-before-reuse
//     guarantee is structural — verifying it requires runtime TLB
//     observation which we don't have at v1.0; we rely on the asid.c
//     sequencing audit.
//
//   asid.inflight_count
//     Verify asid_inflight() reports the running balance accurately
//     across alloc + free.
//
// Exhaustion testing (alloc beyond ASID_USER_LAST) is deliberately NOT
// in the smoke suite — the extinction kills the kernel. A fault-matrix
// test could exercise it; deferred to v1.0 close.

#include "test.h"

#include "../../arch/arm64/asid.h"

#include <thylacine/types.h>

void test_asid_alloc_unique(void);
void test_asid_free_reuses(void);
void test_asid_inflight_count(void);

void test_asid_alloc_unique(void) {
    enum { N = 8 };
    u16 asids[N];

    u64 alloc_before = asid_total_allocated();
    unsigned inflight_before = asid_inflight();

    for (int i = 0; i < N; i++) {
        asids[i] = asid_alloc();
        TEST_ASSERT(asids[i] >= ASID_USER_FIRST,
            "asid_alloc returned below ASID_USER_FIRST");
        TEST_ASSERT(asids[i] <= ASID_USER_LAST,
            "asid_alloc returned above ASID_USER_LAST");
    }

    // Pairwise-distinct check.
    for (int i = 0; i < N; i++) {
        for (int j = i + 1; j < N; j++) {
            TEST_ASSERT(asids[i] != asids[j],
                "asid_alloc returned duplicate ASIDs");
        }
    }

    TEST_EXPECT_EQ(asid_total_allocated() - alloc_before, (u64)N,
        "asid_total_allocated did not advance by N");
    TEST_EXPECT_EQ(asid_inflight() - inflight_before, (unsigned)N,
        "asid_inflight did not advance by N");

    // Free in reverse order — exercises the LIFO free-list discipline.
    u64 free_before = asid_total_freed();
    for (int i = N - 1; i >= 0; i--) {
        asid_free(asids[i]);
    }

    TEST_EXPECT_EQ(asid_total_freed() - free_before, (u64)N,
        "asid_total_freed did not advance by N");
    TEST_EXPECT_EQ(asid_inflight(), inflight_before,
        "asid_inflight did not return to pre-test level");
}

void test_asid_free_reuses(void) {
    // Alloc → free → alloc; the second alloc should pop the just-freed
    // ASID from the LIFO free-list (not advance the monotonic counter).
    u16 a1 = asid_alloc();
    u16 a2 = asid_alloc();
    TEST_ASSERT(a1 != a2, "consecutive allocs returned same ASID");

    asid_free(a1);
    u16 a3 = asid_alloc();
    TEST_EXPECT_EQ(a3, a1,
        "post-free alloc did not reuse the freed ASID via LIFO free-list");

    asid_free(a2);
    asid_free(a3);
}

void test_asid_inflight_count(void) {
    unsigned baseline = asid_inflight();

    u16 a = asid_alloc();
    TEST_EXPECT_EQ(asid_inflight(), baseline + 1u, "inflight after alloc");

    u16 b = asid_alloc();
    TEST_EXPECT_EQ(asid_inflight(), baseline + 2u, "inflight after 2nd alloc");

    asid_free(a);
    TEST_EXPECT_EQ(asid_inflight(), baseline + 1u, "inflight after free");

    asid_free(b);
    TEST_EXPECT_EQ(asid_inflight(), baseline,
        "inflight returned to baseline after both frees");
}
