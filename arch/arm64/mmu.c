// ARM64 MMU — page table setup + enable.
//
// At P1-C-extras Part B, the kernel is linked at high VA
// KASLR_LINK_VA (0xFFFFA00000080000) and randomized at boot by a
// page-block-aligned offset. mmu.c builds TWO mappings sharing one
// L3 page-grain table:
//
//   - TTBR0 identity for the low 4 GiB. Required for early boot
//     (PC = load PA pre-branch), DTB access (DTB at PA 0x48000000),
//     and MMIO (PL011 et al). Will retire to user-space once Phase 2
//     introduces process address spaces.
//
//   - TTBR1 kernel high-half at KASLR_LINK_VA + slide. The boot stub
//     long-branches into this VA after mmu_enable() returns. From
//     then on, kernel code runs through TTBR1.
//
// Per-section permissions (W^X invariant I-12) live in the shared L3
// table — same memory under either translation. The boot-stack guard
// page is non-present in this L3, so a stack overflow faults via
// either translation root.
//
// Memory layout for 48-bit VA / 4 KiB granule / 4-level page tables:
//
//   VA[47:39]  L0 index (each entry covers 512 GiB)
//   VA[38:30]  L1 index (each entry covers 1 GiB)
//   VA[29:21]  L2 index (each entry covers 2 MiB; 2 MiB blocks at this level)
//   VA[20:12]  L3 index (each entry covers 4 KiB; 4 KiB pages at this level)
//   VA[11:0]   page offset
//
// TTBR0 layout (low 4 GiB identity):
//   - L0[0] -> l0_ttbr0_l1.
//   - l0_ttbr0_l1[0..3] -> l2_ttbr0[0..3], one L2 per GiB.
//   - L2 entries default to 2 MiB Normal-WB RW blocks.
//   - L2 entry covering PL011 (0x09000000) -> Device-nGnRnE block.
//   - L2 entry covering kernel image -> table descriptor pointing at
//     l3_kernel (page-grain perms).
//
// TTBR1 layout (kernel high-half):
//   - L0[KASLR_L0_IDX] -> l1_ttbr1.
//   - L1[L1_idx_for(slide)] -> l2_ttbr1.
//   - L2[L2_idx_for(slide)] -> l3_kernel (the same L3 used by TTBR0).
//
// Per ARCHITECTURE.md §5.3 + §6 + §24.

#include "mmu.h"
#include "kaslr.h"

#include <stddef.h>           // size_t (P3-Bb mmu_map_mmio)
#include <stdint.h>
#include <thylacine/extinction.h>   // extinction (P3-Bb mmu_map_mmio guards)
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

// Boot-stack guard page. Kernel.ld places this 4 KiB slot immediately
// below the boot stack. mmu.c zeroes its L3 PTE in build_page_tables()
// so a stack overflow faults synchronously rather than silently
// corrupting prior BSS.
extern char _boot_stack_guard[];

// ---------------------------------------------------------------------------
// Page table memory.
//
// All BSS-allocated and 4 KiB-aligned. Cleared by start.S's BSS clear
// before mmu_enable() runs.
//
// Sizing:
//   TTBR0 (low 4 GiB identity):
//     - 1 L0 table (only entry 0 used).
//     - 1 L1 table (entries 0-3 used; one per GiB).
//     - 4 L2 tables (one per GiB; 1 GiB each via 512 × 2 MiB blocks).
//   TTBR1 (kernel high-half):
//     - 1 L0 table (only one entry used — KASLR_L0_IDX).
//     - 1 L1 table (one entry used — L1 index of slide).
//     - 1 L2 table (one entry used — L2 index of slide; points at
//       the SHARED l3_kernel below).
//   Shared:
//     - 1 L3 table for the kernel-image 2 MiB region (fine-grained
//       perms; serves both TTBR0 and TTBR1 mappings).
//
// Memory cost: 10 × 4 KiB = 40 KiB. Negligible.
// ---------------------------------------------------------------------------

#define ENTRIES_PER_TABLE 512
#define PAGE_SHIFT        12
#define PAGE_SIZE         (1ull << PAGE_SHIFT)
#define BLOCK_SHIFT_L2    21
#define BLOCK_SIZE_L2     (1ull << BLOCK_SHIFT_L2)   // 2 MiB
#define BLOCK_SHIFT_L1    30
#define BLOCK_SIZE_L1     (1ull << BLOCK_SHIFT_L1)   // 1 GiB
#define BLOCK_SHIFT_L0    39

// TTBR0 tables (low 4 GiB identity).
static u64 l0_ttbr0[ENTRIES_PER_TABLE]    __attribute__((aligned(PAGE_SIZE)));
static u64 l1_ttbr0[ENTRIES_PER_TABLE]    __attribute__((aligned(PAGE_SIZE)));
static u64 l2_ttbr0[4][ENTRIES_PER_TABLE] __attribute__((aligned(PAGE_SIZE)));

