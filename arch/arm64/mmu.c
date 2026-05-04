// ARM64 MMU — page table setup + enable.
//
// Identity-map the low 4 GiB so the kernel's existing physical addresses
// remain valid after MMU enable. Per-section permissions for the kernel
// image (W^X invariant I-12). Device-nGnRnE for the PL011 region.
//
// Memory layout for 48-bit VA / 4 KiB granule / 4-level page tables:
//
//   VA[47:39]  L0 index (each entry covers 512 GiB)
//   VA[38:30]  L1 index (each entry covers 1 GiB)
//   VA[29:21]  L2 index (each entry covers 2 MiB; 2 MiB blocks at this level)
//   VA[20:12]  L3 index (each entry covers 4 KiB; 4 KiB pages at this level)
//   VA[11:0]   page offset
//
// At P1-C we map exactly:
//
//   - Low 4 GiB via L0[0] -> L1 -> L2 (4 entries, one per GiB).
//   - L2 entries default to 2 MiB block mappings of Normal-WB RW memory.
//   - The 2 MiB region containing the kernel image (0x40000000 + 0x80000
//     = 0x40080000 lives in the second 2 MiB of L2[1.0], i.e. the L2
//     entry at index 1 of the second L2 table) is replaced with an L3
//     table so we can attribute .text / .rodata / .data / .bss / boot
//     stack each at 4 KiB granularity.
//   - The 2 MiB region containing PL011 (0x09000000) is replaced with a
//     Device block.
//
// Per ARCHITECTURE.md §6 + §24.

#include "mmu.h"

#include <stdint.h>
#include <thylacine/types.h>

// ---------------------------------------------------------------------------
// Linker-provided symbols (kernel.ld).
// ---------------------------------------------------------------------------

extern char _kernel_start[];
extern char _kernel_end[];
extern char _bss_start[];

// We don't have explicit per-section start/end symbols for .text /
// .rodata / .data yet, but we can compute the boundaries based on the
// kernel.ld layout: each section is page-aligned, and they appear in
// order .text, .rodata, .data, .bss. We refine kernel.ld below to expose
// these symbols once mmu.c needs them.
extern char _text_end[];      // end of .text (== start of .rodata)
extern char _rodata_end[];    // end of .rodata (== start of .data)
extern char _data_end[];      // end of .data (== start of .bss)
// _bss_start, _bss_end are already exported by kernel.ld.

// ---------------------------------------------------------------------------
// Page table memory.
//
// All BSS-allocated and 4 KiB-aligned. Cleared by start.S's BSS clear
// before mmu_enable() runs.
//
// Sizing for "low 4 GiB identity map":
//   - 1 L0 table (only entry 0 used, but the whole 4 KiB still allocated).
//   - 1 L1 table (entries 0-3 used; one per GiB).
//   - 4 L2 tables (one per GiB; each has 512 entries × 2 MiB = 1 GiB).
//   - 1 L3 table for the kernel-image 2 MiB region (fine-grained perms).
//
// Memory cost: 7 × 4 KiB = 28 KiB. Negligible.
// ---------------------------------------------------------------------------

#define ENTRIES_PER_TABLE 512
#define PAGE_SHIFT        12
#define PAGE_SIZE         (1ull << PAGE_SHIFT)
#define BLOCK_SHIFT_L2    21
#define BLOCK_SIZE_L2     (1ull << BLOCK_SHIFT_L2)   // 2 MiB
#define BLOCK_SHIFT_L1    30
#define BLOCK_SIZE_L1     (1ull << BLOCK_SHIFT_L1)   // 1 GiB

static u64 l0_table[ENTRIES_PER_TABLE]   __attribute__((aligned(PAGE_SIZE)));
static u64 l1_table[ENTRIES_PER_TABLE]   __attribute__((aligned(PAGE_SIZE)));
static u64 l2_table[4][ENTRIES_PER_TABLE] __attribute__((aligned(PAGE_SIZE)));
static u64 l3_kernel[ENTRIES_PER_TABLE]  __attribute__((aligned(PAGE_SIZE)));

