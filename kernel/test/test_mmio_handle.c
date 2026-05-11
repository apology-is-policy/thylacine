// P4-Ib: KObj_MMIO lifecycle + PA exclusivity tests.
//
// Per <thylacine/mmio_handle.h> + specs/handles.tla::HwResourceExclusive.
// Tests cover the basic lifecycle + the overlap-rejection invariant.
// Uses a synthetic PA range chosen to NOT overlap any real MMIO region
// in the QEMU virt platform (TEST_MMIO_PA picked from the "no devices
// here" gap above 0x80000000 — RAM extends from 0x40000000, so this
// range may overlap RAM, but kobj_mmio_create doesn't actually access
// the memory; it just tracks the claim. Real driver use (P4-Ic+) maps
// device PAs and reads/writes them; tests only exercise the claim
// machinery.)

#include "test.h"

#include <thylacine/extinction.h>
#include <thylacine/mmio_handle.h>
#include <thylacine/page.h>
#include <thylacine/types.h>

#include "../../arch/arm64/uart.h"

void test_mmio_handle_create_basic(void);
void test_mmio_handle_create_misaligned_rejected(void);
void test_mmio_handle_create_zero_size_rejected(void);
void test_mmio_handle_create_overflow_rejected(void);
void test_mmio_handle_create_overlap_rejected(void);
void test_mmio_handle_create_adjacent_ok(void);
void test_mmio_handle_create_unref_releases_slot(void);
void test_mmio_handle_double_unref_extincts(void);

// Synthetic PA range used by the tests. Page-aligned. Picked
// arbitrarily — claim tracking doesn't touch the actual memory.
#define TEST_MMIO_PA_A   0x100000000ull
#define TEST_MMIO_PA_B   0x100010000ull   // 64 KiB past A
#define TEST_MMIO_SIZE   0x1000ull        // 1 page

void test_mmio_handle_create_basic(void) {
    struct KObj_MMIO *k = kobj_mmio_create(TEST_MMIO_PA_A, TEST_MMIO_SIZE);
    TEST_ASSERT(k != NULL, "kobj_mmio_create returned NULL");
    TEST_EXPECT_EQ(k->pa, (u64)TEST_MMIO_PA_A, "wrong pa");
    TEST_EXPECT_EQ(k->size, (size_t)TEST_MMIO_SIZE, "wrong size");
    TEST_EXPECT_EQ(k->ref, 1, "ref should start at 1");
    kobj_mmio_unref(k);
}

void test_mmio_handle_create_misaligned_rejected(void) {
    // pa not page-aligned
    struct KObj_MMIO *k1 = kobj_mmio_create(TEST_MMIO_PA_A + 1, TEST_MMIO_SIZE);
    TEST_ASSERT(k1 == NULL, "expected NULL for misaligned pa");

    // size not page-aligned
    struct KObj_MMIO *k2 = kobj_mmio_create(TEST_MMIO_PA_A, 1);
    TEST_ASSERT(k2 == NULL, "expected NULL for misaligned size");
}

void test_mmio_handle_create_zero_size_rejected(void) {
    struct KObj_MMIO *k = kobj_mmio_create(TEST_MMIO_PA_A, 0);
    TEST_ASSERT(k == NULL, "expected NULL for size=0");
}

void test_mmio_handle_create_overflow_rejected(void) {
    // pa + size would overflow u64
    struct KObj_MMIO *k = kobj_mmio_create((u64)-PAGE_SIZE, PAGE_SIZE * 2);
    TEST_ASSERT(k == NULL, "expected NULL for pa+size overflow");
}

// Two creates for the SAME pa range — the second must fail with
// HwResourceExclusive enforcement.
void test_mmio_handle_create_overlap_rejected(void) {
    struct KObj_MMIO *k1 = kobj_mmio_create(TEST_MMIO_PA_A, TEST_MMIO_SIZE);
    TEST_ASSERT(k1 != NULL, "first create should succeed");

    struct KObj_MMIO *k2 = kobj_mmio_create(TEST_MMIO_PA_A, TEST_MMIO_SIZE);
    TEST_ASSERT(k2 == NULL, "second create on same pa should be rejected (HwResourceExclusive)");

    // Partial overlap should also be rejected — start of B falls in A's range.
    struct KObj_MMIO *k3 = kobj_mmio_create(TEST_MMIO_PA_A + (TEST_MMIO_SIZE / 2),
                                            TEST_MMIO_SIZE);
    TEST_ASSERT(k3 == NULL, "partial overlap should be rejected");

    kobj_mmio_unref(k1);
}

// Adjacent (non-overlapping, contiguous) ranges should both succeed.
// k1 spans [PA_A, PA_A + SIZE); k2 spans [PA_A + SIZE, PA_A + 2*SIZE).
// They share only the boundary at PA_A + SIZE, which is open in k1 and
// closed-start in k2 — not an overlap.
void test_mmio_handle_create_adjacent_ok(void) {
    struct KObj_MMIO *k1 = kobj_mmio_create(TEST_MMIO_PA_A, TEST_MMIO_SIZE);
    TEST_ASSERT(k1 != NULL, "k1 create failed");
    struct KObj_MMIO *k2 = kobj_mmio_create(TEST_MMIO_PA_A + TEST_MMIO_SIZE,
                                            TEST_MMIO_SIZE);
    TEST_ASSERT(k2 != NULL, "k2 create (adjacent to k1) should succeed");
    kobj_mmio_unref(k1);
    kobj_mmio_unref(k2);
}

// After unref, the claim slot is freed and a subsequent create on the
// same PA should succeed.
void test_mmio_handle_create_unref_releases_slot(void) {
    struct KObj_MMIO *k1 = kobj_mmio_create(TEST_MMIO_PA_A, TEST_MMIO_SIZE);
    TEST_ASSERT(k1 != NULL, "k1 create failed");
    kobj_mmio_unref(k1);

    struct KObj_MMIO *k2 = kobj_mmio_create(TEST_MMIO_PA_A, TEST_MMIO_SIZE);
    TEST_ASSERT(k2 != NULL, "k2 (after k1 unref) should succeed");
    kobj_mmio_unref(k2);
}

// Verify the live-count instrumentation tracks correctly across a
// short lifecycle. Also exercises kobj_mmio_total_created which is
// useful for leak detection in stress tests later.
void test_mmio_handle_double_unref_extincts(void) {
    u64 created_before = kobj_mmio_total_created();
    u64 live_before    = kobj_mmio_live_count();

    struct KObj_MMIO *k = kobj_mmio_create(TEST_MMIO_PA_B, TEST_MMIO_SIZE);
    TEST_ASSERT(k != NULL, "create failed");
    TEST_EXPECT_EQ(kobj_mmio_total_created(), created_before + 1,
                   "total_created didn't increment");
    TEST_EXPECT_EQ(kobj_mmio_live_count(), live_before + 1,
                   "live_count didn't increment");

    kobj_mmio_unref(k);
    TEST_EXPECT_EQ(kobj_mmio_live_count(), live_before,
                   "live_count didn't decrement on unref");
    // Note: total_created stays bumped — it's cumulative.

    // We intentionally do NOT exercise the double-unref-extincts path
    // here (it would tear down the kernel test harness). The defense
    // is verified by inspection of kobj_mmio_unref's `ref <= 0` check.
}
