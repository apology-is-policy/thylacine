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

#include "../../arch/arm64/uart.h"

#include "../../arch/arm64/hwfeat.h"
#include <thylacine/canary.h>
#include <thylacine/smp.h>
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

    // BTI itself is enforced by the GP bit of the kernel-text and user-text
    // mappings; BT0/BT1 are the strictness knobs that stop PACIxSP being a
    // landing pad for a register BR (start.S, Linux's bti_enable setting).
    bool bt_strict = (sctlr & ((1ull << 35) | (1ull << 36))) == ((1ull << 35) | (1ull << 36));
    TEST_ASSERT(!g_hw_features.bti || bt_strict,
        "CPU implements BTI but SCTLR_EL1.BT0/BT1 are not both set (PACIxSP accepts a register BR)");
}

// XT-3a: every online CPU ends bring-up with the SCTLR_EL1 start.S composes
// (sctlr_el1_init_base) plus the enables layered on it, whatever its entry path
// or the platform's reset value, and the register has not changed since. The
// base is restated here rather than shared with start.S, so that changing it
// means changing both, on purpose.
#define SCTLR_COMPOSED_BASE 0x30D40998ull
#define SCTLR_MMU_ON        ((1ull << 0) | (1ull << 2) | (1ull << 12))   // M C I
#define SCTLR_PAC_ENABLES   ((1ull << 31) | (1ull << 30) | (1ull << 27) | (1ull << 13))
#define SCTLR_BT0_BT1       ((1ull << 35) | (1ull << 36))

static void sctlr_report(const char *what, unsigned cpu, u64 v) {
    uart_puts("    sctlr_composed: ");
    uart_puts(what);
    uart_puts(" cpu=");
    uart_putdec(cpu);
    uart_puts(" sctlr_el1=");
    uart_puthex64(v);
    uart_puts("\n");
}

void test_hardening_sctlr_composed(void) {
    unsigned seen = 0;
    bool whole_ok = true;
    for (unsigned cpu = 0; cpu < DTB_MAX_CPUS; cpu++) {
        const struct hw_cpu_ident *id = hw_cpu_ident(cpu);
        if (!id) continue;
        seen++;
        u64 v = id->sctlr_el1;

        // The PAC enables and BT0/BT1 are detect_smoke's to check; every
        // other bit is owned here.
        if ((v & ~(SCTLR_PAC_ENABLES | SCTLR_BT0_BT1)) != (SCTLR_COMPOSED_BASE | SCTLR_MMU_ON)) {
            sctlr_report("not the composed base plus M, C, I:", cpu, v);
            whole_ok = false;
        }

        // The EL0 controls the composition exists for, named so a failure
        // reads as the bit that moved.
        TEST_ASSERT((v & (1ull << 16)) == 0, "EL0 WFI runs untrapped (nTWI set)");
        TEST_ASSERT((v & (1ull << 18)) != 0, "EL0 WFE traps (nTWE clear)");
        TEST_ASSERT((v & (1ull << 9)) == 0, "EL0 may mask interrupts (UMA set)");
        TEST_ASSERT((v & (1ull << 4)) != 0, "EL0 SP alignment unchecked (SA0 clear)");
        TEST_ASSERT((v & (1ull << 26)) == 0, "EL0 may maintain caches (UCI set)");
    }
    TEST_ASSERT(whole_ok, "a CPU's SCTLR_EL1 is not the composed base plus M, C, I");
    TEST_EXPECT_EQ((u64)seen, (u64)smp_cpu_online_count(),
                   "an online CPU recorded no identity, so its SCTLR_EL1 went unchecked");

    // The record is the bring-up value; the live register must still equal it,
    // or something wrote the register after bring-up. IRQs masked so the CPU
    // read and the register read name the same CPU.
    u64 daif, live;
    unsigned self;
    __asm__ __volatile__("mrs %0, daif" : "=r"(daif) :: "memory");
    __asm__ __volatile__("msr daifset, #2" ::: "memory");
    self = smp_cpu_idx_self();
    __asm__ __volatile__("mrs %0, sctlr_el1" : "=r"(live));
    __asm__ __volatile__("msr daif, %0" :: "r"(daif) : "memory");
    const struct hw_cpu_ident *mine = hw_cpu_ident(self);
    TEST_ASSERT(mine != NULL, "the running CPU recorded no identity");
    if (live != mine->sctlr_el1) sctlr_report("live register moved since bring-up:", self, live);
    TEST_EXPECT_EQ(live, mine->sctlr_el1,
                   "this CPU's SCTLR_EL1 changed after bring-up recorded it");
}
