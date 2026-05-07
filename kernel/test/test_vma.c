// P3-Da: VMA tree tests.
//
// Six smoke tests for the per-Proc VMA list:
//
//   vma.alloc_free_smoke
//     Basic alloc + free; verifies counters advance.
//
//   vma.alloc_constraints
//     Constraint validation: zero-length, unaligned, overlapping range,
//     W+X reject all return NULL.
//
//   vma.insert_lookup_smoke
//     Insert several non-overlapping VMAs into a Proc; verify lookup
//     finds them at every covered address; misses on uncovered addresses.
//
//   vma.insert_overlap_rejected
//     Insert an existing VMA's range overlap → rejected with -1.
//
//   vma.insert_sorted_invariant
//     Insert in mixed order; verify the resulting list is sorted by
//     vaddr_start ascending.
//
//   vma.drain_releases_all
//     vma_drain frees every VMA + decrements BURROW mapping_count.

#include "test.h"

#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/vma.h>
#include <thylacine/burrow.h>

void test_vma_alloc_free_smoke(void);
void test_vma_alloc_constraints(void);
void test_vma_insert_lookup_smoke(void);
void test_vma_insert_overlap_rejected(void);
void test_vma_insert_sorted_invariant(void);
void test_vma_drain_releases_all(void);

// User-VA test ranges. We use bits 47:0 directly (TTBR0 user-half).
// 0x10000000 = 256 MiB; well within any reasonable user-VA bound.
#define VA_BASE       0x10000000ull
#define VA_PAGE       4096ull
#define VA_2MIB       (2ull * 1024 * 1024)

void test_vma_alloc_free_smoke(void) {
    u64 alloc_before = vma_total_allocated();
    u64 free_before  = vma_total_freed();

    struct Burrow *burrow = burrow_create_anon(VA_PAGE);
    TEST_ASSERT(burrow != NULL, "burrow_create_anon failed");

    struct Vma *v = vma_alloc(VA_BASE, VA_BASE + VA_PAGE,
                              VMA_PROT_RW, burrow, 0);
    TEST_ASSERT(v != NULL, "vma_alloc failed");
    TEST_EXPECT_EQ(v->vaddr_start, VA_BASE,           "vaddr_start");
    TEST_EXPECT_EQ(v->vaddr_end,   VA_BASE + VA_PAGE, "vaddr_end");
    TEST_EXPECT_EQ(v->prot,        VMA_PROT_RW,       "prot");
    TEST_EXPECT_EQ(v->burrow,         burrow,               "burrow");
    TEST_EXPECT_EQ(v->burrow_offset,  0u,                "burrow_offset");

    TEST_EXPECT_EQ(vma_total_allocated() - alloc_before, 1ull, "allocated counter");

    vma_free(v);
    TEST_EXPECT_EQ(vma_total_freed() - free_before, 1ull, "freed counter");

    burrow_unref(burrow);
}

void test_vma_alloc_constraints(void) {
    struct Burrow *burrow = burrow_create_anon(VA_PAGE);
    TEST_ASSERT(burrow != NULL, "burrow_create_anon failed");

    // Zero-length range.
    TEST_ASSERT(vma_alloc(VA_BASE, VA_BASE, VMA_PROT_RW, burrow, 0) == NULL,
        "zero-length range should be rejected");

    // Reversed range.
    TEST_ASSERT(vma_alloc(VA_BASE + VA_PAGE, VA_BASE, VMA_PROT_RW, burrow, 0) == NULL,
        "reversed range should be rejected");

    // Unaligned start.
    TEST_ASSERT(vma_alloc(VA_BASE + 1, VA_BASE + VA_PAGE, VMA_PROT_RW, burrow, 0) == NULL,
        "unaligned start should be rejected");

    // Unaligned end.
    TEST_ASSERT(vma_alloc(VA_BASE, VA_BASE + VA_PAGE + 1, VMA_PROT_RW, burrow, 0) == NULL,
        "unaligned end should be rejected");

    // W+X reject.
    TEST_ASSERT(vma_alloc(VA_BASE, VA_BASE + VA_PAGE,
                          VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC,
                          burrow, 0) == NULL,
        "W+X should be rejected");

    // NULL BURROW.
    TEST_ASSERT(vma_alloc(VA_BASE, VA_BASE + VA_PAGE, VMA_PROT_RW, NULL, 0) == NULL,
        "NULL BURROW should be rejected");

    burrow_unref(burrow);
}

