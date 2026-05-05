// SMP bring-up tests (P2-Ca).
//
// smp.bringup_smoke
//   Verifies smp_init produced a consistent CPU count + every secondary
//   came online. The actual PSCI bring-up happens in main.c before the
//   test harness runs; this test just inspects the state.
//
//   On QEMU virt with -smp 4: dtb_cpu_count() == 4, smp_cpu_count() == 4,
//   smp_cpu_online_count() == 4 (boot + 3 secondaries). g_cpu_online[1..3]
//   all true. Online[0] (boot) also true (set by smp_init unconditionally).

#include "test.h"

#include <thylacine/dtb.h>
#include <thylacine/smp.h>
#include <thylacine/types.h>

void test_smp_bringup_smoke(void) {
    u32 dtb_cpus = dtb_cpu_count();
    TEST_ASSERT(dtb_cpus >= 1,
        "DTB must report at least one CPU (the boot CPU)");
    TEST_ASSERT(dtb_cpus <= DTB_MAX_CPUS,
        "DTB CPU count exceeds DTB_MAX_CPUS — parser bound bug");

    unsigned smp_cpus = smp_cpu_count();
    TEST_EXPECT_EQ((u64)smp_cpus, (u64)dtb_cpus,
        "smp_cpu_count must mirror dtb_cpu_count");

    unsigned online = smp_cpu_online_count();
    TEST_ASSERT(online >= 1,
        "boot CPU must be online");
    TEST_EXPECT_EQ((u64)online, (u64)smp_cpus,
        "all CPUs must be online (PSCI bring-up succeeded)");

    // Boot CPU's online + alive flags set by smp_init.
    TEST_ASSERT(g_cpu_online[0] == 1,
        "g_cpu_online[0] (boot) must be set");
    TEST_ASSERT(g_cpu_alive[0] == 1,
        "g_cpu_alive[0] (boot) must be set");

    // Each secondary's online flag set by its asm trampoline; alive
    // flag set by per_cpu_main at the kernel's high VA after PAC
    // apply + MMU enable + VBAR install + TPIDR set. Both must be
    // observable here for smp_init to have returned successfully.
    for (unsigned i = 1; i < smp_cpus; i++) {
        TEST_ASSERT(g_cpu_online[i] == 1,
            "secondary CPU online flag must be set after smp_init");
        TEST_ASSERT(g_cpu_alive[i] == 1,
            "secondary CPU alive flag (per_cpu_main reached) must be set");
    }

    // Slots beyond cpu_count should remain 0 (BSS default, untouched).
    for (unsigned i = smp_cpus; i < DTB_MAX_CPUS; i++) {
        TEST_EXPECT_EQ((u64)g_cpu_online[i], (u64)0,
            "g_cpu_online beyond cpu_count must remain 0");
        TEST_EXPECT_EQ((u64)g_cpu_alive[i], (u64)0,
            "g_cpu_alive beyond cpu_count must remain 0");
    }
}
