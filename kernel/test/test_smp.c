// SMP bring-up tests (P2-Ca / P2-Cc / P2-Cd).
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
//
// smp.per_cpu_idle_smoke (P2-Cd)
//   Verifies each CPU has its sched state initialized + an idle thread
//   recorded. CPU 0's idle is `kthread`; CPUs 1..N-1's idles are
//   allocated by per_cpu_main via thread_init_per_cpu_idle. All idles
//   share kproc as their proc.
//
// smp.ipi_resched_smoke (P2-Cdc)
//   Boot CPU sends IPI_RESCHED to each secondary; waits for each
//   secondary's g_ipi_resched_count slot to increment from its
//   pre-send baseline. Verifies cross-CPU SGI delivery: the GIC
//   distributor + each secondary's redistributor + CPU interface
//   are correctly bringing IPI_RESCHED through to the IPI handler.
//
// smp.work_stealing_smoke (P2-Ce)
//   Boot CPU creates N threads (N = cpu count) that loop incrementing
//   a per-CPU counter g_steal_test_count[smp_cpu_idx_self()] and yield.
//   Boot ready()s all on its own tree, then sends IPI_RESCHED to each
//   secondary. The IPI wakes each secondary's WFI; the post-WFI sched()
//   in per_cpu_main's idle loop finds the secondary's tree empty and
//   try_steal()s a thread from boot's tree. The stolen thread runs on
//   the secondary, incrementing the counter for that CPU's slot.
//   Verifies that at least one secondary observed the steal (i.e.,
//   g_steal_test_count[i] > 0 for some i >= 1).

#include "test.h"

#include "../../arch/arm64/gic.h"
#include "../../arch/arm64/timer.h"

#include <thylacine/dtb.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>
#include <thylacine/smp.h>
#include <thylacine/thread.h>
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

void test_smp_per_cpu_idle_smoke(void) {
    unsigned cpus = smp_cpu_count();
    TEST_ASSERT(cpus >= 1, "smp_cpu_count is positive");

    // Boot CPU's idle is kthread (set by thread_init + sched_init(0)).
    struct Thread *idle0 = sched_idle_thread(0);
    TEST_ASSERT(idle0 != NULL, "boot CPU's sched_idle_thread is non-NULL");
    TEST_ASSERT(idle0 == kthread(),
        "boot CPU's idle thread must equal kthread");
    TEST_ASSERT(idle0->magic == THREAD_MAGIC,
        "boot CPU idle has correct magic");
    TEST_ASSERT(idle0->proc == kproc(),
        "boot CPU idle's proc is kproc");

    // Secondaries: each per_cpu_main allocates a fresh idle Thread via
    // thread_init_per_cpu_idle and registers it with sched_init(idx).
    // After smp_init returns, all sched_idle_thread(1..N-1) are non-NULL.
    for (unsigned i = 1; i < cpus; i++) {
        struct Thread *t = sched_idle_thread(i);
        TEST_ASSERT(t != NULL, "secondary's sched_idle_thread is non-NULL");
        TEST_ASSERT(t != kthread(),
            "secondary's idle thread must NOT be kthread (per-CPU)");
        TEST_ASSERT(t->magic == THREAD_MAGIC,
            "secondary's idle has correct magic");
        TEST_ASSERT(t->proc == kproc(),
            "secondary's idle's proc is kproc");
        TEST_ASSERT(t->state == THREAD_RUNNING,
            "secondary's idle is RUNNING (it IS the executing thread on its CPU)");
        TEST_ASSERT(t->band == SCHED_BAND_IDLE,
            "secondary's idle is in SCHED_BAND_IDLE");
    }

    // Slots beyond cpu_count return NULL.
    for (unsigned i = cpus; i < DTB_MAX_CPUS; i++) {
        TEST_ASSERT(sched_idle_thread(i) == NULL,
            "sched_idle_thread beyond cpu_count is NULL");
    }
}

void test_smp_ipi_resched_smoke(void) {
    unsigned cpus = smp_cpu_count();
    if (cpus < 2) {
        // No secondaries available — skip but don't fail. Boot CPU
        // can't send an IPI to itself meaningfully via SGI 1R (Linux
        // does sometimes for parking, but the semantics aren't what
        // we want here).
        return;
    }

    // Snapshot pre-send counts. Each secondary may have already
    // received some IPIs during bring-up if anyone (e.g., a future
    // sub-chunk) sent them; the test verifies post-send > pre-send,
    // not absolute count.
    u64 baseline[DTB_MAX_CPUS];
    for (unsigned i = 0; i < DTB_MAX_CPUS; i++) {
        baseline[i] = g_ipi_resched_count[i];
    }

    // Send IPI_RESCHED to each secondary.
    for (unsigned i = 1; i < cpus; i++) {
        TEST_ASSERT(gic_send_ipi(i, IPI_RESCHED),
            "gic_send_ipi must accept valid target + SGI INTID");
    }

    // Wait for each secondary's count to increment. Per ARM IHI 0069
    // §4.5 SGI delivery is microsecond-fast on QEMU virt; bound the
    // poll at 100 ticks (100 ms) per secondary to allow generous
    // headroom for emulator latency.
    for (unsigned i = 1; i < cpus; i++) {
        u64 deadline = timer_get_ticks() + 100;
        while (g_ipi_resched_count[i] <= baseline[i]) {
            if (timer_get_ticks() > deadline) {
                TEST_ASSERT(false,
                    "IPI_RESCHED to secondary CPU never observed (handler not invoked?)");
            }
            // No WFI — IRQs masked at PSTATE here would block our own
            // wake; the boot CPU is still running with IRQs enabled
            // (msr daifclr #2 in main.c) so timer ticks fire and
            // timer_get_ticks() advances. dmb is enough to re-fetch
            // the volatile.
            __asm__ __volatile__("dmb ish" ::: "memory");
        }
        TEST_ASSERT(g_ipi_resched_count[i] > baseline[i],
            "IPI_RESCHED handler must have incremented secondary's count");
    }

    // Boot CPU's slot should be unchanged — we don't send to ourselves.
    TEST_EXPECT_EQ(g_ipi_resched_count[0], baseline[0],
        "boot CPU's IPI count is unchanged (we did not target ourselves)");
}

