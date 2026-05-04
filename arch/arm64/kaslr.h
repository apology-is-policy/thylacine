// KASLR — Kernel Address Space Layout Randomization (P1-C-extras Part B).
//
// Per ARCHITECTURE.md §5.3 + §6.2 + §24 + invariant I-16. The kernel is
// linked at a fixed high VA (KASLR_LINK_VA = 0xFFFFA00000080000) inside
// TTBR1's "kernel modules + KASLR" region (0xFFFF_A000_*). At boot, the
// kernel chooses a random offset (page-block-aligned, bounded < 1 GiB),
// applies any R_AARCH64_RELATIVE entries in the embedded .rela.dyn
// section, builds a TTBR1 mapping at KASLR_LINK_VA + offset, then
// long-branches to the runtime VA.
//
// On QEMU virt the seed comes from /chosen/rng-seed (which UEFI usually
// publishes; QEMU's direct -kernel boot also publishes 256 bits there).
// On bare-metal Pi 5 (post-v1.0) UEFI publishes /chosen/kaslr-seed. The
// fallback for both being absent is the cycle counter (cntpct_el0), with
// a logged warning in the boot banner.
//
// "kaslr_init" is the boot-time entry point. It returns the chosen
// slide (= runtime_va_base - KASLR_LINK_VA) which the boot stub passes
// to mmu_enable() for the TTBR1 mapping and uses to compute the high-
// VA branch target after MMU enable.

#ifndef THYLACINE_ARCH_ARM64_KASLR_H
#define THYLACINE_ARCH_ARM64_KASLR_H

#include <thylacine/types.h>

// Link-time virtual base address of the kernel image. Mirrors
// KERNEL_LINK_VA in arch/arm64/kernel.ld; the linker ASSERT enforces
// the C / linker-script values agree. We can't read the linker's
// KERNEL_LINK_VA at runtime in PIE/PIC mode (PC-relative addressing
// gives the load PA, not the link VA), so the constant lives here.
#define KASLR_LINK_VA  0xFFFFA00000080000ull

// Maximum KASLR slide. Bounded so the L0 table walk stays in a single
// L0 entry (bits 47..39 of VA must be unchanged across the kaslr range
// — see mmu.c TTBR1 setup). KASLR_LINK_VA bits 38..0 = 0x80000, and
// 2^39 - 0x80000 = ~512 GiB headroom before bit 39 carries; we stay
// well clear at 16 GiB max.
//
// Mask preserves bits 33..21 = 13 bits = 8192 distinct 2 MiB-aligned
// offsets in [0, 16 GiB). Conservative compared to ARCH §6.2's
// allowable 32 TiB range, but a comfortable balance of entropy and
// table-builder simplicity (single L0 entry, dynamic L1/L2 indices).
#define KASLR_OFFSET_MASK   0x3FFE00000ull  // 16 GiB - 2 MiB, page-block aligned
#define KASLR_ALIGN_BITS    21              // 2 MiB alignment

// Source of the seed used. Surfaced in the boot banner for diagnostic
// purposes (so a developer can tell whether high-entropy DTB seed or
// the low-entropy cycle-counter fallback was used).
typedef enum {
    KASLR_SEED_NONE             = 0,    // pre-init / no entropy at all
    KASLR_SEED_DTB_KASLR_SEED   = 1,    // /chosen/kaslr-seed (UEFI on bare metal)
    KASLR_SEED_DTB_RNG_SEED     = 2,    // /chosen/rng-seed (QEMU virt fallback)
    KASLR_SEED_CNTPCT           = 3,    // ARM generic counter fallback
} kaslr_seed_source_t;

// Pick a slide based on the best available entropy source, then walk
// .rela.dyn applying R_AARCH64_RELATIVE entries. Called from
// arch/arm64/start.S immediately before mmu_enable(). Returns the
// chosen slide (in bytes); pass to mmu_enable() so it builds the TTBR1
// mapping at KASLR_LINK_VA + slide.
//
// Side effect: writes to BSS variables _kaslr_offset and _kaslr_seed_source
// for boot_main()'s banner. _saved_dtb_ptr must be valid by call time
// (i.e., the BSS clear and DTB-save in _real_start have run).
u64 kaslr_init(void);

// Diagnostic accessors used by boot_main() for the banner.
u64 kaslr_get_offset(void);
kaslr_seed_source_t kaslr_get_seed_source(void);

// String form of the seed source (for the boot banner). Returned
// pointer is to a static literal; callers must not free.
const char *kaslr_seed_source_str(kaslr_seed_source_t s);

// Translate a load-PA address (typically obtained via PC-relative
// adrp+add at boot, when PC = load PA) into the corresponding
// post-KASLR high VA. Used by the boot stub to compute the long-
// branch target after mmu_enable() returns.
//
// Caller must call this AFTER kaslr_init() (which sets g_kaslr_offset).
u64 kaslr_high_va_addr(void *pa);

// The runtime kernel high-VA base = KASLR_LINK_VA + offset. Surfaced
// for the banner ("kernel base: 0x...").
u64 kaslr_kernel_high_base(void);

// Cached load-PA bounds of the kernel image. kaslr_init() captures
// (uintptr_t)_kernel_start and (uintptr_t)_kernel_end while running
// at PA (PC = load PA, PC-relative addressing gives PA). After the
// long-branch into TTBR1, those PC-relative computations resolve to
// high VAs; consumers that need the load PA (e.g., phys_init for the
// kernel-image reservation) read these accessors instead.
u64 kaslr_kernel_pa_start(void);
u64 kaslr_kernel_pa_end(void);

#endif // THYLACINE_ARCH_ARM64_KASLR_H
