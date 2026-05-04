// KASLR (Kernel ASLR) — chooses a randomized kernel base, applies any
// R_AARCH64_RELATIVE relocations, and reports the chosen slide back to
// the boot stub for TTBR1 mapping and the high-VA long-branch.
//
// At P1-C-extras Part B with -fpie + -fdirect-access-external-data, the
// kernel emits zero R_AARCH64_RELATIVE entries (all references are
// PC-relative). The relocation walker therefore terminates after a
// single header inspection. The infrastructure is in place for future
// code that introduces absolute pointer references (e.g., function
// pointer tables in static data); each such reference will land in
// .rela.dyn automatically and be patched here.
//
// Per ARCHITECTURE.md §5.3 (KASLR), §6.2 (memory layout), §24
// (hardening), invariant I-16.

#include "kaslr.h"

#include <stdint.h>
#include <thylacine/dtb.h>
#include <thylacine/types.h>

// From arch/arm64/start.S — DTB physical address (saved before MMU on).
extern volatile u64 _saved_dtb_ptr;

// From arch/arm64/kernel.ld — bounds of .rela.dyn.
extern char _rela_start[];
extern char _rela_end[];

// PC-relative anchor used to compute the kernel's load PA at runtime
// (PIC mode: adrp+add of any kernel symbol gives the runtime PA when
// running with MMU off and PC = load PA).
extern char _kernel_start[];

// ---------------------------------------------------------------------------
// State surfaced to boot_main() for the banner.
// ---------------------------------------------------------------------------

static u64 g_kaslr_offset;
static kaslr_seed_source_t g_kaslr_seed_source;

// Load PAs of the kernel image, cached during kaslr_init while we're
// still running at PA (PC = load PA, so PC-relative adrp+add gives
// the load PA). After the long-branch into TTBR1, PC-relative gives
// high VA, so phys_init reads these accessors instead of recomputing.
//
// `volatile` is load-bearing here. Without it, clang at -O2 with
// -fpie -mcmodel=tiny folds the store-then-load pattern: it observes
// that g_kernel_pa_start is only ever assigned `(uintptr_t)_kernel_start`
// (which it treats as a link-time constant) and rewrites the storage
// as a 1-byte boolean ("was it set?"), then in the accessor returns
// the link-time address gated on that boolean. At runtime under PIE,
// the link-time address ≠ load PA, so the value comes back wrong.
// The fix is to force actual 8-byte memory traffic via volatile —
// the address that gets stored is the one the adrp+add at the call
// site computed (PA when running pre-MMU, high VA after the branch).
static volatile u64 g_kernel_pa_start;
static volatile u64 g_kernel_pa_end;

u64 kaslr_get_offset(void) {
    return g_kaslr_offset;
}

kaslr_seed_source_t kaslr_get_seed_source(void) {
    return g_kaslr_seed_source;
}

u64 kaslr_high_va_addr(void *pa) {
    u64 pa_val      = (u64)(uintptr_t)pa;
    u64 pa_kernel   = (u64)(uintptr_t)_kernel_start;
    u64 image_off   = pa_val - pa_kernel;
    return KASLR_LINK_VA + g_kaslr_offset + image_off;
}

u64 kaslr_kernel_high_base(void) {
    return KASLR_LINK_VA + g_kaslr_offset;
}

u64 kaslr_kernel_pa_start(void) {
    return g_kernel_pa_start;
}

u64 kaslr_kernel_pa_end(void) {
    return g_kernel_pa_end;
}

const char *kaslr_seed_source_str(kaslr_seed_source_t s) {
    switch (s) {
    case KASLR_SEED_DTB_KASLR_SEED: return "DTB /chosen/kaslr-seed";
    case KASLR_SEED_DTB_RNG_SEED:   return "DTB /chosen/rng-seed";
    case KASLR_SEED_CNTPCT:         return "cntpct (low-entropy fallback)";
    case KASLR_SEED_NONE:
    default:                        return "none";
    }
}

// ---------------------------------------------------------------------------
// Entropy sources, in priority order.
// ---------------------------------------------------------------------------

