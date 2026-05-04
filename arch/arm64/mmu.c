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

void mmu_enable(u64 slide) {
    build_page_tables(slide);

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