// TTBR1 tables (kernel high-half). Sparse — only one entry populated
// at each level.
static u64 l0_ttbr1[ENTRIES_PER_TABLE]    __attribute__((aligned(PAGE_SIZE)));
static u64 l1_ttbr1[ENTRIES_PER_TABLE]    __attribute__((aligned(PAGE_SIZE)));
static u64 l2_ttbr1[ENTRIES_PER_TABLE]    __attribute__((aligned(PAGE_SIZE)));

// Shared L3 table for the kernel-image 2 MiB region. Both TTBR0 and
// TTBR1 walks land here for kernel-image accesses.
static u64 l3_kernel[ENTRIES_PER_TABLE]   __attribute__((aligned(PAGE_SIZE)));

// =============================================================================
// P3-Bb: Direct map + vmalloc tables (TTBR1 high half).
// =============================================================================
//
// Direct map (linear PA→KVA at base 0xFFFF_0000_0000_0000):
//   - l0_ttbr1[0] -> l1_directmap.
//   - l1_directmap[1..8] -> 1 GiB blocks at PA 1 GiB..9 GiB (RAM range).
//   - l1_directmap[0] = invalid (PA 0..1 GiB is MMIO on QEMU virt; not
//     part of direct map).
//
// vmalloc range (page-grain MMIO at base 0xFFFF_8000_0000_0000):
//   - l0_ttbr1[256] -> l1_vmalloc.
//   - l1_vmalloc[0] -> l2_vmalloc.
//   - l2_vmalloc[0] -> l3_vmalloc (covers 2 MiB; 512 × 4 KiB pages).
//   - l3_vmalloc populated by mmu_map_mmio at runtime.
//
// All direct-map PTEs unconditionally R/W + XN (PTE_KERN_RW_BLOCK has
// PTE_PXN | PTE_UXN built in). All vmalloc-MMIO PTEs are
// PTE_DEVICE_RW (Device-nGnRnE + PXN + UXN). W^X invariant I-12 holds
// at the alias level.

static u64 l1_directmap[ENTRIES_PER_TABLE] __attribute__((aligned(PAGE_SIZE)));
static u64 l1_vmalloc[ENTRIES_PER_TABLE]   __attribute__((aligned(PAGE_SIZE)));
static u64 l2_vmalloc[ENTRIES_PER_TABLE]   __attribute__((aligned(PAGE_SIZE)));
static u64 l3_vmalloc[ENTRIES_PER_TABLE]   __attribute__((aligned(PAGE_SIZE)));

// vmalloc bump-allocator cursor. Tracks the next free L3 entry index in
// l3_vmalloc. Each mmu_map_mmio call advances by ceil(size / PAGE_SIZE).
// Bounded by ENTRIES_PER_TABLE (512); v1.0 uses ~81 entries (PL011 + GIC
// dist + 4×GIC redists), leaving ~431 entries of headroom.
static u32 g_vmalloc_next_idx;

// ---------------------------------------------------------------------------
// PTE constructors and inspectors.
// ---------------------------------------------------------------------------

// Build a table descriptor pointing at a next-level table (L0/L1/L2 -> next).
// In PIE/PIC mode, (uintptr_t)table is PC-relative — gives the runtime
// PA when build_page_tables() runs with MMU off and PC = load PA.
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

// P1-H BTI guard-page invariant. A future PTE-bit refactor that moves
// PTE_GP without updating PTE_KERN_TEXT would silently disable BTI on
// kernel text on FEAT_BTI hardware. These asserts pin both the bit
// position and PTE_KERN_TEXT's adoption.
_Static_assert(PTE_GP == (1ull << 50),
               "PTE_GP must be bit 50 per ARM ARM (FEAT_BTI Stage 1 EL1 PTE)");
_Static_assert((PTE_KERN_TEXT & PTE_GP) != 0,
               "PTE_KERN_TEXT must be BTI-guarded (GP=1) so the compiler's "
               "bti markers take effect on FEAT_BTI hardware");

// ---------------------------------------------------------------------------
// Build the page tables (TTBR0 identity + TTBR1 kernel high-half).
// ---------------------------------------------------------------------------

#define PL011_BASE_FALLBACK 0x09000000ull

