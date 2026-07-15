// test_hwdebug.c — Go IDE Stage 8a-2a: the hardware-debug bring-up + verify
// primitives (arch/arm64/hwdebug.c). These are EL1-testable: the DFR0
// enumeration + the DBGB*/MDSCR register arm/disarm round-trip (proving the
// guest CAN program the debug registers on this substrate). The end-to-end
// DELIVERY proof — that an armed EL0 breakpoint's EC 0x30 actually reaches EL1
// — needs a real EL0 execution, which only the in-guest /hwbp-verify probe can
// produce (a kernel test runs at EL1 with no EL0-scheduling loop). This file is
// the EL1 half; the probe is the EL0 half.

#include "test.h"

#include "../include/thylacine/types.h"
#include "../../arch/arm64/hwdebug.h"
#include "../../arch/arm64/hwfeat.h"

void test_hwdebug_dfr0_enumerate(void);
void test_hwdebug_arm_disarm_roundtrip(void);

// DFR0 enumeration: every ARMv8 debug implementation has at least 2 breakpoints
// and 2 watchpoints (architectural minimum). hw_features_detect already ran at
// boot; re-run to be self-contained.
void test_hwdebug_dfr0_enumerate(void) {
    hwdebug_enumerate();
    TEST_ASSERT(g_hw_features.num_brps >= 2,
                "DFR0 reports >= 2 hardware breakpoints (architectural minimum)");
    TEST_ASSERT(g_hw_features.num_wrps >= 2,
                "DFR0 reports >= 2 hardware watchpoints (architectural minimum)");
}

#define MDSCR_MDE_BIT   (1ull << 15)
#define DBGBCR_E_BIT    (1ull << 0)
#define DBGBCR_EL0_BP   0x1E5ull

// Arm bp0 at a test VA, confirm the DBGBVR0/DBGBCR0/MDSCR.MDE writes stuck (the
// guest can program the debug registers), that a concurrent arm is refused, and
// that disarm clears them. Uses a low, unmapped user VA: PMC=EL0 means the
// breakpoint fires only at EL0, and no EL0 thread runs during the boot test
// phase, so arming it at EL1 here is inert. Read the registers into locals +
// DISARM before any assert (the cleanup-before-assert discipline: a
// TEST_ASSERT returns on failure, and a lingering armed bp0 + MDE must never
// escape the test).
void test_hwdebug_arm_disarm_roundtrip(void) {
    const u64 TEST_VA = 0x100000ull;   // 1 MiB, 4-byte aligned, user-half, unmapped

    bool armed = hwdebug_verify_arm(0x7fffffff, TEST_VA);   // a fake pid

    u64  bvr = 0, bcr = 0, mdscr = 0;
    bool second_refused = false;
    if (armed) {
        __asm__ __volatile__("mrs %0, dbgbvr0_el1" : "=r"(bvr));
        __asm__ __volatile__("mrs %0, dbgbcr0_el1" : "=r"(bcr));
        __asm__ __volatile__("mrs %0, mdscr_el1"   : "=r"(mdscr));
        // A second arm while one is live is refused (one verify at a time).
        second_refused = !hwdebug_verify_arm(0x7ffffffe, TEST_VA);
    }

    // Always disarm (safe if not armed) BEFORE the asserts.
    hwdebug_verify_disarm();
    u64 bcr_after = 0, mdscr_after = 0;
    __asm__ __volatile__("mrs %0, dbgbcr0_el1" : "=r"(bcr_after));
    __asm__ __volatile__("mrs %0, mdscr_el1"   : "=r"(mdscr_after));

    TEST_ASSERT(armed, "hwdebug_verify_arm succeeds when nothing is armed");
    // DBGBVR0 holds the 4-byte-aligned VA in its low (VA-size) bits; the RESS
    // high bits are sign-extended from bit 48 (0 for a low user VA).
    TEST_EXPECT_EQ(bvr & ((1ull << 48) - 1), TEST_VA, "DBGBVR0 == the armed VA");
    TEST_EXPECT_EQ(bcr & DBGBCR_EL0_BP, DBGBCR_EL0_BP,
                   "DBGBCR0 has E=1, PMC=0b10 (EL0), BAS=0xF");
    TEST_ASSERT((mdscr & MDSCR_MDE_BIT) != 0, "MDSCR.MDE set while armed");
    TEST_ASSERT(second_refused, "a concurrent arm is refused (one verify at a time)");
    TEST_ASSERT((bcr_after & DBGBCR_E_BIT) == 0, "DBGBCR0.E cleared after disarm");
    TEST_ASSERT((mdscr_after & MDSCR_MDE_BIT) == 0, "MDSCR.MDE cleared after disarm");
}
