// hardening leaf-API smoke (P1-H).
//
// Verifies:
//   - Stack canary cookie was initialized to a non-zero value
//     (canary_init ran from kaslr_init).
//   - hw_features_detect populated g_hw_features consistently with
//     a fresh read of the ID registers (catch-bug for the field-
//     extraction layout drifting from ARM ARM).
//   - At least PAC + BTI are detected on the QEMU virt + Pi 5 target
//     hardware (would FAIL on bare ARMv8.0 — but neither of our v1.0
//     targets is bare 8.0; would surface a regression that breaks our
//     hardening assumptions).

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

    // Sanity: on QEMU virt with -cpu max + Pi 5, both PAC and BTI
    // should be implemented. If a future toolchain change demotes the
    // detected feature set silently, this catches it.
    TEST_ASSERT(g_hw_features.pac_apa || g_hw_features.pac_api,
        "expected PAC support on this CPU (ARMv8.3+ target)");
    TEST_ASSERT(g_hw_features.bti,
        "expected BTI support on this CPU (ARMv8.5+ target)");

    // SCTLR_EL1 runtime enforcement check (P1-H audit F25). If a future
    // refactor drops the SCTLR write in start.S, the compile-time PAC/
    // BTI markers are emitted but become HINT-space NOPs at runtime —
    // boot succeeds silently, hardening becomes performative. This
    // assertion catches that class of regression.
    u64 sctlr;
    __asm__ __volatile__("mrs %0, sctlr_el1" : "=r"(sctlr));
    TEST_ASSERT((sctlr & (1ull << 31)) != 0,
        "SCTLR_EL1.EnIA not set (PAC instructions are NOPs)");
    TEST_ASSERT((sctlr & (1ull << 35)) != 0,
        "SCTLR_EL1.BT0 not set (BTI not enforced at EL1)");
}
