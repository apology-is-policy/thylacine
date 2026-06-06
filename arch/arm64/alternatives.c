// Boot-time instruction patcher (Lazarus W1.5). See alternatives.h +
// PORTABILITY.md section 4.5. Iterates the .altinstructions table built by
// the ALTERNATIVE() macro and, for each entry whose feature the running CPU
// implements, rewrites the in-place baseline (LL/SC) to the recorded
// single-instruction replacement (LSE), NOP-padding the tail. The write
// itself goes through mmu_patch_text (a transient RW-not-X alias, so W^X /
// I-12 is never violated).
//
// Audit-bearing: self-modifying .text. ARCHITECTURE.md section 25.4 +
// CLAUDE.md audit-trigger table.

#include "alternatives.h"
#include <thylacine/types.h>
#include <thylacine/extinction.h>
#include "hwfeat.h"
#include "mmu.h"                   // mmu_patch_text

// One .altinstructions entry. Both offsets are PC-relative to their own
// field (built by the ALTERNATIVE macro's `.word X - .`), keeping the table
// reloc-free and KASLR-independent. 12 bytes, pinned.
struct alt_instr {
    s32 site_off;     // .word 661b - . : LL/SC site in .text
    s32 repl_off;     // .word 663f - . : LSE replacement in .altinstr_replacement
    u16 feature;      // ALT_FEAT_*
    u8  orig_len;     // bytes at the site (the LL/SC sequence)
    u8  alt_len;      // bytes of replacement (a single LSE instruction = 4)
} __attribute__((packed));
_Static_assert(sizeof(struct alt_instr) == 12, "alt_instr wire layout");

// Linker-provided bounds of the table (kernel.ld, inside .rodata).
extern const struct alt_instr _alt_instructions_start[];
extern const struct alt_instr _alt_instructions_end[];

u32 g_alt_total;
u32 g_alt_applied;

#define A64_NOP   0xD503201Fu   // `nop` (little-endian word)

static bool alt_feature_present(u16 feature) {
    switch (feature) {
    case ALT_FEAT_LSE: return g_hw_features.atomic;
    default:           return false;   // unknown feature id: never patch
    }
}

void apply_alternatives(void) {
    // Mask ALL of DAIF (D/A/I/F) for the whole pass. Single-CPU here
    // (pre-smp_init), so the mask is the only thing that could otherwise
    // execute a half-patched site (an IRQ handler taking a spinlock whose
    // xchg is mid-rewrite). Masking FIQ + SError too -- not just IRQ --
    // also closes the (today-benign) window in patch_kva_to_pa where a
    // handler taken between `at s1e1r` and the `mrs par_el1` read could
    // clobber PAR_EL1 and yield a wrong PA; mirrors the #713 eret-window
    // full-DAIF discipline.
    u64 daif;
    __asm__ __volatile__("mrs %0, daif\n\tmsr daifset, #0xf\n"
                         : "=r"(daif) :: "memory");

    g_alt_total = 0;
    g_alt_applied = 0;

    for (const struct alt_instr *a = _alt_instructions_start;
         a < _alt_instructions_end; a++) {
        g_alt_total++;
        if (!alt_feature_present(a->feature))
            continue;
        // The baseline is always the longer (or equal) sequence -- the
        // replacement must fit with room to NOP-pad. A violation is a
        // build-time authoring bug in an ALTERNATIVE() use.
        if (a->alt_len > a->orig_len || (a->orig_len & 3u) || (a->alt_len & 3u))
            extinction("apply_alternatives: malformed entry (len)");

        u8 *site = (u8 *)((const u8 *)&a->site_off + a->site_off);
        const u8 *repl = (const u8 *)&a->repl_off + a->repl_off;

        // Build [replacement | NOP-pad] in a stack buffer, then write the
        // whole orig_len region in one shot (one canonical fetch-stream
        // change per site). Max site length is bounded; 32 bytes is ample
        // for any single RMW LL/SC sequence.
        u8 buf[32];
        if (a->orig_len > sizeof(buf))
            extinction("apply_alternatives: orig_len exceeds patch buffer");
        for (u8 i = 0; i < a->alt_len; i++)
            buf[i] = repl[i];
        for (u32 off = a->alt_len; off < a->orig_len; off += 4) {
            buf[off + 0] = (u8)(A64_NOP);
            buf[off + 1] = (u8)(A64_NOP >> 8);
            buf[off + 2] = (u8)(A64_NOP >> 16);
            buf[off + 3] = (u8)(A64_NOP >> 24);
        }
        mmu_patch_text(site, buf, a->orig_len);
        g_alt_applied++;
    }

    __asm__ __volatile__("msr daif, %0\n" :: "r"(daif) : "memory");
}
