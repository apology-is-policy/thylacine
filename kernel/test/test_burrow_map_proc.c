// P3-Db: burrow_map(Proc*, ...) / burrow_unmap(Proc*, ...) tests.
//
// Five tests exercising the high-level address-space-installation API:
//
//   burrow.map_proc_smoke
//     burrow_map(p, v, vaddr, length, prot) installs a VMA visible via
//     vma_lookup. mapping_count tracks. vma_drain releases.
//
//   burrow.map_proc_constraints
//     Bad arguments return -1: NULL inputs, zero length, unaligned
//     vaddr, unaligned length, W+X prot. mapping_count unchanged after
//     each rejection.
//
//   burrow.map_proc_overlap_rejected
//     Two non-overlapping ranges accepted; overlap rejected. The
//     rejection MUST roll back the mapping_count++ that vma_alloc took
//     before vma_insert was called (verified via mapping_count delta).
//
//   burrow.unmap_proc_smoke
//     burrow_map followed by burrow_unmap removes the VMA + decrements
//     mapping_count.
//
//   burrow.unmap_proc_no_match
//     burrow_unmap with non-matching range returns -1 without disturbing
//     existing VMAs or mapping_count.

#include "test.h"

#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>
#include <thylacine/burrow.h>

void test_vmo_map_proc_smoke(void);
void test_vmo_map_proc_constraints(void);
void test_vmo_map_proc_overlap_rejected(void);
void test_vmo_unmap_proc_smoke(void);
void test_vmo_unmap_proc_no_match(void);

#define TEST_VA   0x10000000ull           // 256 MiB; well inside user-VA
#define ONE_PAGE  PAGE_SIZE
#define TWO_PAGES (2ull * PAGE_SIZE)

static struct Proc *make_proc(void) {
    struct Proc *p = proc_alloc();
    return p;
}

static void drop_proc(struct Proc *p) {
    if (!p) return;
    p->state = 2;             // PROC_STATE_ZOMBIE
    proc_free(p);
}

void test_vmo_map_proc_smoke(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    struct Burrow *v = burrow_create_anon(ONE_PAGE);
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");

    int mapping_before = burrow_mapping_count(v);

    int rc = burrow_map(p, v, TEST_VA, ONE_PAGE, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, 0, "burrow_map should succeed on a clean Proc");
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before + 1,
        "burrow_map should increment mapping_count via vma_alloc");

    // VMA visible via vma_lookup at the start, end-1, and middle.
    struct Vma *vma = vma_lookup(p, TEST_VA);
    TEST_ASSERT(vma != NULL, "vma_lookup at start returned NULL");
    TEST_EXPECT_EQ(vma->vaddr_start, TEST_VA,                "vaddr_start");
    TEST_EXPECT_EQ(vma->vaddr_end,   TEST_VA + ONE_PAGE,     "vaddr_end");
    TEST_EXPECT_EQ(vma->prot,        VMA_PROT_RW,            "prot");
    TEST_EXPECT_EQ(vma->burrow,         v,                      "burrow backref");

    // vma_drain handles the cleanup — also exercised by proc_free below.
    vma_drain(p);
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before,
        "vma_drain returns mapping_count to baseline");

    drop_proc(p);
    burrow_unref(v);
}

void test_vmo_map_proc_constraints(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    struct Burrow *v = burrow_create_anon(ONE_PAGE);
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");

    int mapping_before = burrow_mapping_count(v);

    // NULL Proc.
    TEST_EXPECT_EQ(burrow_map(NULL, v, TEST_VA, ONE_PAGE, VMA_PROT_RW), -1,
        "NULL Proc rejected");

    // NULL BURROW.
    TEST_EXPECT_EQ(burrow_map(p, NULL, TEST_VA, ONE_PAGE, VMA_PROT_RW), -1,
        "NULL BURROW rejected");

    // Zero length.
    TEST_EXPECT_EQ(burrow_map(p, v, TEST_VA, 0, VMA_PROT_RW), -1,
        "zero length rejected");

    // Unaligned vaddr.
    TEST_EXPECT_EQ(burrow_map(p, v, TEST_VA + 1, ONE_PAGE, VMA_PROT_RW), -1,
        "unaligned vaddr rejected");

    // Unaligned length.
    TEST_EXPECT_EQ(burrow_map(p, v, TEST_VA, ONE_PAGE + 1, VMA_PROT_RW), -1,
        "unaligned length rejected");

    // W+X prot.
    TEST_EXPECT_EQ(burrow_map(p, v, TEST_VA, ONE_PAGE,
                           VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC), -1,
        "W+X prot rejected");

    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before,
        "rejected map calls must NOT touch mapping_count");
    TEST_ASSERT(p->vmas == NULL, "rejected map calls must NOT install a VMA");

    drop_proc(p);
    burrow_unref(v);
}