void test_vma_insert_lookup_smoke(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct Burrow *burrow = burrow_create_anon(VA_2MIB);
    TEST_ASSERT(burrow != NULL, "burrow_create_anon failed");

    // Three non-overlapping VMAs at distinct VA offsets.
    struct Vma *v1 = vma_alloc(VA_BASE,                VA_BASE +     VA_PAGE,
                               VMA_PROT_RX, burrow, 0);
    struct Vma *v2 = vma_alloc(VA_BASE + 2 * VA_PAGE,  VA_BASE + 3 * VA_PAGE,
                               VMA_PROT_RW, burrow, VA_PAGE);
    struct Vma *v3 = vma_alloc(VA_BASE + 5 * VA_PAGE,  VA_BASE + 6 * VA_PAGE,
                               VMA_PROT_READ, burrow, 2 * VA_PAGE);
    TEST_ASSERT(v1 && v2 && v3, "vma_alloc failed");

    TEST_EXPECT_EQ(vma_insert(p, v1), 0, "insert v1");
    TEST_EXPECT_EQ(vma_insert(p, v2), 0, "insert v2");
    TEST_EXPECT_EQ(vma_insert(p, v3), 0, "insert v3");

    // Lookup at covered addresses.
    TEST_EXPECT_EQ(vma_lookup(p, VA_BASE),                 v1, "lookup v1 start");
    TEST_EXPECT_EQ(vma_lookup(p, VA_BASE + VA_PAGE - 1),   v1, "lookup v1 end-1");
    TEST_EXPECT_EQ(vma_lookup(p, VA_BASE + 2 * VA_PAGE),   v2, "lookup v2 start");
    TEST_EXPECT_EQ(vma_lookup(p, VA_BASE + 5 * VA_PAGE),   v3, "lookup v3 start");

    // Lookup at uncovered addresses → NULL.
    TEST_ASSERT(vma_lookup(p, VA_BASE + VA_PAGE)     == NULL, "gap after v1");
    TEST_ASSERT(vma_lookup(p, VA_BASE + 4 * VA_PAGE) == NULL, "gap between v2 and v3");
    TEST_ASSERT(vma_lookup(p, 0)                     == NULL, "lookup before any VMA");

    // Drain + free Proc (lifecycle gate).
    vma_drain(p);
    p->state = 2;     // PROC_STATE_ZOMBIE
    proc_free(p);
    burrow_unref(burrow);
}

