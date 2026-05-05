// SMP bring-up tests (P2-Ca / P2-Cc).
//
// smp.bringup_smoke
//   Verifies smp_init produced a consistent CPU count + every secondary
//   came online. The actual PSCI bring-up happens in main.c before the
//   test harness runs; this test just inspects the state.
//
//   On QEMU virt with -smp 4: dtb_cpu_count() == 4, smp_cpu_count() == 4,
//   smp_cpu_online_count() == 4 (boot + 3 secondaries). g_cpu_online[1..3]
//   all true. Online[0] (boot) also true (set by smp_init unconditionally).
//
// smp.exception_stack_smoke (P2-Cc)
//   Verifies the boot CPU is running in SPSel=0 mode with SP_EL1 set
//   to the top of g_exception_stacks[0]. This confirms the per-CPU
//   exception-stack discipline that start.S installs in the SPSel=0
//   transition (step 4.6).

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

void test_smp_exception_stack_smoke(void) {
    // PSTATE.SPSel must be 0 (kernel mode SP = SP_EL0). start.S
    // _real_start step 4.6 sets this; an exception entry transiently
    // sets SPSel=1 but eret restores from SPSR.M[0]=0 → back to 0.
    // (We're in normal kernel context here, no live exception, so 0.)
    u64 spsel;
    __asm__ __volatile__("mrs %0, SPSel" : "=r"(spsel));
    TEST_EXPECT_EQ(spsel, (u64)0,
        "kernel must run in SPSel=0 mode (SP_EL0 active)");

    // BSS allocation sanity. The compile-time size is fixed by the
    // declaration, but verify the bound is consistent with the per-CPU
    // EXCEPTION_STACK_SIZE × DTB_MAX_CPUS layout. Spot-check that
    // slots are contiguous (no padding inserted by the linker).
    TEST_EXPECT_EQ((u64)sizeof(g_exception_stacks),
                   (u64)(DTB_MAX_CPUS * EXCEPTION_STACK_SIZE),
        "g_exception_stacks BSS size must equal NCPUS * EXCEPTION_STACK_SIZE");
    uintptr_t base = (uintptr_t)&g_exception_stacks[0][0];
    for (unsigned i = 0; i < DTB_MAX_CPUS; i++) {
        uintptr_t slot_base = (uintptr_t)&g_exception_stacks[i][0];
        TEST_EXPECT_EQ((u64)slot_base, (u64)(base + i * EXCEPTION_STACK_SIZE),
            "per-CPU exception stack slot must be contiguous");
    }

    // Runtime evidence: timer_irq_handler captures &local on its
    // first invocation per CPU into g_exception_stack_observed[cpu].
    // For the boot CPU we expect this to fall inside slot 0's range
    // — the SP at handler entry was hardware-switched to SP_EL1 =
    // top of g_exception_stacks[0]. (Direct mrs SP_EL1 from EL1 is
    // UNDEFINED per ARM ARM, so we observe via this hook instead.)
    //
    // Slots [1..N-1] remain zero at v1.0 P2-Cc — secondaries don't
    // unmask IRQs yet (P2-Cd lands their per-CPU GIC + idle threads).
    uintptr_t observed_boot = g_exception_stack_observed[0];
    TEST_ASSERT(observed_boot != 0,
        "timer IRQ on boot CPU must have captured an SP value by now");
    uintptr_t slot0_base = (uintptr_t)&g_exception_stacks[0][0];
    uintptr_t slot0_top  = slot0_base + EXCEPTION_STACK_SIZE;
    TEST_ASSERT(observed_boot >= slot0_base && observed_boot < slot0_top,
        "boot CPU's SP at IRQ entry must fall inside g_exception_stacks[0]");
}