static void build_page_tables(u64 slide) {
    // PA of kernel start. In PIC mode this is PC-relative — gives the
    // runtime load PA when called pre-MMU-on with PC = load PA.
    u64 pa_kernel_start = (u64)(uintptr_t)_kernel_start;
    u64 kernel_2mib_pa  = pa_kernel_start & ~(BLOCK_SIZE_L2 - 1);

    // TTBR0 identity: L0[0] -> L1; L1[0..3] -> L2[0..3].
    l0_ttbr0[0] = make_table_pte(l1_ttbr0);
    for (int i = 0; i < 4; i++) {
        l1_ttbr0[i] = make_table_pte(l2_ttbr0[i]);
    }

    // TTBR0 L2 entries: default 2 MiB Normal-WB RW blocks across 4 GiB.
    for (int gib = 0; gib < 4; gib++) {
        for (int j = 0; j < ENTRIES_PER_TABLE; j++) {
            paddr_t pa = ((paddr_t)gib << BLOCK_SHIFT_L1) +
                         ((paddr_t)j << BLOCK_SHIFT_L2);
            l2_ttbr0[gib][j] = make_block_pte_l2(pa, PTE_KERN_RW_BLOCK);
        }
    }

    // TTBR0 override: PL011 region (covers 0x09000000) -> Device block.
    {
        u32 gib = (u32)(PL011_BASE_FALLBACK >> BLOCK_SHIFT_L1);
        u32 idx = (u32)((PL011_BASE_FALLBACK >> BLOCK_SHIFT_L2) & 0x1ff);
        paddr_t pa = (PL011_BASE_FALLBACK & ~(BLOCK_SIZE_L2 - 1));
        l2_ttbr0[gib][idx] = make_block_pte_l2(pa, PTE_DEVICE_RW_BLOCK);
    }

    // Build the SHARED kernel L3 table — covers the 2 MiB region
    // containing the kernel image. Same content under TTBR0 (low VA)
    // and TTBR1 (high VA): page descriptors mapping to PA.
    for (int j = 0; j < ENTRIES_PER_TABLE; j++) {
        paddr_t pa = kernel_2mib_pa + ((paddr_t)j << PAGE_SHIFT);
        l3_kernel[j] = make_page_pte_l3(pa, PTE_KERN_RW);
    }

    // Per-section overrides on l3_kernel.
    {
        u64 ks   = (u64)(uintptr_t)_kernel_start;
        u64 te   = (u64)(uintptr_t)_text_end;
        u64 re   = (u64)(uintptr_t)_rodata_end;
        u64 de   = (u64)(uintptr_t)_data_end;
        u64 bs   = (u64)(uintptr_t)_bss_start;
        u64 ke   = (u64)(uintptr_t)_kernel_end;

        // .text — RX
        for (u64 p = ks; p < te; p += PAGE_SIZE) {
            u32 i = (u32)((p - kernel_2mib_pa) >> PAGE_SHIFT);
            l3_kernel[i] = make_page_pte_l3(p, PTE_KERN_TEXT);
        }
        // .rodata + .rela.dyn + .dynamic-family — R
        // (.rela.dyn / .dynamic etc. live between .rodata and .bss in
        // the linker layout; treating them as R is conservative and
        // correct.)
        for (u64 p = te; p < re; p += PAGE_SIZE) {
            u32 i = (u32)((p - kernel_2mib_pa) >> PAGE_SHIFT);
            l3_kernel[i] = make_page_pte_l3(p, PTE_KERN_RO);
        }
        // .data + .bss + .dynamic-family writable bits — RW
        for (u64 p = de; p < ke; p += PAGE_SIZE) {
            u32 i = (u32)((p - kernel_2mib_pa) >> PAGE_SHIFT);
            l3_kernel[i] = make_page_pte_l3(p, PTE_KERN_RW);
        }
        (void)bs;       // keep symbol referenced for future use

        // Boot-stack guard page → non-present (PTE_VALID=0).
        u64 guard_pa = (u64)(uintptr_t)_boot_stack_guard;
        u32 guard_idx = (u32)((guard_pa - kernel_2mib_pa) >> PAGE_SHIFT);
        l3_kernel[guard_idx] = 0;
    }

    // TTBR0: link the kernel L3 into the appropriate L2 entry of the
    // identity map (so the kernel image is reachable at PA pre-branch).
    {
        u32 gib = (u32)(kernel_2mib_pa >> BLOCK_SHIFT_L1);
        u32 idx = (u32)((kernel_2mib_pa >> BLOCK_SHIFT_L2) & 0x1ff);
        l2_ttbr0[gib][idx] = make_table_pte(l3_kernel);
    }

    // TTBR1: kernel high-half mapping at KASLR_LINK_VA + slide.
    //
    // The high VA we want to land at is KASLR_LINK_VA + slide. Compute
    // the L0 / L1 / L2 indices for that VA. Slide is page-block-aligned
    // (2 MiB) and bounded so L0 stays fixed inside the kernel-modules
    // KASLR region (per ARCH §6.2: 0xFFFF_A000_*).
    //
    // The 2 MiB block containing KASLR_LINK_VA + slide is always
    // kernel_2mib_va_base + slide_2mib (where kernel_2mib_va_base is
    // KASLR_LINK_VA & ~(2 MiB - 1) = 0xFFFFA00000000000), because slide
    // is itself 2 MiB-aligned. Equivalently: high_va & ~(2 MiB - 1).
    {
        u64 kernel_high_va = KASLR_LINK_VA + slide;
        u64 high_va_2mib   = kernel_high_va & ~(BLOCK_SIZE_L2 - 1);

        u32 l0_idx = (u32)((high_va_2mib >> BLOCK_SHIFT_L0) & 0x1ff);
        u32 l1_idx = (u32)((high_va_2mib >> BLOCK_SHIFT_L1) & 0x1ff);
        u32 l2_idx = (u32)((high_va_2mib >> BLOCK_SHIFT_L2) & 0x1ff);

        l0_ttbr1[l0_idx] = make_table_pte(l1_ttbr1);
        l1_ttbr1[l1_idx] = make_table_pte(l2_ttbr1);
        l2_ttbr1[l2_idx] = make_table_pte(l3_kernel);     // shared L3
    }

    // P3-Bb: TTBR1 direct map at base KERNEL_DIRECT_MAP_BASE
    // (0xFFFF_0000_0000_0000). Linear PA→KVA for physical RAM.
    //
    // VA decode for 0xFFFF_0000_0000_0000:
    //   bits 47:39 = 0 → l0_ttbr1[0].
    //   bits 38:30 = 0..N → l1_directmap[gib] (1 GiB block per entry).
    //
    // Map PA 1 GiB..9 GiB (l1_directmap[1..8]). PA < 1 GiB is conventionally
    // MMIO and is not part of direct map (l1_directmap[0] left invalid).
    // PA ≥ 9 GiB (l1_directmap[9..]) left invalid; v1.0 caps at 8 GiB.
    {
        l0_ttbr1[0] = make_table_pte(l1_directmap);
        for (u32 gib = 1; gib <= 8; gib++) {
            paddr_t pa = (paddr_t)gib << BLOCK_SHIFT_L1;
            // PTE_KERN_RW_BLOCK has PTE_PXN | PTE_UXN built in: every
            // direct-map page is R/W + XN. W^X invariant I-12 holds at
            // the alias level (kernel image's RX mapping is at a
            // different VA via TTBR1 KASLR offset; same physical page
            // never both W and X via the same translation).
            l1_directmap[gib] = (pa & ~(BLOCK_SIZE_L1 - 1)) | PTE_KERN_RW_BLOCK;
        }
    }

    // P3-Bb: TTBR1 vmalloc range at base VMALLOC_BASE
    // (0xFFFF_8000_0000_0000). Page-grain MMIO mappings populated by
    // mmu_map_mmio at runtime.
    //
    // VA decode for 0xFFFF_8000_0000_0000:
    //   bits 47:39 = 256 → l0_ttbr1[256].
    //   bits 38:30 = 0   → l1_vmalloc[0].
    //   bits 29:21 = 0   → l2_vmalloc[0].
    //   bits 20:12 = 0..511 → l3_vmalloc[idx] (4 KiB pages).
    //
    // The L3 table covers the first 2 MiB of vmalloc (512 × 4 KiB).
    // l3_vmalloc entries are zero (invalid) until populated by
    // mmu_map_mmio.
    {
        l0_ttbr1[256] = make_table_pte(l1_vmalloc);
        l1_vmalloc[0] = make_table_pte(l2_vmalloc);
        l2_vmalloc[0] = make_table_pte(l3_vmalloc);
        // l3_vmalloc entries left zero — populated by mmu_map_mmio post-
        // boot.
        g_vmalloc_next_idx = 0;
    }
}

