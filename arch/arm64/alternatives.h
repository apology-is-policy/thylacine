// Boot-time instruction-patching framework (Lazarus W1.5).
//
// The kernel is compiled to the ARMv8.0 floor (LL/SC atomics; no LSE) so
// one binary runs on every v8.0+ core (A72 included). On cores that DO
// implement a later feature we rewrite the relevant instruction sites at
// boot to the faster single-instruction form -- with no steady-state
// runtime branch and no function call. This is the Linux arm64
// "alternatives" model (apply_alternatives <- arch/arm64/kernel/alternative.c).
//
// At W1.5 the only feature is FEAT_LSE: the LL/SC RMW atomics
// (arch/arm64/atomic_lse.h) carry a single-instruction LSE replacement
// that the boot pass copies over the LL/SC sequence iff the CPU has LSE.
//
// PORTABILITY.md section 4.5 is the binding design. The pass is
// audit-bearing (it self-modifies .text) -- ARCHITECTURE.md section 25.4 +
// CLAUDE.md audit-trigger table.

#ifndef THYLACINE_ARM64_ALTERNATIVES_H
#define THYLACINE_ARM64_ALTERNATIVES_H

#include <thylacine/types.h>

// Feature ids recorded in each .altinstructions entry. apply_alternatives()
// maps each to a g_hw_features predicate. MUST be plain integer literals
// (the value is stringized into a `.hword` assembler directive below, so a
// `u` suffix or a non-numeric token would not assemble).
#define ALT_FEAT_LSE   0

// Two-level stringize so the feature ARGUMENT is macro-expanded to its
// integer value before being pasted into the `.hword` directive (a bare
// `#feature` would stringize the token name, e.g. "ALT_FEAT_LSE").
#define ALT_STR_(x)    #x
#define ALT_XSTR_(x)   ALT_STR_(x)

// ALTERNATIVE(oldinstr, newinstr, feature)
//
// Emits `oldinstr` in place in .text (the always-correct baseline that runs
// on every core), records a patch entry in .altinstructions, and stashes
// `newinstr` (the single-instruction replacement) in .altinstr_replacement.
// apply_alternatives() copies `newinstr` over the `oldinstr` site at boot
// and NOP-pads the tail iff `feature` is present.
//
// Both .word offsets are PC-relative to their own location, so the table is
// reloc-free and KASLR-independent: the table entry and its target slide by
// the same boot-time offset, leaving the stored deltas valid (the same
// trick the Halls symbol table uses; see kernel.ld + halls.c). The
// replacement always fits because the LL/SC baseline (the longer sequence)
// is the `oldinstr` -- apply_alternatives extincts if alt_len > orig_len.
//
// Section attribute "a" (allocatable, read-only) keeps both metadata
// sections RO+NX: the patcher only READS them. No page is ever writable and
// executable -- W^X / I-12 holds (the write itself goes through a transient
// RW-not-X alias in mmu_patch_text).
#define ALTERNATIVE(oldinstr, newinstr, feature)                 \
    "661:\n\t" oldinstr "\n662:\n"                               \
    ".pushsection .altinstructions,\"a\",@progbits\n"            \
    "  .word 661b - .\n"                                         \
    "  .word 663f - .\n"                                         \
    "  .hword " ALT_XSTR_(feature) "\n"                          \
    "  .byte  662b - 661b\n"                                     \
    "  .byte  664f - 663f\n"                                     \
    ".popsection\n"                                              \
    ".pushsection .altinstr_replacement,\"a\",@progbits\n"       \
    "663:\n\t.arch_extension lse\n\t" newinstr                   \
    "\n\t.arch_extension nolse\n664:\n"                          \
    ".popsection\n"

// The boot pass. Runs once, single-CPU, IRQ-masked, after
// hw_features_detect() and before smp_init() (so no other core executes a
// site mid-patch, and secondaries fetch already-patched bytes with cold
// I-caches). Idempotent.
void apply_alternatives(void);

// Counters the pass records, for the unit test: g_alt_total = entries seen;
// g_alt_applied = entries whose feature was present and were patched. On
// LSE hardware (every entry is FEAT_LSE at W1.5) applied == total; on a
// non-LSE core applied == 0.
extern u32 g_alt_total;
extern u32 g_alt_applied;

#endif // THYLACINE_ARM64_ALTERNATIVES_H