// ---------------------------------------------------------------------------
// PTE constructors and inspectors.
// ---------------------------------------------------------------------------

// Build a table descriptor pointing at a next-level table (L0/L1/L2 -> next).
static inline u64 make_table_pte(const void *table) {
    return ((u64)(uintptr_t)table) | PTE_VALID | PTE_TYPE_TABLE;
}

// Build a 2 MiB block descriptor at L2 with the given attribute mask.
static inline u64 make_block_pte_l2(paddr_t pa, u64 attrs) {
    return (pa & ~(BLOCK_SIZE_L2 - 1)) | attrs;
}

// Build a 4 KiB page descriptor at L3 with the given attribute mask.
static inline u64 make_page_pte_l3(paddr_t pa, u64 attrs) {
    return (pa & ~(PAGE_SIZE - 1)) | attrs;
}

bool pte_violates_wxe(u64 pte) {
    if (!(pte & PTE_VALID)) return false;
    bool writable = !(pte & PTE_AP_RO_EL1);          // AP[2:1] != RO_EL1 / RO_ANY
    // (PTE_AP_RO_EL1 sets bit 7; PTE_AP_RO_ANY sets bits 7+6. Bit 7 is the
    // "RO" indicator — if it's clear, AP[2:1] is 0b00 (RW EL1) or 0b01
    // (RW any), which means writable.)
    bool exec_el1 = !(pte & PTE_PXN);
    return writable && exec_el1;
}

// ---------------------------------------------------------------------------
// Static assertions on the PTE bit constants.
//
// W^X invariant I-12 is enforced at construction time by the PTE_KERN_*
// helpers. The asserts below would catch a regression where someone
// changes the helpers and accidentally allows W+X.
// ---------------------------------------------------------------------------

_Static_assert((PTE_KERN_TEXT & PTE_AP_RO_EL1) != 0,
               "PTE_KERN_TEXT must be RO at EL1");
_Static_assert((PTE_KERN_TEXT & PTE_PXN) == 0,
               "PTE_KERN_TEXT must allow EL1 execution");
_Static_assert((PTE_KERN_RW & PTE_AP_RO_EL1) == 0,
               "PTE_KERN_RW must be RW at EL1");
_Static_assert((PTE_KERN_RW & PTE_PXN) != 0,
               "PTE_KERN_RW must NOT allow EL1 execution");
_Static_assert((PTE_KERN_RO & PTE_AP_RO_EL1) != 0,
               "PTE_KERN_RO must be RO at EL1");
_Static_assert((PTE_KERN_RO & PTE_PXN) != 0,
               "PTE_KERN_RO must NOT allow EL1 execution");
_Static_assert((PTE_DEVICE_RW_BLOCK & PTE_PXN) != 0,
               "Device PTEs must NOT allow EL1 execution");

// ---------------------------------------------------------------------------
// Build the identity-mapped page tables.
//
// L0[0] -> l1_table.
// l1_table[0..3] -> l2_table[0..3], each covering 1 GiB.
// l2_table[i] entries default to 2 MiB Normal-WB RW blocks.
// Override: replace the 2 MiB entry that contains 0x09000000 (PL011)
//           with a Device-nGnRnE block.
// Override: replace the 2 MiB entry that contains the kernel image with
//           a table descriptor pointing at l3_kernel[]; populate
//           l3_kernel with per-section page descriptors.
// ---------------------------------------------------------------------------

#define PL011_BASE_FALLBACK 0x09000000ull
#define KERNEL_LOAD_ADDR    0x40080000ull