// ---------------------------------------------------------------------------
// MMU enable. Sets MAIR / TCR / TTBR / SCTLR in the canonical order:
//
//   1. MAIR_EL1 (memory attributes). Must be set before any cacheable
//      mapping is enabled.
//   2. TCR_EL1 (translation control). 48-bit VA (T0SZ=T1SZ=16),
//      4 KiB granule for both halves, IPS=40-bit PA, inner+outer WB
//      cacheable table walks for both halves.
//   3. TTBR0_EL1 = l0_ttbr0 (identity). TTBR1_EL1 = l0_ttbr1 (kernel
//      high half).
//   4. ISB (instruction sync barrier).
//   5. SCTLR_EL1 |= M (MMU on) | C (data cache) | I (instruction cache).
//   6. ISB.
//
// Once SCTLR.M=1, all addresses are translated. We keep PC = load PA
// through the transition (TTBR0 identity covers the kernel image),
// then the boot stub long-branches to the high VA via TTBR1.
// ---------------------------------------------------------------------------

#define TCR_T0SZ_48BIT   (16ull << 0)         // VA size = 64 - 16 = 48
#define TCR_TG0_4K       (0ull  << 14)
#define TCR_SH0_INNER    (3ull  << 12)
#define TCR_ORGN0_WB     (1ull  << 10)
#define TCR_IRGN0_WB     (1ull  << 8)
#define TCR_T1SZ_48BIT   (16ull << 16)
#define TCR_TG1_4K       (2ull  << 30)        // 4 KiB granule (TG1 encoding: 2 = 4K)
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