// Hardware fallback. cntpct_el0 (physical counter) is readable at EL1
// regardless of HCR_EL2.TGE / CNTHCTL_EL2 settings (we already enabled
// EL1PCTEN in the EL2 drop sequence). Counter advances at the
// generic-timer frequency (typically 24 MHz on QEMU virt or 19.2 MHz
// on Pi 5); a few ms of boot is millions of cycles, so the low bits
// have some entropy from boot-time variance.
static u64 read_cntpct(void) {
    u64 v;
    __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

// Mix function — small SipHash-like avalanche using XOR + rotate +
// multiply with an odd constant. Spreads entropy across all 64 bits
// when the input has structured low-bit entropy (e.g., a counter).
static u64 mix64(u64 x) {
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ull;
    x ^= x >> 33;
    return x;
}

// ---------------------------------------------------------------------------
// Relocation walker.
//
// .rela.dyn contains an array of struct elf64_rela. For each entry of
// type R_AARCH64_RELATIVE: write (addend + slide) at the target. The
// target's link-time VA is r_offset; we convert to PA via the runtime
// adrp-derived load_pa, since the boot stub runs with MMU off.
// ---------------------------------------------------------------------------

struct elf64_rela {
    u64 r_offset;
    u64 r_info;
    s64 r_addend;
};

#define R_AARCH64_RELATIVE 1027u

static void apply_relocations(u64 slide) {
    // PIC: (uintptr_t)_kernel_start at runtime gives the runtime PA
    // (PC-relative addressing). Use it to derive the slide between
    // link VA and load PA, so we can convert link-VA reloc targets
    // to PAs we can write to with MMU still off.
    u64 pa_kernel_start = (u64)(uintptr_t)_kernel_start;
    s64 link_to_pa = (s64)pa_kernel_start - (s64)KASLR_LINK_VA;

    const struct elf64_rela *rel = (const struct elf64_rela *)(uintptr_t)_rela_start;
    const struct elf64_rela *end = (const struct elf64_rela *)(uintptr_t)_rela_end;

    while (rel < end) {
        u32 type = (u32)(rel->r_info & 0xFFFFFFFFu);
        if (type == R_AARCH64_RELATIVE) {
            u64 target_pa = (u64)((s64)rel->r_offset + link_to_pa);
            u64 *target = (u64 *)(uintptr_t)target_pa;
            *target = (u64)(rel->r_addend + (s64)slide);
        }
        // Other types (R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, etc.)
        // shouldn't appear in our static-pie kernel. If they do, the
        // walker silently skips them; build-time linker flags keep the
        // surface narrow.
        rel++;
    }
}

// ---------------------------------------------------------------------------
// kaslr_init — compose entropy, choose a slide, apply relocations.
// ---------------------------------------------------------------------------

u64 kaslr_init(void) {
    u64 raw = 0;
    kaslr_seed_source_t source = KASLR_SEED_NONE;

    // Cache load-PA bounds of the kernel image. We're still running
    // at PA here (PC = load PA pre-MMU + pre-long-branch), so the
    // PC-relative adrp+add evaluates to the runtime load PA. After
    // the long-branch into TTBR1 (kicked by start.S), the same
    // expression resolves to the high VA — phys_init reads these
    // cached values instead.
    g_kernel_pa_start = (u64)(uintptr_t)_kernel_start;
    extern char _kernel_end[];
    g_kernel_pa_end   = (u64)(uintptr_t)_kernel_end;

    // Initialize the DTB parser if the boot stub didn't already.
    if (!dtb_is_ready()) {
        (void)dtb_init((paddr_t)_saved_dtb_ptr);
    }

    // Try DTB sources in priority order: kaslr-seed (UEFI on bare
    // metal), then rng-seed (QEMU virt and most UEFI environments),
    // then the cntpct hardware-counter fallback.
    raw = dtb_get_chosen_kaslr_seed();
    if (raw != 0) {
        source = KASLR_SEED_DTB_KASLR_SEED;
    } else {
        raw = dtb_get_chosen_rng_seed();
        if (raw != 0) {
            source = KASLR_SEED_DTB_RNG_SEED;
        } else {
            raw = read_cntpct();
            if (raw != 0) {
                source = KASLR_SEED_CNTPCT;
            }
        }
    }

    // Mix bits to avoid weak low-order entropy in the cntpct path.
    u64 mixed = mix64(raw);

    // Choose offset: 2 MiB-aligned, bounded to < 1 GiB. Always non-zero
    // (so KASLR doesn't trivially return slide=0 if mix happens to give
    // us all-zero low bits).
    u64 offset = mixed & KASLR_OFFSET_MASK;
    if (offset == 0) {
        offset = 1ull << KASLR_ALIGN_BITS;   // minimum 2 MiB
    }

    g_kaslr_offset = offset;
    g_kaslr_seed_source = source;

    // The slide IS the offset (since we link at KASLR_LINK_VA and run
    // at KASLR_LINK_VA + offset, the slide between them is offset).
    apply_relocations(offset);

    return offset;
}