static void build_identity_map(void) {
    // L0[0] -> L1.
    l0_table[0] = make_table_pte(l1_table);

    // L1[0..3] -> L2[0..3] (4 GiB total).
    for (int i = 0; i < 4; i++) {
        l1_table[i] = make_table_pte(l2_table[i]);
    }

    // L2 entries: default 2 MiB Normal-WB RW blocks across the full 4 GiB.
    for (int gib = 0; gib < 4; gib++) {
        for (int j = 0; j < ENTRIES_PER_TABLE; j++) {
            paddr_t pa = ((paddr_t)gib << BLOCK_SHIFT_L1) +
                         ((paddr_t)j << BLOCK_SHIFT_L2);
            l2_table[gib][j] = make_block_pte_l2(pa, PTE_KERN_RW_BLOCK);
        }
    }

    // Override: PL011 region (covers 0x09000000) -> Device block.
    {
        u32 gib = (u32)(PL011_BASE_FALLBACK >> BLOCK_SHIFT_L1);
        u32 idx = (u32)((PL011_BASE_FALLBACK >> BLOCK_SHIFT_L2) & 0x1ff);
        paddr_t pa = (PL011_BASE_FALLBACK & ~(BLOCK_SIZE_L2 - 1));
        l2_table[gib][idx] = make_block_pte_l2(pa, PTE_DEVICE_RW_BLOCK);
    }

    // Override: kernel-image 2 MiB region -> L3 table for fine-grained
    // permissions. The kernel currently spans roughly 0x40080000..0x40088000
    // (~28 KiB), all within the 2 MiB region 0x40000000..0x40200000 (the
    // SECOND 2 MiB region, since the FIRST is 0x40000000..0x40200000 — wait,
    // let me re-check). KERNEL_LOAD_ADDR = 0x40080000.
    //   gib = 1 (because 0x40000000 / 1 GiB = 1).
    //   idx = (0x40080000 >> 21) & 0x1ff = 0  (because 0x40080000 / 2 MiB
    //         = 0x80000000 / 0x200000 = 0x400 within the 1 GiB region; mod
    //         512 = 0).
    // Wait. 0x40080000 / 0x200000 = 0x200 (= 512). Hmm.
    //
    // Let me recompute. 0x40080000 in binary: 0100_0000_0000_1000_0000_...
    //   bits[63:30] = 0x1 (gib index = 1)
    //   bits[29:21] = 0     (l2 index within that gib = 0)
    //   bits[20:0]  = 0x80000 (offset within the 2 MiB region)
    //
    // So the kernel lives in l2_table[1][0], which covers
    // 0x40000000..0x40200000. We override that to point at l3_kernel.
    {
        u64 kernel_2mib_base = KERNEL_LOAD_ADDR & ~(BLOCK_SIZE_L2 - 1);
        u32 gib = (u32)(kernel_2mib_base >> BLOCK_SHIFT_L1);
        u32 idx = (u32)((kernel_2mib_base >> BLOCK_SHIFT_L2) & 0x1ff);

        // Fill l3_kernel: every page in the 2 MiB region gets a default
        // Normal-WB RW page mapping. We then override the kernel-image
        // pages with per-section attributes.
        for (int j = 0; j < ENTRIES_PER_TABLE; j++) {
            paddr_t pa = kernel_2mib_base + ((paddr_t)j << PAGE_SHIFT);
            l3_kernel[j] = make_page_pte_l3(pa, PTE_KERN_RW);
        }

        // Per-section overrides. Each section is page-aligned per kernel.ld.
        // Indices into l3_kernel[] = (page_pa - kernel_2mib_base) / PAGE_SIZE.
        u64 ks   = (u64)(uintptr_t)_kernel_start;
        u64 te   = (u64)(uintptr_t)_text_end;
        u64 re   = (u64)(uintptr_t)_rodata_end;
        u64 de   = (u64)(uintptr_t)_data_end;
        u64 bs   = (u64)(uintptr_t)_bss_start;
        u64 ke   = (u64)(uintptr_t)_kernel_end;

        // .text — RX
        for (u64 p = ks; p < te; p += PAGE_SIZE) {
            u32 i = (u32)((p - kernel_2mib_base) >> PAGE_SHIFT);
            l3_kernel[i] = make_page_pte_l3(p, PTE_KERN_TEXT);
        }
        // .rodata — R
        for (u64 p = te; p < re; p += PAGE_SIZE) {
            u32 i = (u32)((p - kernel_2mib_base) >> PAGE_SHIFT);
            l3_kernel[i] = make_page_pte_l3(p, PTE_KERN_RO);
        }
        // .data + .bss — RW (default; explicit assignment for clarity)
        for (u64 p = de; p < ke; p += PAGE_SIZE) {
            u32 i = (u32)((p - kernel_2mib_base) >> PAGE_SHIFT);
            l3_kernel[i] = make_page_pte_l3(p, PTE_KERN_RW);
        }
        // (.data is between de and bs but currently empty; the loop
        // above covers de..ke which includes both .data and .bss.)
        (void)bs;       // keep the symbol referenced for future use

        l2_table[gib][idx] = make_table_pte(l3_kernel);
    }
}