void test_vma_insert_overlap_rejected(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct Burrow *burrow = burrow_create_anon(VA_2MIB);
    TEST_ASSERT(burrow != NULL, "burrow_create_anon failed");

    struct Vma *v1 = vma_alloc(VA_BASE, VA_BASE + 2 * VA_PAGE,
                               VMA_PROT_RW, burrow, 0);
    TEST_ASSERT(v1 && vma_insert(p, v1) == 0, "insert v1");

    // Exact-overlap.
    struct Vma *v2 = vma_alloc(VA_BASE, VA_BASE + 2 * VA_PAGE,
                               VMA_PROT_RW, burrow, 0);
    TEST_ASSERT(v2 != NULL, "vma_alloc v2");
    TEST_EXPECT_EQ(vma_insert(p, v2), -1, "exact overlap rejected");
    vma_free(v2);

    // Partial overlap (left).
    struct Vma *v3 = vma_alloc(VA_BASE - VA_PAGE, VA_BASE + VA_PAGE,
                               VMA_PROT_RW, burrow, 0);
    TEST_ASSERT(v3 != NULL, "vma_alloc v3");
    TEST_EXPECT_EQ(vma_insert(p, v3), -1, "partial overlap (left) rejected");
    vma_free(v3);

    // Partial overlap (right).
    struct Vma *v4 = vma_alloc(VA_BASE + VA_PAGE, VA_BASE + 3 * VA_PAGE,
                               VMA_PROT_RW, burrow, 0);
    TEST_ASSERT(v4 != NULL, "vma_alloc v4");
    TEST_EXPECT_EQ(vma_insert(p, v4), -1, "partial overlap (right) rejected");
    vma_free(v4);

    // Adjacent (touching at boundary) should NOT overlap — half-open ranges.
    struct Vma *v5 = vma_alloc(VA_BASE + 2 * VA_PAGE, VA_BASE + 3 * VA_PAGE,
                               VMA_PROT_RW, burrow, 0);
    TEST_ASSERT(v5 != NULL, "vma_alloc v5 (adjacent)");
    TEST_EXPECT_EQ(vma_insert(p, v5), 0, "adjacent (no overlap) accepted");

    vma_drain(p);
    p->state = 2;
    proc_free(p);
    burrow_unref(burrow);
}

void test_vma_insert_sorted_invariant(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct Burrow *burrow = burrow_create_anon(VA_2MIB);
    TEST_ASSERT(burrow != NULL, "burrow_create_anon failed");

    // Insert in mixed order: 4, 2, 6, 1, 3.
    u64 starts[] = { 4, 2, 6, 1, 3 };
    for (unsigned i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
        u64 s = VA_BASE + starts[i] * VA_PAGE;
        struct Vma *v = vma_alloc(s, s + VA_PAGE, VMA_PROT_RW, burrow, 0);
        TEST_ASSERT(v != NULL, "vma_alloc");
        TEST_EXPECT_EQ(vma_insert(p, v), 0, "insert");
    }

    // Walk the list; verify ascending order.
    u64 prev_start = 0;
    unsigned count = 0;
    for (struct Vma *cur = p->vmas; cur; cur = cur->next) {
        TEST_ASSERT(cur->vaddr_start > prev_start, "list not sorted ascending");
        prev_start = cur->vaddr_start;
        count++;
    }
    TEST_EXPECT_EQ(count, 5u, "5 VMAs in list");

    vma_drain(p);
    p->state = 2;
    proc_free(p);
    burrow_unref(burrow);
}

void test_vma_drain_releases_all(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct Burrow *burrow = burrow_create_anon(VA_2MIB);
    TEST_ASSERT(burrow != NULL, "burrow_create_anon failed");

    u64 alloc_before = vma_total_allocated();
    u64 free_before  = vma_total_freed();
    u64 mapping_before = burrow_mapping_count(burrow);

    enum { N = 4 };
    for (int i = 0; i < N; i++) {
        u64 s = VA_BASE + (u64)i * 2 * VA_PAGE;
        struct Vma *v = vma_alloc(s, s + VA_PAGE, VMA_PROT_RW, burrow, 0);
        TEST_ASSERT(v != NULL, "vma_alloc");
        TEST_EXPECT_EQ(vma_insert(p, v), 0, "insert");
    }

    TEST_EXPECT_EQ(burrow_mapping_count(burrow), mapping_before + N,
        "burrow mapping_count advanced by N");

    vma_drain(p);

    TEST_ASSERT(p->vmas == NULL, "vmas list emptied");
    TEST_EXPECT_EQ(vma_total_allocated() - alloc_before, (u64)N, "alloc counter");
    TEST_EXPECT_EQ(vma_total_freed()     - free_before,  (u64)N, "free counter");
    TEST_EXPECT_EQ(burrow_mapping_count(burrow), mapping_before,
        "burrow mapping_count returned to baseline");

    p->state = 2;
    proc_free(p);
    burrow_unref(burrow);
}