static volatile bool g_steal_test_run;
static volatile u64  g_steal_test_count[DTB_MAX_CPUS];
static struct Rendez g_steal_test_rendez[DTB_MAX_CPUS];
static struct Thread *g_steal_test_threads[DTB_MAX_CPUS];

// Cond function for the per-thread "sleep forever" — never returns
// true, so sleep loops indefinitely (= the thread stays SLEEPING).
static int never_true(void *arg) { (void)arg; return 0; }

static void steal_test_entry(void) {
    while (g_steal_test_run) {
        unsigned cpu = smp_cpu_idx_self();
        if (cpu < DTB_MAX_CPUS) g_steal_test_count[cpu]++;
        sched();
    }
    // Exit signal observed. Find OUR slot in the threads array
    // (entry has no arg in the v1.0 thread_create API; we match by
    // current_thread()) and sleep on its dedicated per-thread Rendez.
    // We never get woken — the test wants us to settle into SLEEPING
    // so thread_free can safely reap us. Since each Rendez has a
    // single waiter (Plan 9 convention), one Rendez per thread is
    // required.
    struct Thread *me = current_thread();
    int slot = -1;
    for (unsigned i = 0; i < DTB_MAX_CPUS; i++) {
        if (g_steal_test_threads[i] == me) { slot = (int)i; break; }
    }
    if (slot < 0) {
        // Defensive: shouldn't happen if the test set up threads correctly.
        for (;;) __asm__ __volatile__("wfe");
    }
    sleep(&g_steal_test_rendez[slot], never_true, NULL);
    // Unreachable — sleep loops on cond which is always false.
}

void test_smp_work_stealing_smoke(void) {
    unsigned cpus = smp_cpu_count();
    if (cpus < 2) {
        // No secondaries — nothing to steal to. Skip rather than fail.
        return;
    }

    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run trees must be empty at test entry");

    g_steal_test_run = true;
    for (unsigned i = 0; i < DTB_MAX_CPUS; i++) {
        g_steal_test_count[i] = 0;
        g_steal_test_rendez[i] = (struct Rendez)RENDEZ_INIT;
        g_steal_test_threads[i] = NULL;
    }

    // Create one thread per CPU; ready() each on boot's tree (ready()
    // inserts into THIS CPU's tree at v1.0 P2-Ce). Secondaries will
    // steal these from boot's tree once their WFI wakes via IPI.
    for (unsigned i = 0; i < cpus; i++) {
        g_steal_test_threads[i] = thread_create(kproc(), steal_test_entry);
        TEST_ASSERT(g_steal_test_threads[i] != NULL, "thread_create failed");
        ready(g_steal_test_threads[i]);
    }

    // Send IPI_RESCHED to each secondary to wake their WFI. The
    // post-WFI sched() in per_cpu_main's idle loop is what triggers
    // the steal.
    for (unsigned i = 1; i < cpus; i++) {
        TEST_ASSERT(gic_send_ipi(i, IPI_RESCHED),
            "gic_send_ipi must accept valid target + SGI");
    }

    // Wait window. With 4 CPUs all receiving threads, the test threads
    // hammer their local counters under timer-IRQ-driven preemption
    // on boot + idle-loop sched on secondaries (after the IPI wake).
    // 50 ticks is generous (50 ms at 1000 Hz).
    timer_busy_wait_ticks(50);

    // Signal exit. Threads observe on their next iteration.
    g_steal_test_run = false;

    // Allow drain.
    timer_busy_wait_ticks(20);

    // Verify at least one secondary ran a thread — the smoke that
    // proves the steal path works.
    bool any_secondary_ran = false;
    for (unsigned i = 1; i < cpus; i++) {
        if (g_steal_test_count[i] > 0) {
            any_secondary_ran = true;
            break;
        }
    }
    TEST_ASSERT(any_secondary_ran,
        "no secondary CPU observed work-stealing (try_steal not pulling?)");

    // Cleanup. After exit signal, each thread sleeps on its dedicated
    // Rendez (state SLEEPING, not in any run tree). thread_free
    // accepts SLEEPING threads. The Rendez waiter slot still points
    // at the thread but the test never wakeup()s — no dangling deref
    // since the Rendez memory is also static-test-scope and unused
    // after the test returns.
    for (unsigned i = 0; i < cpus; i++) {
        thread_free(g_steal_test_threads[i]);
        g_steal_test_threads[i] = NULL;
    }
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run trees empty after thread_free");
}
