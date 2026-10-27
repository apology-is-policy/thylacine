// Boot-time LSE alternatives-patcher (Lazarus W1.5).
//
// Two proofs:
//   - patch_applied: the .altinstructions table is non-empty and
//     apply_alternatives patched exactly the entries whose feature the CPU
//     implements (all on an LSE core; none on a v8.0 core). g_alt_applied
//     == g_alt_total is the direct evidence the boot pass ran and rewrote
//     the expected count -- no fragile disassembly required.
//   - atomics_correct: every routed patchable primitive computes the right
//     PRE value and stored result. On -cpu max (the CI baseline) these run
//     the PATCHED single-instruction LSE forms (swpa/ldadd/ldaddal); on a
//     v8.0 core they run the LL/SC baseline. Either way the answers must
//     match -- proving per-op LL/SC <-> LSE equivalence on live hardware.

#include "test.h"

#include "../../arch/arm64/alternatives.h"
#include "../../arch/arm64/atomic_lse.h"
#include "../../arch/arm64/hwfeat.h"
#include <thylacine/types.h>

void test_alternatives_patch_applied(void) {
    // The spinlock test-and-set + the Spoor/SrvConn refcounts + the
    // scheduler steal-rotate are all inlined widely, so every build emits
    // many FEAT_LSE entries.
    TEST_ASSERT(g_alt_total > 0,
        "alternatives table is empty (no patch sites emitted)");

    // apply_alternatives ran at boot (kernel/main.c, pre-smp_init). Every
    // entry is FEAT_LSE at W1.5: on an LSE-capable CPU all are applied; on
    // the v8.0 floor none are.
    if (g_hw_features.atomic) {
        TEST_EXPECT_EQ(g_alt_applied, g_alt_total,
            "FEAT_LSE present but not every alternative was patched");
    } else {
        TEST_EXPECT_EQ(g_alt_applied, 0u,
            "no FEAT_LSE yet the patcher applied alternatives");
    }
}

void test_alternatives_atomics_correct(void) {
    // exchange-acquire (the spinlock primitive): returns PRE, stores new.
    u32 x = 0x11223344u;
    TEST_EXPECT_EQ(t_atomic_xchg_acq_u32(&x, 0xDEADBEEFu), 0x11223344u,
        "xchg returned wrong PRE value");
    TEST_EXPECT_EQ(x, 0xDEADBEEFu, "xchg stored wrong value");

    // fetch_add relaxed (u32) -- the scheduler steal-rotate shape.
    u32 c = 100u;
    TEST_EXPECT_EQ(t_atomic_fetch_add_relaxed_u32(&c, 7u), 100u, "fadd u32 PRE");
    TEST_EXPECT_EQ(c, 107u, "fadd u32 result");

    // fetch_add relaxed + acqrel (int) -- the refcount shape.
    int r = 1;
    TEST_EXPECT_EQ(t_atomic_fetch_add_relaxed_int(&r, 1), 1, "fadd int relaxed PRE");
    TEST_EXPECT_EQ(r, 2, "fadd int relaxed result");
    TEST_EXPECT_EQ(t_atomic_fetch_add_acqrel_int(&r, 3), 2, "fadd int acqrel PRE");
    TEST_EXPECT_EQ(r, 5, "fadd int acqrel result");

    // fetch_sub == fetch_add of the negation: PRE returned, difference stored.
    TEST_EXPECT_EQ(t_atomic_fetch_sub_acqrel_int(&r, 2), 5, "fsub int acqrel PRE");
    TEST_EXPECT_EQ(r, 3, "fsub int acqrel result");
    TEST_EXPECT_EQ(t_atomic_fetch_sub_relaxed_int(&r, 3), 3, "fsub int relaxed PRE");
    TEST_EXPECT_EQ(r, 0, "fsub int relaxed result");

    // Negation identity under wrap: subtract more than held -> two's-complement.
    int w = 5;
    TEST_EXPECT_EQ(t_atomic_fetch_sub_relaxed_int(&w, 8), 5, "fsub wrap PRE");
    TEST_EXPECT_EQ(w, -3, "fsub wrap result");
}
