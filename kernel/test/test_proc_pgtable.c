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
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

void test_proc_pgtable_alloc_smoke(void);
void test_proc_pgtable_lifecycle_stress(void);
void test_proc_ttbr0_swap_smoke(void);

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

// =============================================================================
// P3-Bdb: TTBR0 swap on context switch.
// =============================================================================

// Cross-thread observation slots. Two children each record their live
// TTBR0_EL1, their Proc's pgtable_root, and their Proc's ASID. The
// parent verifies post-reap.
static volatile u64 g_ttbr0_test_ttbr0[2];
static volatile u64 g_ttbr0_test_pgtable[2];
static volatile u32 g_ttbr0_test_asid[2];

static void ttbr0_swap_child(void *arg) {
    int idx = (int)(uintptr_t)arg;

    // Read TTBR0_EL1 — the value cpu_switch_context loaded when
    // switching into us. It MUST equal (proc->asid << 48) | proc->pgtable_root.
    u64 ttbr0;
    __asm__ __volatile__("mrs %0, ttbr0_el1" : "=r"(ttbr0));

    struct Thread *t = current_thread();
    g_ttbr0_test_ttbr0[idx]   = ttbr0;
    g_ttbr0_test_pgtable[idx] = t->proc->pgtable_root;
    g_ttbr0_test_asid[idx]    = t->proc->asid;

    exits("ok");
}

void test_proc_ttbr0_swap_smoke(void) {
    // Reset slots so a stale value from a prior test wouldn't pass.
    for (int i = 0; i < 2; i++) {
        g_ttbr0_test_ttbr0[i]   = 0;
        g_ttbr0_test_pgtable[i] = 0;
        g_ttbr0_test_asid[i]    = 0;
    }

    int pid0 = rfork(RFPROC, ttbr0_swap_child, (void *)(uintptr_t)0);
    TEST_ASSERT(pid0 > 0, "rfork pid0 failed");
    int pid1 = rfork(RFPROC, ttbr0_swap_child, (void *)(uintptr_t)1);
    TEST_ASSERT(pid1 > 0, "rfork pid1 failed");

    int status;
    int reaped0 = wait_pid(&status);
    int reaped1 = wait_pid(&status);
    TEST_ASSERT(reaped0 > 0 && reaped1 > 0, "wait_pid failed");

    for (int i = 0; i < 2; i++) {
        TEST_ASSERT(g_ttbr0_test_pgtable[i] != 0,
            "child pgtable_root is 0 (rfork didn't allocate?)");
        TEST_ASSERT(g_ttbr0_test_asid[i] >= ASID_USER_FIRST &&
                    g_ttbr0_test_asid[i] <= ASID_USER_LAST,
            "child asid out of valid range");

        u64 expected = ((u64)g_ttbr0_test_asid[i] << 48) |
                       g_ttbr0_test_pgtable[i];
        TEST_EXPECT_EQ(g_ttbr0_test_ttbr0[i], expected,
            "live TTBR0_EL1 doesn't match (asid<<48)|pgtable_root");
    }

    // Two distinct children must have distinct ASIDs (else asid_alloc
    // is broken or context switch loaded the wrong TTBR0).
    TEST_ASSERT(g_ttbr0_test_asid[0] != g_ttbr0_test_asid[1],
        "two rfork'd children share the same ASID (asid_alloc bug?)");

    // And distinct pgtable roots.
    TEST_ASSERT(g_ttbr0_test_pgtable[0] != g_ttbr0_test_pgtable[1],
        "two rfork'd children share the same pgtable_root "
        "(proc_pgtable_create bug?)");
}
