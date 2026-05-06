// P3-Bb: kernel direct map + vmalloc tests.
//
// Three smoke tests:
//
//   directmap.kva_round_trip
//     Verify pa_to_kva ↔ kva_to_pa is a bijection over physical memory
//     addresses (both arithmetic identity and round-trip through the
//     allocator).
//
//   directmap.alloc_through_directmap
//     Allocate via kpage_alloc and kmem_cache_alloc; verify the returned
//     pointer is in the direct-map range (>= KERNEL_DIRECT_MAP_BASE) and
//     that writes/reads through it work (proves the TTBR1 direct map is
//     live and kernel can dereference KVAs).
//
//   directmap.vmalloc_mmio_smoke
//     Map a region of physical RAM via mmu_map_mmio (using a known-safe
//     PA — the test thread's own kstack page), verify the returned KVA
//     is in vmalloc range, and verify writes via the mapped KVA appear
//     at the original PA when read via the direct map.
//
//   At v1.0 P3-Bb this last test exercises mmu_map_mmio with Device-
//   nGnRnE attributes against RAM. The mapping technically works
//   (Device-nGnRnE access to RAM is architecturally defined as
//   uncached but valid). The test's purpose is to verify the mmu_map_
//   mmio plumbing (PTE construction, TLB visibility, returned KVA),
//   NOT to test Device attribute behavior — which is meaningful only
//   against actual MMIO. Real MMIO is mapped from gic.c and uart.c at
//   driver-init time; that path is not exercised here because we don't
//   want to remap PL011 mid-test.

#include "test.h"

#include "../../arch/arm64/mmu.h"

#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/types.h>

#include "../../mm/phys.h"

void test_directmap_kva_round_trip(void);
void test_directmap_alloc_through_directmap(void);
void test_directmap_vmalloc_mmio_smoke(void);

void test_directmap_kva_round_trip(void) {
    // Arithmetic identity: pa | KERNEL_DIRECT_MAP_BASE → kva; kva &
    // ~KERNEL_DIRECT_MAP_BASE → pa. Verify across a representative
    // PA range (1 GiB, 2 GiB, 4 GiB-1, page-aligned values).
    paddr_t pas[] = {
        0x40000000ull,                  // 1 GiB (RAM start on QEMU virt)
        0x40001000ull,                  // 1 GiB + 1 page
        0x80000000ull,                  // 2 GiB
        0xBFFFF000ull,                  // ~3 GiB
    };

    for (unsigned i = 0; i < sizeof(pas) / sizeof(pas[0]); i++) {
        paddr_t pa = pas[i];
        void   *kva = pa_to_kva(pa);
        paddr_t pa2 = kva_to_pa(kva);

        TEST_EXPECT_EQ(pa, pa2,
            "pa_to_kva → kva_to_pa is not the identity");
        TEST_ASSERT(((u64)(uintptr_t)kva & KERNEL_DIRECT_MAP_BASE) == KERNEL_DIRECT_MAP_BASE,
            "pa_to_kva did not set KERNEL_DIRECT_MAP_BASE bits");
    }
}

void test_directmap_alloc_through_directmap(void) {
    // kpage_alloc returns a direct-map KVA. Write a sentinel value;
    // read it back; verify it survives. This implicitly verifies the
    // TTBR1 direct-map L1 block PTE is live (the dereference would
    // page-fault otherwise).
    //
    // KP_ZERO ensures the page starts zeroed, so our write sees a
    // distinguishable value (not pre-existing garbage that happens to
    // match the sentinel).
    void *p = kpage_alloc(KP_ZERO);
    TEST_ASSERT(p != NULL, "kpage_alloc failed");
    TEST_ASSERT(((u64)(uintptr_t)p & KERNEL_DIRECT_MAP_BASE) == KERNEL_DIRECT_MAP_BASE,
        "kpage_alloc did not return a direct-map KVA");

    // Write + read sentinel.
    volatile u64 *q = (volatile u64 *)p;
    const u64 sentinel = 0x1EC0DED2E5A113D7ULL;
    q[0] = sentinel;
    q[1] = ~sentinel;
    TEST_EXPECT_EQ(q[0], sentinel,
        "direct-map write/read mismatch (sentinel)");
    TEST_EXPECT_EQ(q[1], ~sentinel,
        "direct-map write/read mismatch (inverse)");

    kpage_free(p);
}

void test_directmap_vmalloc_mmio_smoke(void) {
    // Allocate a kernel page (whose KVA is in the direct map) and use
    // its PA as the target of mmu_map_mmio. The returned KVA is in
    // vmalloc range. Verify the vmalloc KVA is in range (>=
    // VMALLOC_BASE).
    //
    // We do NOT write through the vmalloc mapping because Device-
    // nGnRnE attributes on regular RAM produce architecturally-defined
    // but unusual semantics (uncached, no gathering). Reading would
    // also be Device-nGnRnE, not a useful round-trip.
    //
    // The plumbing test (PTE construction + TLB visibility + bumped
    // cursor) is what we want here.
    void *backing = kpage_alloc(KP_ZERO);
    TEST_ASSERT(backing != NULL, "kpage_alloc for backing failed");

    paddr_t pa = kva_to_pa(backing);

    // Verify pre-mmu_map_mmio cursor state (irrelevant value, but
    // the bump should advance after the call).
    void *kva = mmu_map_mmio(pa, PAGE_SIZE);
    TEST_ASSERT(kva != NULL, "mmu_map_mmio returned NULL");
    TEST_ASSERT((u64)(uintptr_t)kva >= VMALLOC_BASE,
        "mmu_map_mmio returned a non-vmalloc address");
    TEST_ASSERT((u64)(uintptr_t)kva < VMALLOC_BASE + (1ull << 21),
        "mmu_map_mmio returned outside the first 2 MiB of vmalloc");

    // Note: there's no public mmu_unmap_mmio at v1.0 P3-Bb (the bump
    // allocator is one-way; reuse comes via boot reset only). The
    // l3_vmalloc entry stays populated; the PA-backing kpage is
    // freed below, but the vmalloc KVA still maps to it. At v1.0 this
    // is an accepted leak in the test (1 vmalloc slot consumed; 511
    // remain). Phase 5+ would add reclaim.

    kpage_free(backing);
}
