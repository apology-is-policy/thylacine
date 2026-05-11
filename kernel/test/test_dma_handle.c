// P4-Ic5b1b: KObj_DMA lifecycle + Burrow integration + syscall-path tests.
//
// Per <thylacine/dma_handle.h> + specs/handles.tla. The KObj_DMA's
// HwResourceExclusive enforcement comes "for free" from the buddy
// allocator (each alloc_pages call returns a fresh chunk), so the tests
// here focus on:
//
//   1. Basic lifecycle: create returns a valid struct with refcount=1,
//      contiguous PA, page-aligned size.
//   2. Argument validation: zero / overflow / oversize rejected.
//   3. Refcount discipline: ref/unref balanced; last unref frees pages.
//   4. Burrow integration: burrow_create_dma takes a kobj_dma ref;
//      burrow_unref drops it; proc_free tears down VMA → Burrow → KObj.
//   5. Page-content correctness: KP_ZERO at create ⇒ the buffer reads
//      as all-zero from the kernel direct-map alias.

#include "test.h"

#include <thylacine/burrow.h>
#include <thylacine/dma_handle.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>

#include "../../arch/arm64/uart.h"

void test_dma_handle_create_basic(void);
void test_dma_handle_create_zero_size_rejected(void);
void test_dma_handle_create_oversize_rejected(void);
void test_dma_handle_create_round_up_to_page(void);
void test_dma_handle_distinct_pa(void);
void test_dma_handle_unref_releases_chunk(void);
void test_dma_handle_zero_init(void);
void test_burrow_dma_create_basic(void);
void test_burrow_dma_create_null_rejected(void);
void test_burrow_dma_holds_kobj_ref(void);
void test_burrow_dma_lifecycle_round_trip(void);
void test_dma_map_install_vma(void);
void test_dma_map_proc_free_releases_kobj(void);

// Test sizes — small (fits comfortably in any test universe).
#define TEST_DMA_SIZE_1PAGE  0x1000ull
#define TEST_DMA_SIZE_4PAGE  0x4000ull
#define USER_VA_DMA          0x50000000ull

extern struct Proc *proc_alloc(void);
extern void         proc_free(struct Proc *p);

// =============================================================================
// KObj_DMA layer.
// =============================================================================

void test_dma_handle_create_basic(void) {
    struct KObj_DMA *k = kobj_dma_create(TEST_DMA_SIZE_1PAGE);
    TEST_ASSERT(k != NULL, "kobj_dma_create returned NULL");
    TEST_EXPECT_EQ(k->size, (size_t)TEST_DMA_SIZE_1PAGE, "wrong size");
    TEST_EXPECT_EQ(k->ref, 1, "ref should start at 1");
    TEST_ASSERT(k->pages != NULL, "pages should be allocated");
    TEST_ASSERT((k->pa & (PAGE_SIZE - 1)) == 0, "pa should be page-aligned");
    TEST_EXPECT_EQ(k->pa, page_to_pa(k->pages), "pa should match page_to_pa(pages)");
    kobj_dma_unref(k);
}

void test_dma_handle_create_zero_size_rejected(void) {
    struct KObj_DMA *k = kobj_dma_create(0);
    TEST_ASSERT(k == NULL, "size=0 must reject");
}

void test_dma_handle_create_oversize_rejected(void) {
    // KOBJ_DMA_MAX_SIZE + 1 page must reject.
    struct KObj_DMA *k = kobj_dma_create(KOBJ_DMA_MAX_SIZE + PAGE_SIZE);
    TEST_ASSERT(k == NULL, "oversize must reject");
}

// Sub-page request gets rounded up to a full page.
void test_dma_handle_create_round_up_to_page(void) {
    struct KObj_DMA *k = kobj_dma_create(1);
    TEST_ASSERT(k != NULL, "kobj_dma_create(1) failed (page-up rounding)");
    TEST_EXPECT_EQ(k->size, (size_t)PAGE_SIZE, "size should round up to PAGE_SIZE");
    kobj_dma_unref(k);
}

