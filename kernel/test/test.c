// In-kernel test harness — runner + registry.
//
// The g_tests[] array is the single registration site. New tests
// are added by:
//   1. Writing a `void test_<name>(void)` function in some
//      kernel/test/test_<subsystem>.c file.
//   2. Adding a `{ "<name>", test_<name>, false, NULL }` entry to
//      g_tests[] below, before the sentinel.
//   3. Declaring `void test_<name>(void)` somewhere reachable
//      (we forward-declare them all here; cleaner than per-test
//      headers).
//
// The harness is freestanding-friendly: no constructors, no linker
// sections, no host runtime. Reports each test's outcome on UART
// before continuing to the next.

#include "test.h"

#include "../../arch/arm64/uart.h"

#include <thylacine/types.h>

// ---------------------------------------------------------------------------
// Forward declarations of every test. Bodies live in kernel/test/test_*.c.
// ---------------------------------------------------------------------------

void test_kaslr_mix64_avalanche(void);
void test_dtb_chosen_kaslr_seed_present(void);
void test_phys_alloc_smoke(void);
void test_phys_leak_10k(void);
void test_slub_kmem_smoke(void);
void test_slub_leak_10k(void);
void test_gic_init_smoke(void);
void test_timer_tick_increments(void);
void test_hardening_detect_smoke(void);
void test_context_create_destroy(void);
void test_context_round_trip(void);
void test_sched_dispatch_smoke(void);
void test_sched_runnable_count(void);
void test_sched_preemption_smoke(void);
void test_rendez_sleep_immediate_cond_true(void);
void test_rendez_basic_handoff(void);
void test_rendez_wakeup_no_waiter(void);
void test_smp_bringup_smoke(void);
void test_smp_exception_stack_smoke(void);
void test_smp_per_cpu_idle_smoke(void);
void test_smp_ipi_resched_smoke(void);
void test_smp_work_stealing_smoke(void);
void test_proc_rfork_basic_smoke(void);
void test_proc_rfork_exits_status(void);
void test_proc_rfork_stress_1000(void);

// ---------------------------------------------------------------------------
// Registry. Sentinel-terminated.
// ---------------------------------------------------------------------------

struct test_case g_tests[] = {
    { "kaslr.mix64_avalanche",         test_kaslr_mix64_avalanche,         false, NULL },
    { "dtb.chosen_kaslr_seed_present", test_dtb_chosen_kaslr_seed_present, false, NULL },
    { "phys.alloc_smoke",              test_phys_alloc_smoke,              false, NULL },
    { "phys.leak_10k",                 test_phys_leak_10k,                 false, NULL },
    { "slub.kmem_smoke",               test_slub_kmem_smoke,               false, NULL },
    { "slub.leak_10k",                 test_slub_leak_10k,                 false, NULL },
    { "gic.init_smoke",                test_gic_init_smoke,                false, NULL },
    { "timer.tick_increments",         test_timer_tick_increments,         false, NULL },
    { "hardening.detect_smoke",        test_hardening_detect_smoke,        false, NULL },
    { "context.create_destroy",        test_context_create_destroy,        false, NULL },
    { "context.round_trip",            test_context_round_trip,            false, NULL },
    { "scheduler.dispatch_smoke",      test_sched_dispatch_smoke,          false, NULL },
    { "scheduler.runnable_count",      test_sched_runnable_count,          false, NULL },
    { "scheduler.preemption_smoke",    test_sched_preemption_smoke,        false, NULL },
    { "rendez.sleep_immediate_cond_true",
                                       test_rendez_sleep_immediate_cond_true,
                                                                           false, NULL },
    { "rendez.basic_handoff",          test_rendez_basic_handoff,          false, NULL },
    { "rendez.wakeup_no_waiter",       test_rendez_wakeup_no_waiter,       false, NULL },
    { "smp.bringup_smoke",             test_smp_bringup_smoke,             false, NULL },
    { "smp.exception_stack_smoke",     test_smp_exception_stack_smoke,     false, NULL },
    { "smp.per_cpu_idle_smoke",        test_smp_per_cpu_idle_smoke,        false, NULL },
    { "smp.ipi_resched_smoke",         test_smp_ipi_resched_smoke,         false, NULL },
    { "smp.work_stealing_smoke",       test_smp_work_stealing_smoke,       false, NULL },
    { "proc.rfork_basic_smoke",        test_proc_rfork_basic_smoke,        false, NULL },
    { "proc.rfork_exits_status",       test_proc_rfork_exits_status,       false, NULL },
    { "proc.rfork_stress_1000",        test_proc_rfork_stress_1000,        false, NULL },
    { NULL, NULL, false, NULL },          // sentinel
};

// ---------------------------------------------------------------------------
// Runner.
// ---------------------------------------------------------------------------

static struct test_case *current_test;
static unsigned passed_count, failed_count, total_count;

void test_fail(const char *msg) {
    if (current_test) {
        current_test->failed = true;
        current_test->fail_msg = msg;
    }
}

void test_run_all(void) {
    passed_count = 0;
    failed_count = 0;
    total_count  = 0;

    for (int i = 0; g_tests[i].fn != NULL; i++) {
        current_test = &g_tests[i];
        current_test->failed = false;
        current_test->fail_msg = NULL;
        total_count++;

        uart_puts("    [test] ");
        uart_puts(current_test->name);
        uart_puts(" ... ");

        current_test->fn();

        if (current_test->failed) {
            uart_puts("FAIL: ");
            uart_puts(current_test->fail_msg ? current_test->fail_msg : "(no message)");
            uart_puts("\n");
            failed_count++;
        } else {
            uart_puts("PASS\n");
            passed_count++;
        }
    }

    current_test = NULL;
}

bool test_all_passed(void) {
    return failed_count == 0;
}

unsigned test_total(void)  { return total_count; }
unsigned test_passed(void) { return passed_count; }
unsigned test_failed(void) { return failed_count; }