// mmu_program_this_cpu — program the MMU registers on the calling CPU
// using the already-built page tables (l0_ttbr0 / l0_ttbr1). Idempotent
// modulo SCTLR.M (the OR-in is harmless when already set).
//
// Called from:
//   1. mmu_enable (primary CPU at boot, after build_page_tables).
//   2. secondary CPUs from start.S after PSCI brings them up — they
//      re-use the primary's tables (the kernel image is shared, the
//      user-VA TTBR0 identity map covers the same low 4 GiB).
//
// SMP coherence: TTBR0 and TTBR1 point to tables in regular RAM that
// the primary already clean+invalidated to PoC during build (write
// back any dirty cache lines to PoC; secondaries re-read from PoC
// when their MMU first walks the table). The primary's tlbi_vmalle1is
// in mmu_enable below isn't needed on secondaries because their TLB
// is empty post-PSCI bring-up. P2-Ca trip-hazard: secondaries must
// clean their own dcache before reading the tables (handled by the
// PoC clean done by build_page_tables which uses dc cvau + dsb ish;
// the inner-shareable dsb makes the writeback visible to all CPUs).
void mmu_program_this_cpu(void) {
    __asm__ __volatile__(
        "msr mair_el1, %0\n"
        "msr tcr_el1, %1\n"
        "msr ttbr0_el1, %2\n"
        "msr ttbr1_el1, %3\n"
        "isb\n"
        "mrs x9, sctlr_el1\n"
        "orr x9, x9, %4\n"
        "msr sctlr_el1, x9\n"
        "isb\n"
        :: "r" ((u64)MAIR_VALUE),
           "r" ((u64)TCR_VALUE),
           "r" ((u64)(uintptr_t)l0_ttbr0),
           "r" ((u64)(uintptr_t)l0_ttbr1),
           "r" ((u64)(SCTLR_M | SCTLR_C | SCTLR_I))
        : "x9", "memory"
    );
}

void mmu_enable(u64 slide) {
    build_page_tables(slide);
    mmu_program_this_cpu();

    // After this point, all loads/stores go through the page tables.
    // PC is still load PA via TTBR0; the boot stub now long-branches
    // to the high VA via TTBR1 and continues from there.
}

// ---------------------------------------------------------------------------
// mmu_map_device — post-boot MMIO region attribute change.
//
// Walks the affected L2 entries in TTBR0's identity map (covering the
// low 4 GiB), evicts any cached lines from the prior Normal-WB
// mapping, replaces the L2 entries with Device-nGnRnE blocks, and
// flushes the TLB. Used at boot_main time to install GIC distributor
// + redistributor mappings discovered from DTB; future device drivers
// extend this pattern.
//
// Break-before-make discipline (per ARM ARM B2.7.1):
//   1. Clean+invalidate dcache by VA over the affected region. The
//      prior mapping is Normal-WB cacheable; switching to Device
//      without clearing the cache lines leaves dirty data that may
//      write back later through the new Device mapping (corrupts MMIO
//      registers). Operate by VA via the identity map (low PA == VA).
//      Then dsb ish to publish the cleans before invalidation.
//   2. Zero the L2 entries (break). dsb ishst → tlbi vmalle1is → dsb
//      ish → isb. After this, any access to the region faults.
//   3. Write the new Device descriptors (make). dsb ishst + isb.
//
// We batch BBM across all blocks in the range to keep TLB flushes
// cheap (single full-flush vs N per-VA flushes).
// ---------------------------------------------------------------------------

static inline void dsb_ish(void)   { __asm__ __volatile__("dsb ish"   ::: "memory"); }
static inline void dsb_ishst(void) { __asm__ __volatile__("dsb ishst" ::: "memory"); }
static inline void isb(void)       { __asm__ __volatile__("isb"       ::: "memory"); }
static inline void tlbi_vmalle1is(void) {
    __asm__ __volatile__("tlbi vmalle1is" ::: "memory");
}