// Two creates must yield distinct PAs (buddy allocator's per-alloc
// partitioning is the HwResourceExclusive enforcement for DMA).
void test_dma_handle_distinct_pa(void) {
    struct KObj_DMA *k1 = kobj_dma_create(TEST_DMA_SIZE_1PAGE);
    TEST_ASSERT(k1 != NULL, "k1 create failed");
    struct KObj_DMA *k2 = kobj_dma_create(TEST_DMA_SIZE_1PAGE);
    TEST_ASSERT(k2 != NULL, "k2 create failed");
    TEST_EXPECT_NE(k1->pa, k2->pa, "two creates must yield distinct PAs");
    kobj_dma_unref(k1);
    kobj_dma_unref(k2);
}

void test_dma_handle_unref_releases_chunk(void) {
    u64 live_before = kobj_dma_live_count();
    struct KObj_DMA *k = kobj_dma_create(TEST_DMA_SIZE_4PAGE);
    TEST_ASSERT(k != NULL, "create failed");
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before + 1, "live should bump");

    kobj_dma_unref(k);
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before, "live should drop on final unref");
}

// KP_ZERO at alloc means the buffer reads as all-zero immediately.
// Verify by reading through the kernel direct map (pa_to_kva).
void test_dma_handle_zero_init(void) {
    struct KObj_DMA *k = kobj_dma_create(TEST_DMA_SIZE_1PAGE);
    TEST_ASSERT(k != NULL, "create failed");

    volatile u8 *p = (volatile u8 *)pa_to_kva(k->pa);
    for (size_t i = 0; i < k->size; i++) {
        TEST_EXPECT_EQ(p[i], (u8)0, "DMA buffer not zero-initialized");
    }
    kobj_dma_unref(k);
}

// =============================================================================
// burrow_create_dma layer.
// =============================================================================

void test_burrow_dma_create_basic(void) {
    struct KObj_DMA *kd = kobj_dma_create(TEST_DMA_SIZE_1PAGE);
    TEST_ASSERT(kd != NULL, "kobj_dma_create failed");

    struct Burrow *b = burrow_create_dma(kd);
    TEST_ASSERT(b != NULL, "burrow_create_dma failed");
    TEST_EXPECT_EQ((int)b->type, (int)BURROW_TYPE_DMA, "wrong type");
    TEST_EXPECT_EQ(b->size, (size_t)TEST_DMA_SIZE_1PAGE, "wrong size");
    TEST_EXPECT_EQ(b->pa, kd->pa, "wrong pa");
    TEST_ASSERT(b->pages == NULL, "DMA Burrow should have pages=NULL (chunk on kobj)");
    TEST_EXPECT_EQ(b->handle_count, 1, "construction ref should be 1");
    TEST_EXPECT_EQ(b->mapping_count, 0, "mapping_count starts at 0");
    TEST_ASSERT(b->kobj_dma == kd, "kobj_dma field not set correctly");

    burrow_unref(b);    // frees b + drops Burrow's kobj_dma ref
    kobj_dma_unref(kd);  // drops caller's ref → KObj_DMA freed + pages released
}

void test_burrow_dma_create_null_rejected(void) {
    struct Burrow *b = burrow_create_dma(NULL);
    TEST_ASSERT(b == NULL, "burrow_create_dma(NULL) should return NULL");
}

// Burrow's ref keeps the underlying KObj_DMA alive past the caller's
// own unref. Verified via kobj_dma_live_count.
void test_burrow_dma_holds_kobj_ref(void) {
    u64 live_before = kobj_dma_live_count();

    struct KObj_DMA *kd = kobj_dma_create(TEST_DMA_SIZE_1PAGE);
    TEST_ASSERT(kd != NULL, "create failed");
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before + 1, "create should bump live");

    struct Burrow *b = burrow_create_dma(kd);
    TEST_ASSERT(b != NULL, "burrow_create_dma failed");
    // Burrow create doesn't make a new KObj_DMA.
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before + 1,
                   "burrow_create_dma should not bump live");

    // Drop caller's ref. Burrow's ref keeps it alive.
    kobj_dma_unref(kd);
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before + 1,
                   "kobj should stay alive (Burrow holds ref)");

    // Drop Burrow → last ref drops → kobj freed.
    burrow_unref(b);
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before,
                   "kobj should be freed after Burrow unref");
}