void test_vmo_map_proc_overlap_rejected(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    struct Burrow *v = burrow_create_anon(TWO_PAGES);
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");

    int mapping_before = burrow_mapping_count(v);

    // First map: succeeds.
    TEST_EXPECT_EQ(burrow_map(p, v, TEST_VA, ONE_PAGE, VMA_PROT_RW), 0,
        "first burrow_map should succeed");
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before + 1, "mapping_count = +1");

    // Adjacent (touching at boundary) — half-open ranges, NOT overlap.
    TEST_EXPECT_EQ(burrow_map(p, v, TEST_VA + ONE_PAGE, ONE_PAGE, VMA_PROT_RW), 0,
        "adjacent VMA accepted");
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before + 2, "mapping_count = +2");

    // Exact overlap with first VMA — rejected, mapping_count unchanged.
    TEST_EXPECT_EQ(burrow_map(p, v, TEST_VA, ONE_PAGE, VMA_PROT_RW), -1,
        "exact overlap rejected");
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before + 2,
        "rollback after vma_insert overlap: mapping_count UNCHANGED");

    // Partial overlap — rejected, mapping_count unchanged.
    TEST_EXPECT_EQ(burrow_map(p, v, TEST_VA - ONE_PAGE, TWO_PAGES, VMA_PROT_RW), -1,
        "partial overlap rejected");
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before + 2,
        "rollback after partial overlap: mapping_count UNCHANGED");

    vma_drain(p);
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before,
        "drain restores mapping_count baseline");

    drop_proc(p);
    burrow_unref(v);
}

void test_vmo_unmap_proc_smoke(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    struct Burrow *v = burrow_create_anon(ONE_PAGE);
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");

    int mapping_before = burrow_mapping_count(v);

    TEST_EXPECT_EQ(burrow_map(p, v, TEST_VA, ONE_PAGE, VMA_PROT_RW), 0,
        "burrow_map");
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before + 1, "+1 after map");

    // Exact unmap.
    TEST_EXPECT_EQ(burrow_unmap(p, TEST_VA, ONE_PAGE), 0,
        "burrow_unmap exact match should succeed");
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before,
        "burrow_unmap returns mapping_count to baseline");
    TEST_ASSERT(vma_lookup(p, TEST_VA) == NULL,
        "VMA gone after burrow_unmap");

    drop_proc(p);
    burrow_unref(v);
}

void test_vmo_unmap_proc_no_match(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    struct Burrow *v = burrow_create_anon(ONE_PAGE);
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");

    int mapping_before = burrow_mapping_count(v);
    TEST_EXPECT_EQ(burrow_map(p, v, TEST_VA, ONE_PAGE, VMA_PROT_RW), 0, "map");

    // No VMA at this address.
    TEST_EXPECT_EQ(burrow_unmap(p, TEST_VA + ONE_PAGE, ONE_PAGE), -1,
        "unmap miss returns -1");

    // Wrong start within an existing VMA's range.
    TEST_EXPECT_EQ(burrow_unmap(p, TEST_VA + 1, ONE_PAGE), -1,
        "unmap unaligned in existing VMA returns -1");

    // Wrong length on an existing VMA.
    TEST_EXPECT_EQ(burrow_unmap(p, TEST_VA, TWO_PAGES), -1,
        "unmap with mismatched length returns -1");

    // mapping_count untouched throughout.
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before + 1,
        "failed unmap must not touch mapping_count");

    vma_drain(p);
    drop_proc(p);
    burrow_unref(v);
}