// Conservative cache-line stride. Real ARMv8 cores have CTR_EL0.DminLine
// reporting the actual size; 64 bytes is a safe lower bound for every
// implementation we target (Cortex-A53 / A72 / A76, Apple Silicon,
// QEMU's max model). Stepping at 64 bytes either hits each line
// once (LINE >= DminLine) or visits each line multiple times
// (LINE < DminLine, harmless — `dc civac` is idempotent).
#define DCACHE_LINE_BYTES   64

bool mmu_map_device(paddr_t pa, u64 size) {
    if (size == 0) return false;
    if (pa + size < pa) return false;            // overflow
    if ((pa + size) > (4ull << 30)) return false; // outside TTBR0 identity

    // Round to enclosing 2 MiB blocks.
    paddr_t pa_first = pa & ~(BLOCK_SIZE_L2 - 1);
    paddr_t pa_last  = (pa + size - 1) & ~(BLOCK_SIZE_L2 - 1);
    paddr_t pa_end   = pa_last + BLOCK_SIZE_L2;   // exclusive end

    // F31 (audit-r3): reject ranges that overlap the kernel image's
    // 2 MiB block. That block holds a TABLE descriptor pointing at
    // the SHARED `l3_kernel` (used by both TTBR0 and TTBR1); writing
    // a Device BLOCK descriptor over it would lose the L3 reference,
    // demoting the per-section W^X mapping to a coarse 2 MiB Device
    // block (PXN=1 — kernel text becomes non-executable on the next
    // TTBR0 access). The shared L3 itself is a static array so it's
    // not freed, but the TTBR0 access path is severed.
    {
        u64 kern_pa_start_v = (u64)(uintptr_t)_kernel_start;
        u64 kern_pa_end_v   = (u64)(uintptr_t)_kernel_end;
        u64 kern_2mib       = kern_pa_start_v & ~(BLOCK_SIZE_L2 - 1);
        u64 kern_2mib_end   = ((kern_pa_end_v - 1) & ~(BLOCK_SIZE_L2 - 1))
                              + BLOCK_SIZE_L2;
        // Reject if the requested range hits ANY of the 2 MiB blocks
        // that contain the kernel image. Equivalently: ranges
        // [pa_first, pa_end) and [kern_2mib, kern_2mib_end) intersect.
        if (pa_first < kern_2mib_end && kern_2mib < pa_end) {
            return false;
        }
    }

    // Step 1: clean+invalidate dcache for the region (still mapped
    // Normal-WB at this point). Operate by VA via TTBR0 identity.
    // After this, any dirty lines from the prior mapping have been
    // written back; the cache no longer holds the region.
    for (u64 va = pa_first; va < pa_end; va += DCACHE_LINE_BYTES) {
        __asm__ __volatile__("dc civac, %0" :: "r"(va) : "memory");
    }
    dsb_ish();

    // Step 2: invalidate all affected L2 entries (break).
    for (paddr_t p = pa_first; p <= pa_last; p += BLOCK_SIZE_L2) {
        u32 gib = (u32)(p >> BLOCK_SHIFT_L1);
        u32 idx = (u32)((p >> BLOCK_SHIFT_L2) & 0x1ff);
        l2_ttbr0[gib][idx] = 0;
    }
    dsb_ishst();
    tlbi_vmalle1is();
    dsb_ish();
    isb();

    // Step 3: write the new Device descriptors (make).
    for (paddr_t p = pa_first; p <= pa_last; p += BLOCK_SIZE_L2) {
        u32 gib = (u32)(p >> BLOCK_SHIFT_L1);
        u32 idx = (u32)((p >> BLOCK_SHIFT_L2) & 0x1ff);
        l2_ttbr0[gib][idx] = make_block_pte_l2(p, PTE_DEVICE_RW_BLOCK);
    }
    dsb_ishst();
    isb();

    return true;
}

// ---------------------------------------------------------------------------
// P3-Bb: mmu_map_mmio — page-grain MMIO mapping in vmalloc range.
// ---------------------------------------------------------------------------
//
// Allocates `n_pages = ceil(size / PAGE_SIZE)` consecutive entries in
// l3_vmalloc starting at g_vmalloc_next_idx. Each entry maps the
// corresponding 4 KiB chunk of `[pa..pa+size)` with Device-nGnRnE
// attributes (PTE_DEVICE_RW). Returns the kernel VA of the first byte.
//
// `pa` must be 4 KiB-aligned (extincts otherwise). `size` is rounded
// up to PAGE_SIZE. Returns NULL if the vmalloc l3 table runs out of
// entries (logged but not fatal — the caller decides).
//
// Threading: at v1.0 P3-Bb mmu_map_mmio is called only from boot_main
// (single-threaded). The bump cursor is not lock-protected. Phase 3+
// callers from other contexts (driver init, exec) need to acquire a
// vmalloc lock before calling. Documented as a trip-hazard.
//
// No TLB flush issued: l3_vmalloc entries were previously zero (invalid),
// so there are no stale TLB entries to invalidate. dsb_ishst + isb
// publish the new entries to other CPUs via the inner-shareable scope.