// ---------------------------------------------------------------------------
// MMU enable. Sets MAIR / TCR / TTBR / SCTLR in the canonical order:
//
//   1. MAIR_EL1 (memory attributes). Must be set before any cacheable
//      mapping is enabled.
//   2. TCR_EL1 (translation control). 48-bit VA, 4 KiB granule, IPS=4
//      (40-bit PA — covers our 4 GiB identity), inner+outer WB cacheable
//      table walks.
//   3. TTBR0_EL1 / TTBR1_EL1. We use TTBR0 for the identity map; TTBR1
//      is empty at P1-C (KASLR populates it at P1-C-extras).
//   4. ISB (instruction sync barrier).
//   5. SCTLR_EL1 |= M (MMU on) | C (data cache) | I (instruction cache).
//   6. ISB.
//
// Once SCTLR.M=1, all kernel addresses are translated via the TTBR0 map.
// Since our map is identity, code/data accesses see the same physical
// addresses they did pre-MMU — execution continues seamlessly.
// ---------------------------------------------------------------------------

#define TCR_T0SZ_48BIT   (16ull << 0)         // VA size = 64 - 16 = 48
#define TCR_TG0_4K       (0ull  << 14)
#define TCR_SH0_INNER    (3ull  << 12)
#define TCR_ORGN0_WB     (1ull  << 10)
#define TCR_IRGN0_WB     (1ull  << 8)
#define TCR_T1SZ_48BIT   (16ull << 16)        // (TTBR1 unused at P1-C)
#define TCR_TG1_4K       (2ull  << 30)        // 4 KiB granule (yes, 2 = 4K for TG1)
#define TCR_SH1_INNER    (3ull  << 28)
#define TCR_ORGN1_WB     (1ull  << 26)
#define TCR_IRGN1_WB     (1ull  << 24)
#define TCR_IPS_36BIT    (1ull  << 32)        // 64 GiB physical max (sufficient)
#define TCR_IPS_40BIT    (2ull  << 32)        // 1 TiB physical max
#define TCR_IPS_VALUE    TCR_IPS_40BIT

#define TCR_VALUE \
    (TCR_T0SZ_48BIT | TCR_TG0_4K | TCR_SH0_INNER | TCR_ORGN0_WB | TCR_IRGN0_WB | \
     TCR_T1SZ_48BIT | TCR_TG1_4K | TCR_SH1_INNER | TCR_ORGN1_WB | TCR_IRGN1_WB | \
     TCR_IPS_VALUE)

#define SCTLR_M  (1ull << 0)
#define SCTLR_C  (1ull << 2)
#define SCTLR_I  (1ull << 12)

void mmu_enable(void) {
    build_identity_map();

    __asm__ __volatile__(
        "msr mair_el1, %0\n"
        "msr tcr_el1, %1\n"
        "msr ttbr0_el1, %2\n"
        "msr ttbr1_el1, xzr\n"               // empty at P1-C
        "isb\n"
        "mrs x9, sctlr_el1\n"
        "orr x9, x9, %3\n"
        "msr sctlr_el1, x9\n"
        "isb\n"
        :: "r" ((u64)MAIR_VALUE),
           "r" ((u64)TCR_VALUE),
           "r" ((u64)(uintptr_t)l0_table),
           "r" ((u64)(SCTLR_M | SCTLR_C | SCTLR_I))
        : "x9", "memory"
    );

    // After this point, all loads/stores go through the page tables.
    // The boot stack, kernel image, and PL011 are all reachable at
    // their physical addresses (identity-mapped).
}
