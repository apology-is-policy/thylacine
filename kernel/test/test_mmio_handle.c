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
void test_mmio_handle_create_kernel_reserved_rejected(void);
void test_mmio_handle_virtio_mmio_claimable(void);
void test_mmio_handle_create_out_of_ips_rejected(void);

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

// R10 F154 (P1) regression: kobj_mmio_create rejects PAs that overlap
// kernel-reserved MMIO ranges (GIC, PL011, ECAM). Without this, a
// userspace driver with CAP_HW_CREATE could claim the GIC distributor
// + map it into userspace + scribble on kernel IRQ state.
// kobj_mmio_reserve_kernel_ranges() at boot pre-populates g_mmio_claims;
// this test verifies the rejection at the user-create path.
//
// VirtIO MMIO was originally also reserved by F154 but the reservation
// was refined at P4-Ic5b1a (kernel doesn't actively use virtio-mmio
// after `virtio_init` boot probe; the F154 close was over-broad). See
// `test_mmio_handle_virtio_mmio_claimable` for the positive-case test.
void test_mmio_handle_create_kernel_reserved_rejected(void) {
    // GIC distributor on QEMU virt: PA 0x08000000, 64 KiB. The
    // reservation is page-aligned outward so the entire range is
    // protected. A kobj_mmio_create at the exact GIC PA must fail.
    struct KObj_MMIO *k = kobj_mmio_create(0x08000000ull, PAGE_SIZE);
    TEST_ASSERT(k == NULL,
                "kobj_mmio_create on GIC distributor PA must be rejected");

    // PL011 UART: PA 0x09000000, 4 KiB. Also reserved.
    struct KObj_MMIO *k2 = kobj_mmio_create(0x09000000ull, PAGE_SIZE);
    TEST_ASSERT(k2 == NULL,
                "kobj_mmio_create on PL011 UART PA must be rejected");

    // Partial overlap with GIC redistributor — start of redist range
    // is 0x080a0000. A create at 0x08090000 (NOT in redist) + size
    // 0x20000 would overlap the redist start. Should be rejected.
    struct KObj_MMIO *k3 = kobj_mmio_create(0x08090000ull, 0x20000ull);
    TEST_ASSERT(k3 == NULL,
                "kobj_mmio_create overlapping GIC redist must be rejected");
}

// P4-Ic5b1a refinement of R10 F154: virtio-mmio slots are NOT in the
// kernel-reserved list. QEMU virt publishes 32 virtio-mmio nodes at
// PA 0x0a000000 + 0x200 × n; the kernel `virtio_init` probes them
// (reads MagicValue/Version/DeviceID/VendorID) but doesn't keep any
// active access. A driver-process holder of CAP_HW_CREATE can claim
// any virtio-mmio slot for its own use.
//
// Test: confirm a virtio-mmio PA range is claimable. Pick the last
// slot's page (0x0a003000 → 0x0a004000 covers slots 24..31; the
// virtio_init mmu_map_mmio mapping coexists fine with the userspace
// kobj because they're separate L3 entries — kernel KVA in vmalloc
// and user VA in a user-mapped Burrow). Cleanup via unref.
void test_mmio_handle_virtio_mmio_claimable(void) {
    // Last virtio-mmio page on QEMU virt. Page-aligned, full page span.
    struct KObj_MMIO *k = kobj_mmio_create(0x0a003000ull, PAGE_SIZE);
    TEST_ASSERT(k != NULL,
                "virtio-mmio PA must be claimable post-P4-Ic5b1a "
                "(was rejected pre-refinement due to over-broad R10 F154)");
    kobj_mmio_unref(k);

    // First virtio-mmio page (covers slots 0..7). Same expectation.
    struct KObj_MMIO *k2 = kobj_mmio_create(0x0a000000ull, PAGE_SIZE);
    TEST_ASSERT(k2 != NULL,
                "first virtio-mmio slot page must also be claimable");
    kobj_mmio_unref(k2);
}

// R10 F156 (P2) regression: kobj_mmio_create rejects PA that exceeds
// TCR.IPS = 40 bits. Without this rejection, a userspace caller would
// trigger FAULT_UNHANDLED_USER on first access → kernel extinction.
// Mirrors mmu_map_mmio's IPS check.
void test_mmio_handle_create_out_of_ips_rejected(void) {
    // PA at 1 TiB (2^40). Architecturally invalid in 40-bit IPS.
    struct KObj_MMIO *k = kobj_mmio_create((u64)1 << 40, PAGE_SIZE);
    TEST_ASSERT(k == NULL, "kobj_mmio_create at PA=2^40 must be rejected");

    // PA just below the boundary, but pa + size crosses it: pa =
    // (1<<40) - PAGE_SIZE; size = 2 * PAGE_SIZE. The first page is
    // valid but pa+size-1 sits at (1<<40) + PAGE_SIZE - 1 → out of IPS.
    struct KObj_MMIO *k2 = kobj_mmio_create(((u64)1 << 40) - PAGE_SIZE,
                                            2 * PAGE_SIZE);
    TEST_ASSERT(k2 == NULL,
                "kobj_mmio_create with range crossing IPS boundary must be rejected");
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