void *mmu_map_mmio(paddr_t pa, size_t size) {
    if (size == 0) return NULL;
    if (pa & (PAGE_SIZE - 1)) {
        extinction("mmu_map_mmio: pa not page-aligned");
    }
    if (pa + size < pa) {
        extinction("mmu_map_mmio: pa + size overflow");
    }

    size_t aligned_size = (size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
    u32 n_pages = (u32)(aligned_size >> PAGE_SHIFT);

    if (g_vmalloc_next_idx + n_pages > ENTRIES_PER_TABLE) {
        // Out of vmalloc space. v1.0 doesn't expand; extinct loudly so
        // the bound is visible (rather than silent NULL return that a
        // caller might dereference).
        extinction("mmu_map_mmio: l3_vmalloc exhausted (>512 4-KiB MMIO pages)");
    }

    u32 start_idx = g_vmalloc_next_idx;
    for (u32 i = 0; i < n_pages; i++) {
        paddr_t page_pa = pa + ((paddr_t)i << PAGE_SHIFT);
        l3_vmalloc[start_idx + i] = make_page_pte_l3(page_pa, PTE_DEVICE_RW);
    }
    g_vmalloc_next_idx += n_pages;

    dsb_ishst();
    isb();

    return (void *)(uintptr_t)(VMALLOC_BASE + ((u64)start_idx << PAGE_SHIFT));
}

// ---------------------------------------------------------------------------
// P2-Dc: per-thread kstack guard pages.
//
// Bulk RAM is mapped via 2 MiB block descriptors at L2 in TTBR0. To make
// a single 4 KiB page no-access, we demote the relevant L2 block to an
// L3 table (allocating a fresh L3 from buddy), populate the L3 with 512
// page-PTEs reproducing the original block's mapping, then modify the
// specific L3 entry to be invalid (causing a fault on access).
//
// At v1.0 these are called single-threaded from the boot CPU. SMP-safe
// demote at Phase 5+ when multi-thread Procs make concurrent thread_create
// possible — a global mmu_lock plus careful break-before-make across
// CPUs (DSB ISH + TLB flush + DSB ISH; tlbi vmalle1is is already
// inner-shareable so all CPUs see the invalidation).
// ---------------------------------------------------------------------------

#include <thylacine/page.h>

#include "../../mm/phys.h"

// Locate the L2 entry covering `pa` in TTBR0's identity tables.
// Returns NULL if pa is outside the TTBR0 identity range (>= 4 GiB).
static u64 *l2_ttbr0_entry_for(paddr_t pa) {
    u32 gib = (u32)(pa >> BLOCK_SHIFT_L1);
    u32 idx = (u32)((pa >> BLOCK_SHIFT_L2) & 0x1ff);
    if (gib >= 4) return NULL;
    return &l2_ttbr0[gib][idx];
}

// Demote the L2 block covering `pa` to an L3 table. Idempotent: if the
// L2 entry is already a table descriptor, returns the existing L3.
//
// Allocates one page (4 KiB, page-aligned, KP_ZERO) for the L3 table.
// Returns NULL on OOM or if `pa` is outside TTBR0 identity or if the
// L2 entry is invalid (no source mapping to demote from).
static u64 *demote_l2_block_to_l3(paddr_t pa) {
    u64 *l2_entry_p = l2_ttbr0_entry_for(pa);
    if (!l2_entry_p) return NULL;
    u64 entry = *l2_entry_p;

    // Already a table descriptor → reuse.
    if ((entry & PTE_VALID) && (entry & PTE_TYPE_TABLE)) {
        return (u64 *)(uintptr_t)(entry & ~0xFFFull);
    }
    // Source must be a valid block to be demoted.
    if (!(entry & PTE_VALID)) return NULL;

    // Allocate L3 table.
    struct page *l3_pg = alloc_pages(0, KP_ZERO);
    if (!l3_pg) return NULL;
    u64 *l3 = (u64 *)(uintptr_t)page_to_pa(l3_pg);

    // Reproduce the L2 block's mapping at L3 granularity. At v1.0 bulk
    // RAM uses PTE_KERN_RW_BLOCK; the L3 equivalent is PTE_KERN_RW.
    paddr_t block_start = pa & ~(BLOCK_SIZE_L2 - 1);
    for (u32 i = 0; i < ENTRIES_PER_TABLE; i++) {
        paddr_t page_pa = block_start + ((u64)i * PAGE_SIZE);
        l3[i] = make_page_pte_l3(page_pa, PTE_KERN_RW);
    }

    // Break-before-make: invalidate the L2 entry, TLB-flush, write the
    // new table descriptor.
    *l2_entry_p = 0;
    dsb_ishst();
    tlbi_vmalle1is();
    dsb_ish();
    isb();

    *l2_entry_p = make_table_pte(l3);
    dsb_ishst();
    isb();

    return l3;
}

bool mmu_set_no_access(paddr_t pa) {
    if (pa & (PAGE_SIZE - 1)) return false;       // must be page-aligned

    u64 *l3 = demote_l2_block_to_l3(pa);
    if (!l3) return false;

    u32 idx = (u32)((pa >> PAGE_SHIFT) & 0x1ff);

    // Invalidate the page's L3 entry. Break-before-make for this single
    // PTE: the entry was a valid PTE_KERN_RW (set by demote_l2_block_to_l3)
    // and we're transitioning to invalid; ARM ARM requires break before
    // any change that could change the output address or permissions.
    l3[idx] = 0;
    dsb_ishst();
    tlbi_vmalle1is();
    dsb_ish();
    isb();

    return true;
}

bool mmu_restore_normal(paddr_t pa) {
    if (pa & (PAGE_SIZE - 1)) return false;

    u64 *l2_entry_p = l2_ttbr0_entry_for(pa);
    if (!l2_entry_p) return false;
    u64 entry = *l2_entry_p;

    // If the L2 entry is still a block, the page is implicitly RW
    // already — nothing to restore (mmu_set_no_access was never called
    // for any page in this 2 MiB block).
    if ((entry & PTE_VALID) && !(entry & PTE_TYPE_TABLE)) {
        return true;
    }
    // Or if the L2 entry is invalid, that's a misuse (caller is trying
    // to unprotect something never mapped). Return failure.
    if (!(entry & PTE_VALID)) return false;

    u64 *l3 = (u64 *)(uintptr_t)(entry & ~0xFFFull);
    u32 idx = (u32)((pa >> PAGE_SHIFT) & 0x1ff);

    l3[idx] = make_page_pte_l3(pa, PTE_KERN_RW);
    dsb_ishst();
    tlbi_vmalle1is();
    dsb_ish();
    isb();

    return true;
}

bool mmu_set_no_access_range(paddr_t pa, unsigned n_pages) {
    if (pa & (PAGE_SIZE - 1)) return false;
    if (n_pages == 0)         return false;
    paddr_t end = pa + (paddr_t)n_pages * PAGE_SIZE;
    if (end <= pa)            return false;       // overflow
    // Range must lie within a single 2 MiB block.
    if ((pa >> BLOCK_SHIFT_L2) != ((end - 1) >> BLOCK_SHIFT_L2)) return false;

    u64 *l3 = demote_l2_block_to_l3(pa);
    if (!l3) return false;

    // Invalidate each L3 entry in the run; one TLB flush at the end.
    for (unsigned i = 0; i < n_pages; i++) {
        u32 idx = (u32)(((pa >> PAGE_SHIFT) + i) & 0x1ff);
        l3[idx] = 0;
    }
    dsb_ishst();
    tlbi_vmalle1is();
    dsb_ish();
    isb();
    return true;
}

bool mmu_restore_normal_range(paddr_t pa, unsigned n_pages) {
    if (pa & (PAGE_SIZE - 1)) return false;
    if (n_pages == 0)         return false;
    paddr_t end = pa + (paddr_t)n_pages * PAGE_SIZE;
    if (end <= pa)            return false;
    if ((pa >> BLOCK_SHIFT_L2) != ((end - 1) >> BLOCK_SHIFT_L2)) return false;

    u64 *l2_entry_p = l2_ttbr0_entry_for(pa);
    if (!l2_entry_p) return false;
    u64 entry = *l2_entry_p;

    if ((entry & PTE_VALID) && !(entry & PTE_TYPE_TABLE)) {
        // Block is still mapped as a single 2 MiB block — pages are
        // already normal, nothing to do.
        return true;
    }
    if (!(entry & PTE_VALID)) return false;

    u64 *l3 = (u64 *)(uintptr_t)(entry & ~0xFFFull);
    for (unsigned i = 0; i < n_pages; i++) {
        paddr_t page_pa = pa + (paddr_t)i * PAGE_SIZE;
        u32 idx = (u32)((page_pa >> PAGE_SHIFT) & 0x1ff);
        l3[idx] = make_page_pte_l3(page_pa, PTE_KERN_RW);
    }
    dsb_ishst();
    tlbi_vmalle1is();
    dsb_ish();
    isb();
    return true;
}
