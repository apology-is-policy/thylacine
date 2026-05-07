// P3-Bcb / P3-Db: per-Proc page-table allocator tests.
//
// Three+1 smoke tests:
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
//   proc.ttbr0_swap_smoke
//     Two rfork'd children record their live TTBR0_EL1; verify each
//     equals (asid<<48) | pgtable_root.
//
//   proc.pgtable_destroy_walk_releases_subtables  (P3-Db; closes
//                                                  trip-hazard #116)
//     Manually install a 3-deep sub-table chain (L1 → L2 → L3) under
//     the Proc's L0; drop the Proc; verify all 4 page-table pages
//     return to buddy (proc_pgtable_destroy walks the tree, not just
//     the L0 root).
//
// At v1.0 P3-Db the page table sits mostly empty — burrow_map installs
// only VMAs (not PTEs); demand paging via P3-Dc populates sub-tables
// on fault. The destroy_walk test exercises the walker by manually
// installing the tree the way demand paging will once it lands.

#include "test.h"

#include "../../arch/arm64/asid.h"
#include "../../arch/arm64/mmu.h"

#include "../../mm/phys.h"

#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

void test_proc_pgtable_alloc_smoke(void);
void test_proc_pgtable_lifecycle_stress(void);
void test_proc_ttbr0_swap_smoke(void);
void test_proc_pgtable_destroy_walk_releases_subtables(void);

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

// =============================================================================
// P3-Db: proc_pgtable_destroy walks the L0 → L1 → L2 → L3 tree.
// =============================================================================

// Local PTE-bit duplicates for table-descriptor construction. The
// production constants live in arch/arm64/mmu.h; we duplicate just the
// two bits we need so the test stays a self-contained unit and isn't
// fragile to PTE-bit-layout refactors elsewhere.
#define PTE_BIT_VALID       (1ull << 0)
#define PTE_BIT_TYPE_TABLE  (1ull << 1)

static inline u64 mk_table_pte(paddr_t next_table_pa) {
    return next_table_pa | PTE_BIT_VALID | PTE_BIT_TYPE_TABLE;
}

void test_proc_pgtable_destroy_walk_releases_subtables(void) {
    // Snapshot free-page count BEFORE we allocate anything. proc_alloc
    // may consume non-pgtable buddy pages too (struct Proc via SLUB
    // cache, handle table fill, etc.) — but proc_free releases them
    // symmetrically, so the round-trip check `free_after == free_before`
    // is the meaningful invariant.
    u64 free_before = phys_free_pages();

    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    TEST_ASSERT(p->pgtable_root != 0, "pgtable_root is 0");

    // Allocate three sub-tables (L1, L2, L3). Each is one 4 KiB page.
    struct page *l1_pg = alloc_pages(0, KP_ZERO);
    struct page *l2_pg = alloc_pages(0, KP_ZERO);
    struct page *l3_pg = alloc_pages(0, KP_ZERO);
    TEST_ASSERT(l1_pg && l2_pg && l3_pg, "alloc_pages for sub-tables failed");

    paddr_t l1_pa = page_to_pa(l1_pg);
    paddr_t l2_pa = page_to_pa(l2_pg);
    paddr_t l3_pa = page_to_pa(l3_pg);

    // Install the chain. L0[0] → L1; L1[0] → L2; L2[0] → L3; L3[0]
    // unset (we don't install a leaf — leaf pages are owned by the VMA
    // layer; the destroy walker correctly DOES NOT free them).
    u64 *l0 = (u64 *)pa_to_kva(p->pgtable_root);
    u64 *l1 = (u64 *)pa_to_kva(l1_pa);
    u64 *l2 = (u64 *)pa_to_kva(l2_pa);
    l0[0] = mk_table_pte(l1_pa);
    l1[0] = mk_table_pte(l2_pa);
    l2[0] = mk_table_pte(l3_pa);

    // Drop the Proc. proc_free → proc_pgtable_destroy walks the tree,
    // freeing L1, L2, L3 sub-tables AND the L0 root. asid_free fires
    // alongside, but ASIDs are slot indices not buddy pages — they
    // don't affect free_pages.
    p->state = 2;     // PROC_STATE_ZOMBIE
    proc_free(p);

    // Round-trip: free count returns to baseline IFF the walker freed
    // every sub-table. Pre-P3-Db implementation freed only the L0 →
    // free_after = free_before - 3 (L1+L2+L3 leaked).
    u64 free_after = phys_free_pages();
    TEST_EXPECT_EQ(free_after, free_before,
        "proc_pgtable_destroy must walk + free ALL sub-tables; "
        "if 3 pages are missing, only L0 got freed (the bug this test pins)");
}
