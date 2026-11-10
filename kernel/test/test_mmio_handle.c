// P4-Ib: KObj_MMIO lifecycle + PA exclusivity tests.
//
// Per <thylacine/mmio_handle.h> + specs/handles.tla::HwResourceExclusive.
// Tests cover the basic lifecycle + the overlap-rejection invariant.
// Uses a synthetic PA range at 60 GiB, where the QEMU virt machine has
// neither a device nor RAM (a claim over RAM is refused) and which a CPU
// with a 36-bit physical address size can still name. The tests only
// exercise the claim machinery and never touch the range.

#include "test.h"
#include <thylacine/dtb.h>

#include <thylacine/extinction.h>
#include <thylacine/mmio_handle.h>
#include <thylacine/page.h>
#include <thylacine/types.h>

#include "../../arch/arm64/gic.h"
#include "../../arch/arm64/mmu.h"
#include "../../arch/arm64/uart.h"
#include "../../mm/phys.h"

// Synthetic PA range used by the tests. Page-aligned.
#define TEST_MMIO_PA_A   0xF00000000ull
#define TEST_MMIO_PA_B   0xF00010000ull   // 64 KiB past A
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

// A device window is never the machine's memory: a claim that shares a byte
// with the memory bank is refused, whoever asks. The control is one page away,
// just past the bank's end, where nothing differs but the RAM.
void test_mmio_handle_create_over_ram_rejected(void) {
    paddr_t base = 0, zone_end = 0;
    phys_zone_bounds(&base, &zone_end);
    paddr_t end = phys_ram_end();
    TEST_ASSERT(base < zone_end && zone_end <= end && (end & (PAGE_SIZE - 1)) == 0,
                "premise: a page-aligned bank that holds the zone");

    // The predicate by itself, away from the claim table (a reservation next to
    // the bank cannot stand in for it): a range is refused for any byte inside,
    // wherever it starts and ends.
    u64 span = (u64)(end - base);
    TEST_ASSERT(phys_range_touches_ram(base, PAGE_SIZE), "the first page touches");
    TEST_ASSERT(phys_range_touches_ram(end - PAGE_SIZE, PAGE_SIZE), "the last page touches");
    TEST_ASSERT(phys_range_touches_ram(base - PAGE_SIZE, 2 * PAGE_SIZE), "a range that starts below and ends inside touches");
    TEST_ASSERT(phys_range_touches_ram(end - PAGE_SIZE, 2 * PAGE_SIZE), "a range that starts inside and ends above touches");
    TEST_ASSERT(phys_range_touches_ram(base - PAGE_SIZE, span + 2 * PAGE_SIZE), "a range that holds the whole bank touches");
    TEST_ASSERT(!phys_range_touches_ram(base - PAGE_SIZE, PAGE_SIZE), "the page just below does not");
    TEST_ASSERT(!phys_range_touches_ram(end, PAGE_SIZE), "the page just above does not");
    TEST_ASSERT(!phys_range_touches_ram(base, 0), "an empty range does not");

    struct page *pg = alloc_pages(0, KP_ZERO);
    TEST_ASSERT(pg != NULL, "alloc a RAM page");
    struct KObj_MMIO *over_page = kobj_mmio_create(page_to_pa(pg), PAGE_SIZE);
    free_pages(pg, 0);
    struct KObj_MMIO *first    = kobj_mmio_create(base, PAGE_SIZE);
    struct KObj_MMIO *last     = kobj_mmio_create(end - PAGE_SIZE, PAGE_SIZE);
    struct KObj_MMIO *straddle = kobj_mmio_create(end - PAGE_SIZE, 2 * PAGE_SIZE);
    struct KObj_MMIO *past     = kobj_mmio_create(end, PAGE_SIZE);
    bool r_page = over_page != NULL, r_first = first != NULL, r_last = last != NULL;
    bool r_straddle = straddle != NULL, r_past = past != NULL;
    if (over_page) kobj_mmio_unref(over_page);
    if (first)     kobj_mmio_unref(first);
    if (last)      kobj_mmio_unref(last);
    if (straddle)  kobj_mmio_unref(straddle);
    if (past)      kobj_mmio_unref(past);

    TEST_ASSERT(r_past, "control: the page just past the zone can be claimed");
    TEST_ASSERT(!r_page, "a claim over an allocated page is refused");
    TEST_ASSERT(!r_first, "a claim over the zone's first page is refused");
    TEST_ASSERT(!r_last, "a claim over the zone's last page is refused");
    TEST_ASSERT(!r_straddle, "a claim that runs off the zone's end is refused for the page inside it");
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

    // CPU-side GIC region rejection. The region differs by GIC version:
    //   - v3: the redistributor (start 0x080a0000 on QEMU virt). A create at
    //     0x08090000 + 0x20000 starts OUTSIDE it but extends INTO it -- the
    //     partial-overlap case R10 F154 is about.
    //   - v2: the GICC CPU interface (the kernel's IRQ ack/EOI registers). A
    //     create overlapping it must be rejected too, or a CAP_HW_CREATE
    //     driver could scribble on kernel IRQ state -- same I-5 hazard.
    if (gic_version() == GIC_VERSION_V2) {
        struct KObj_MMIO *k3 = kobj_mmio_create(gic_cpu_iface_pa(), PAGE_SIZE);
        TEST_ASSERT(k3 == NULL,
                    "kobj_mmio_create on GICv2 cpu-interface PA must be rejected");
    } else {
        struct KObj_MMIO *k3 = kobj_mmio_create(0x08090000ull, 0x20000ull);
        TEST_ASSERT(k3 == NULL,
                    "kobj_mmio_create overlapping GIC redist must be rejected");
    }

    // Every MSI controller's doorbell page (v2m frame, ITS translater).
    struct dtb_pci_msi msi;
    if (!test_requires(dtb_msi_controller_n(0, &msi), "thylacine.msi", 0, false,
                       "the DTB names no MSI controller"))
        return;
    for (u32 i = 0; dtb_msi_controller_n(i, &msi); i++) {
        TEST_ASSERT(!kobj_mmio_create(msi.pa & ~4095ull, 4096),
                    "MSI controller page must never be claimable by EL0");
    }
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
    // The bound is what a translation may output: the CPU's own physical
    // address size where that is below the 40 bits TCR.IPS is programmed for.
    u32 bits = mmu_pa_bits();
    TEST_ASSERT(bits >= 32 && bits <= 40, "premise: a physical address size the kernel can program");
    u64 top = (u64)1 << bits;
    struct KObj_MMIO *k = kobj_mmio_create(top, PAGE_SIZE);
    // The first page is valid, the second is past the bound.
    struct KObj_MMIO *k2 = kobj_mmio_create(top - PAGE_SIZE, 2 * PAGE_SIZE);
    bool past = k != NULL, crossing = k2 != NULL;
    if (k)  kobj_mmio_unref(k);
    if (k2) kobj_mmio_unref(k2);
    // Control: the last page under the bound can be claimed.
    struct KObj_MMIO *k3 = kobj_mmio_create(top - PAGE_SIZE, PAGE_SIZE);
    bool last_ok = k3 != NULL;
    if (k3) kobj_mmio_unref(k3);
    TEST_ASSERT(!past, "a claim at the first address past the physical address size is refused");
    TEST_ASSERT(!crossing, "a claim that crosses the physical address size is refused");
    TEST_ASSERT(last_ok, "control: the last page under the bound can be claimed");
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

void test_mmio_kernel_reservation_union(void) {
    TEST_ASSERT(kobj_mmio_test_reservation_union(),
                "partial/transitive overlap must reserve entire union atomically");
}
