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

// #166 / Warp-2 audit F3: the 64-bit MMIO window -- the arena a BAR too
// large for the ~752 MiB 32-bit window must come from (a `hostmem=N`
// virtio-gpu presents a multi-GiB one; without it the whole PCI claim
// aborts, which is what #166's userspace half could never fix alone).
//
// MEASURED PLATFORM FACT (both dumped with `-machine virt,dumpdtb`):
//   TCG  `-cpu cortex-a72` -> 3 ranges entries; the 0b11 window is
//                             0x80_0000_0000 + 512 GiB.
//   HVF  `-cpu host` (M2)  -> 2 entries ONLY (I/O + 32-bit MMIO). Apple
//                             Silicon's IPA limit makes QEMU omit the high
//                             window entirely.
// So presence is host-dependent and this test must not assert it. What it
// DOES assert unconditionally is the property a broken walker would break:
// a 0b11 query must never ALIAS the 32-bit entry. That is the bug worth
// catching -- a walker ignoring the space code returns true with the
// 32-bit window, which fails the `base >= 4 GiB` assert on TCG and the
// "must be absent, not aliased" assert on HVF.
void test_dtb_pci_mem_window64(void) {
    TEST_ASSERT(dtb_is_ready(), "DTB must be initialized post-phys_init");

    u64 b32 = 0, s32 = 0;
    TEST_ASSERT(dtb_pci_mem_window(&b32, &s32), "32-bit window must be found");
    TEST_ASSERT(s32 < 0x100000000ull,
                "the 32-bit window cannot hold a 4 GiB BAR (the #166 premise)");

    u64 base = 0, size = 0;
    if (dtb_pci_mem_window64(&base, &size)) {
        // Present (TCG): it must be a DISTINCT, high, large arena.
        TEST_ASSERT(base != b32, "the 64-bit window must not alias the 32-bit entry");
        TEST_ASSERT(base >= 0x100000000ull, "the 64-bit window lives above 4 GiB");
        TEST_ASSERT(size > s32, "the 64-bit window is the larger arena");
        TEST_ASSERT(base + size > base, "64-bit window must not overflow");
    } else {
        // Absent (HVF): the ONLY honest reading is "this host has no high
        // window", and a claim needing one fails honestly. Prove it was a
        // real absence rather than a parse failure -- the same property
        // read moments ago still works, and nothing was aliased into the
        // out-params.
        TEST_EXPECT_EQ(base, (u64)0, "absent window must not write out_base");
        TEST_EXPECT_EQ(size, (u64)0, "absent window must not write out_size");
        TEST_ASSERT(dtb_pci_mem_window(&b32, &s32),
                    "the ranges property is still parseable (absence is real)");
    }

    TEST_ASSERT(!dtb_pci_mem_window64(NULL, &size), "NULL base -> false");
}
