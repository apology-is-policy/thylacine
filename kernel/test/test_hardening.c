// hardening leaf-API smoke (P1-H).
//
// Verifies:
//   - Stack canary cookie was initialized to a non-zero value
//     (canary_init ran from kaslr_init).
//   - hw_features_detect populated g_hw_features consistently with
//     a fresh read of the ID registers (catch-bug for the field-
//     extraction layout drifting from ARM ARM).
//   - PAC + BTI are runtime-conditional (Lazarus W1): IFF the running
//     CPU implements the feature, start.S enabled it in SCTLR_EL1 (the
//     F25 anti-performative check, in conditional form). On the v8.0
//     floor (A72) both are absent and the implication is vacuous.

#include "test.h"

#include "../../arch/arm64/hwfeat.h"
#include <thylacine/canary.h>
#include <thylacine/types.h>

static u64 read_id_aa64isar1_el1_again(void) {
    u64 v;
    __asm__ __volatile__("mrs %0, id_aa64isar1_el1" : "=r"(v));
    return v;
}

static u64 read_id_aa64pfr1_el1_again(void) {
    u64 v;
    __asm__ __volatile__("mrs %0, id_aa64pfr1_el1" : "=r"(v));
    return v;
}

void test_hardening_detect_smoke(void) {
    // Canary cookie should be non-zero (kaslr_init seeded it from
    // mixed entropy; our canary_init guarantees non-zero output).
    TEST_ASSERT(canary_get_cookie() != 0,
        "stack canary cookie is zero (canary_init didn't run?)");

    // hw_features_detect's claims should match a fresh read of the
    // ID registers — catches a field-extraction drift.
    u64 isar1 = read_id_aa64isar1_el1_again();
    bool pac_apa_fresh = ((isar1 >> 4) & 0xF) != 0;
    TEST_EXPECT_EQ((u64)g_hw_features.pac_apa, (u64)pac_apa_fresh,
        "g_hw_features.pac_apa drifted from ID_AA64ISAR1_EL1");

    u64 pfr1 = read_id_aa64pfr1_el1_again();
    bool bti_fresh = ((pfr1 >> 0) & 0xF) != 0;
    TEST_EXPECT_EQ((u64)g_hw_features.bti, (u64)bti_fresh,
        "g_hw_features.bti drifted from ID_AA64PFR1_EL1");

    // Lazarus W1: PAC + BTI are runtime-conditional now (the v8.0 floor /
    // A72 has neither), so we no longer REQUIRE the CPU to implement them.
    // But the P1-H audit F25 anti-performative invariant still holds in its
    // conditional form: IFF the running CPU implements the feature, start.S
    // (pac_apply_this_cpu) MUST have enabled it in SCTLR_EL1 -- otherwise the
    // compile-time markers are emitted but silently NOP and hardening becomes
    // performative. On a CPU without the feature the SCTLR bit is gated off
    // (and RES0 anyway), so the implication is vacuously satisfied.
    u64 sctlr;
    __asm__ __volatile__("mrs %0, sctlr_el1" : "=r"(sctlr));

    bool pac = g_hw_features.pac_apa || g_hw_features.pac_api;
    bool en_ia = (sctlr & (1ull << 31)) != 0;
    TEST_ASSERT(!pac || en_ia,
        "CPU implements PAC but SCTLR_EL1.EnIA is clear (PAC is performative)");

    bool en_bt0 = (sctlr & (1ull << 35)) != 0;
    TEST_ASSERT(!g_hw_features.bti || en_bt0,
        "CPU implements BTI but SCTLR_EL1.BT0 is clear (BTI not enforced)");
}