// Symmetric: Burrow first, kobj second.
void test_burrow_dma_lifecycle_round_trip(void) {
    u64 live_before = kobj_dma_live_count();

    struct KObj_DMA *kd = kobj_dma_create(TEST_DMA_SIZE_1PAGE);
    TEST_ASSERT(kd != NULL, "create failed");
    struct Burrow *b = burrow_create_dma(kd);
    TEST_ASSERT(b != NULL, "burrow create failed");

    burrow_unref(b);
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before + 1,
                   "kobj alive while caller holds ref");

    kobj_dma_unref(kd);
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before,
                   "kobj freed when both refs gone");
}

// =============================================================================
// burrow_map + VMA integration.
// =============================================================================

// burrow_create_dma + burrow_map installs a VMA reachable via vma_lookup.
// proc_free tears down: VMA → burrow_release_mapping → burrow_free_internal
// → kobj_dma_unref → free_pages.
void test_dma_map_install_vma(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct KObj_DMA *kd = kobj_dma_create(TEST_DMA_SIZE_4PAGE);
    TEST_ASSERT(kd != NULL, "kobj_dma_create failed");

    struct Burrow *b = burrow_create_dma(kd);
    TEST_ASSERT(b != NULL, "burrow_create_dma failed");

    int rc = burrow_map(p, b, USER_VA_DMA, TEST_DMA_SIZE_4PAGE, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, 0, "burrow_map failed");
    burrow_unref(b);    // transfer ref to VMA

    struct Vma *vma = vma_lookup(p, USER_VA_DMA);
    TEST_ASSERT(vma != NULL, "vma_lookup didn't find the new VMA");
    TEST_EXPECT_EQ(vma->vaddr_start, (u64)USER_VA_DMA, "wrong vaddr_start");
    TEST_EXPECT_EQ(vma->vaddr_end,   (u64)(USER_VA_DMA + TEST_DMA_SIZE_4PAGE),
                   "wrong vaddr_end");
    TEST_ASSERT(vma->burrow != NULL, "VMA's burrow is NULL");
    TEST_EXPECT_EQ((int)vma->burrow->type, (int)BURROW_TYPE_DMA,
                   "VMA's burrow has wrong type");
    TEST_EXPECT_EQ(vma->burrow->pa, kd->pa, "VMA's burrow has wrong pa");

    // Clean up.
    p->state = 2;     // PROC_STATE_ZOMBIE
    proc_free(p);
    kobj_dma_unref(kd);
}

// proc_free path correctly tears down the entire chain even when only
// the VMA holds the Burrow ref (caller already dropped kd). Verifies
// the cross-subsystem refcount handoff matches the MMIO pattern.
void test_dma_map_proc_free_releases_kobj(void) {
    u64 live_before = kobj_dma_live_count();

    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct KObj_DMA *kd = kobj_dma_create(TEST_DMA_SIZE_1PAGE);
    TEST_ASSERT(kd != NULL, "create failed");
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before + 1, "live +1");

    struct Burrow *b = burrow_create_dma(kd);
    TEST_ASSERT(b != NULL, "burrow create failed");
    int rc = burrow_map(p, b, USER_VA_DMA, TEST_DMA_SIZE_1PAGE, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, 0, "burrow_map failed");
    burrow_unref(b);   // transfer to VMA

    // Drop caller's kd ref BEFORE proc_free. VMA's Burrow still holds.
    kobj_dma_unref(kd);
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before + 1,
                   "kobj stays alive while VMA's Burrow holds ref");

    p->state = 2;     // PROC_STATE_ZOMBIE
    proc_free(p);
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before,
                   "kobj freed after proc_free walks VMAs + Burrows");
}
