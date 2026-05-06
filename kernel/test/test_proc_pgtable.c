// P3-Bcb: per-Proc page-table allocator tests.
//
// Two smoke tests:
//
//   proc.pgtable_alloc_smoke
//     proc_alloc returns a Proc with non-zero pgtable_root and an asid
//     in [ASID_USER_FIRST, ASID_USER_LAST]. The pgtable_root is page-
//     aligned. proc_free releases both — verified by asid inflight
//     decrementing.
//
//   proc.pgtable_lifecycle_stress
//     Many alloc/free cycles. Verify proc_total_created /
//     proc_total_destroyed advance in step (no leak), and asid_inflight
//     returns to baseline (no ASID leak).
//
// At v1.0 P3-Bcb the page table sits unused — P3-Bd loads it into
// TTBR0_EL1 at context switch. These tests verify the lifecycle
// plumbing (alloc, store-in-Proc, free, ASID accounting) without
// requiring TTBR0 swap.

#include "test.h"

#include "../../arch/arm64/asid.h"

#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>

void test_proc_pgtable_alloc_smoke(void);
void test_proc_pgtable_lifecycle_stress(void);

void test_proc_pgtable_alloc_smoke(void) {
    unsigned asid_inflight_before = asid_inflight();

    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc returned NULL");

    TEST_ASSERT(p->pgtable_root != 0,
        "proc_alloc did not install a pgtable_root");
    TEST_ASSERT((p->pgtable_root & (PAGE_SIZE - 1)) == 0,
        "pgtable_root is not page-aligned");

    TEST_ASSERT(p->asid >= ASID_USER_FIRST,
        "proc_alloc asid below ASID_USER_FIRST");
    TEST_ASSERT(p->asid <= ASID_USER_LAST,
        "proc_alloc asid above ASID_USER_LAST");

    TEST_EXPECT_EQ(asid_inflight(), asid_inflight_before + 1u,
        "asid_inflight did not advance by 1 after proc_alloc");

    // Drive Proc to ZOMBIE so proc_free's lifecycle gate passes.
    p->state = 2;     // PROC_STATE_ZOMBIE
    proc_free(p);

    TEST_EXPECT_EQ(asid_inflight(), asid_inflight_before,
        "asid_inflight did not return to baseline after proc_free");
}

void test_proc_pgtable_lifecycle_stress(void) {
    enum { ITERS = 64 };

    u64      created_before  = proc_total_created();
    u64      destroyed_before = proc_total_destroyed();
    unsigned asid_before     = asid_inflight();

    for (int i = 0; i < ITERS; i++) {
        struct Proc *p = proc_alloc();
        TEST_ASSERT(p != NULL, "proc_alloc failed mid-stress");
        TEST_ASSERT(p->pgtable_root != 0,
            "proc_alloc didn't install pgtable_root mid-stress");
        TEST_ASSERT(p->asid >= ASID_USER_FIRST && p->asid <= ASID_USER_LAST,
            "proc_alloc asid out of range mid-stress");

        p->state = 2;     // PROC_STATE_ZOMBIE
        proc_free(p);
    }

    TEST_ASSERT(proc_total_created()   - created_before   == ITERS,
        "proc_total_created didn't advance by ITERS");
    TEST_ASSERT(proc_total_destroyed() - destroyed_before == ITERS,
        "proc_total_destroyed didn't advance by ITERS (leak?)");
    TEST_EXPECT_EQ(asid_inflight(), asid_before,
        "asid_inflight didn't return to baseline (ASID leak?)");
}
