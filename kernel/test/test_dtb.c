// dtb.c leaf-API tests — DTB parser surface coverage.
//
// We can't trivially construct synthetic DTB blobs in the kernel
// environment without a host-side build (no malloc, no host runtime).
// Instead we exercise the parser against the LIVE boot DTB that
// `phys_init`'s caller already validated, and verify the chosen-seed
// readers return non-zero — which they MUST, since KASLR's banner
// shows a successfully-derived offset.
//
// This is a regression check, not a black-box parser test. If the
// parser silently breaks (e.g., a future refactor mishandles the
// chosen-walk), this test fires immediately.

#include "test.h"

#include <thylacine/dtb.h>
#include <thylacine/types.h>

void test_dtb_chosen_kaslr_seed_present(void) {
    TEST_ASSERT(dtb_is_ready(),
        "DTB parser should be initialized post-phys_init");

    u64 kaslr_seed = dtb_get_chosen_kaslr_seed();
    u64 rng_seed   = dtb_get_chosen_rng_seed();

    // QEMU virt populates BOTH /chosen/kaslr-seed (newer QEMU) AND
    // /chosen/rng-seed (always). At least one of them must be
    // non-zero, otherwise our entropy chain fell back to cntpct.
    TEST_ASSERT(kaslr_seed != 0 || rng_seed != 0,
        "DTB /chosen must publish at least one seed");

    // Total size sanity: a real DTB is at least 200 bytes (header is
    // 40 bytes; the structure block + strings round it up). 4 GiB is
    // an obvious upper bound.
    u32 totalsize = dtb_get_total_size();
    TEST_ASSERT(totalsize >= 0xC8 && totalsize < 0xFFFFFFFFu,
        "DTB total_size should be reasonable");
}

// pci-1a: the PCIe host bridge's INTx -> GIC INTID routing, parsed from
// the live boot DTB's interrupt-map. Device-independent (the routing
// table exists regardless of which PCI devices are plugged).
void test_dtb_pci_intx_route(void) {
    TEST_ASSERT(dtb_is_ready(), "DTB must be initialized post-phys_init");

    // QEMU virt's gpex routes INTx to GIC SPIs 3..6 (= INTID 35..38) by the
    // standard swizzle  INTID = 35 + (((dev % 4) + pin - 1) % 4).  The
    // interrupt-map-mask keeps only 2 device bits, so dev is taken mod 4 —
    // dev 4..7 alias dev 0..3, which exercises the mask path.
    for (u32 dev = 0; dev < 8; dev++) {
        for (u32 pin = 1; pin <= 4; pin++) {
            u32 intid = 0;
            bool ok = dtb_pci_intx_route((u8)dev, (u8)pin, &intid);
            TEST_ASSERT(ok, "dtb_pci_intx_route should resolve a valid (dev,pin)");
            u32 expect = 35u + (((dev % 4u) + pin - 1u) % 4u);
            TEST_EXPECT_EQ((u64)intid, (u64)expect,
                           "PCI INTx swizzle -> GIC INTID mismatch");
        }
    }

    // Invalid pins (0, 5) match no interrupt-map row -> false, *out untouched.
    u32 intid = 0xDEAD;
    TEST_ASSERT(!dtb_pci_intx_route(0, 0, &intid),
                "INTx pin 0 should not resolve");
    TEST_ASSERT(!dtb_pci_intx_route(0, 5, &intid),
                "INTx pin 5 should not resolve");
    TEST_EXPECT_EQ((u64)intid, (u64)0xDEADu,
                   "failed route must not write *out_gic_intid");
    // NULL out -> false.
    TEST_ASSERT(!dtb_pci_intx_route(0, 1, NULL),
                "NULL out_gic_intid should return false");
}

// pci-1a: the PCIe 32-bit MMIO window (the `ranges` entry the kernel
// assigns BARs from). QEMU virt: base 0x10000000, ~768 MiB.
void test_dtb_pci_mem_window(void) {
    TEST_ASSERT(dtb_is_ready(), "DTB must be initialized post-phys_init");

    u64 base = 0, size = 0;
    bool ok = dtb_pci_mem_window(&base, &size);
    TEST_ASSERT(ok, "dtb_pci_mem_window should find the 32-bit MMIO window");
    TEST_EXPECT_EQ(base, (u64)0x10000000ull,
                   "PCI MMIO window base should be 0x10000000 on QEMU virt");
    TEST_ASSERT(size >= 0x100000ull,
                "PCI MMIO window should be at least 1 MiB");
    TEST_ASSERT(base + size > base, "PCI MMIO window must not overflow");
    // NULL args -> false.
    TEST_ASSERT(!dtb_pci_mem_window(NULL, &size), "NULL base -> false");
}
