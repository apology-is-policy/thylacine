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
void test_proc_cascading_rfork_wait_smoke(void);
void test_proc_cascading_rfork_stress(void);
void test_proc_orphan_reparent_smoke(void);
void test_namespace_bind_smoke(void);
void test_namespace_cycle_rejected(void);
void test_namespace_fork_isolated(void);
void test_handles_alloc_close_smoke(void);
void test_handles_rights_monotonic(void);
void test_handles_dup_lifecycle(void);
void test_handles_full_table_oom(void);
void test_handles_kind_classifiers(void);
void test_vmo_create_close_round_trip(void);
void test_vmo_refcount_lifecycle(void);
void test_vmo_map_unmap_lifecycle(void);
void test_vmo_handles_x_mappings_matrix(void);
void test_vmo_via_handle_table(void);
void test_vmo_handle_table_orphan_cleanup(void);
void test_vmo_size_overflow_rejected(void);
void test_vmo_dup_oom_rollback(void);
void test_asid_alloc_unique(void);
void test_asid_free_reuses(void);
void test_asid_inflight_count(void);
void test_directmap_kva_round_trip(void);
void test_directmap_alloc_through_directmap(void);
void test_directmap_vmalloc_mmio_smoke(void);
void test_elf_parse_minimal_ok(void);
void test_elf_parse_multi_segment_ok(void);
void test_elf_header_rejection(void);
void test_elf_rwx_rejected(void);
void test_elf_bounds_rejection(void);
void test_elf_policy_rejection(void);

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
    { "proc.cascading_rfork_wait_smoke",
                                       test_proc_cascading_rfork_wait_smoke,
                                                                           false, NULL },
    { "proc.cascading_rfork_stress",   test_proc_cascading_rfork_stress,   false, NULL },
    { "proc.orphan_reparent_smoke",    test_proc_orphan_reparent_smoke,    false, NULL },
    { "namespace.bind_smoke",          test_namespace_bind_smoke,          false, NULL },
    { "namespace.cycle_rejected",      test_namespace_cycle_rejected,      false, NULL },
    { "namespace.fork_isolated",       test_namespace_fork_isolated,       false, NULL },
    { "handles.alloc_close_smoke",     test_handles_alloc_close_smoke,     false, NULL },
    { "handles.rights_monotonic",      test_handles_rights_monotonic,      false, NULL },
    { "handles.dup_lifecycle",         test_handles_dup_lifecycle,         false, NULL },
    { "handles.full_table_oom",        test_handles_full_table_oom,        false, NULL },
    { "handles.kind_classifiers",      test_handles_kind_classifiers,      false, NULL },
    { "vmo.create_close_round_trip",   test_vmo_create_close_round_trip,   false, NULL },
    { "vmo.refcount_lifecycle",        test_vmo_refcount_lifecycle,        false, NULL },
    { "vmo.map_unmap_lifecycle",       test_vmo_map_unmap_lifecycle,       false, NULL },
    { "vmo.handles_x_mappings_matrix", test_vmo_handles_x_mappings_matrix, false, NULL },
    { "vmo.via_handle_table",          test_vmo_via_handle_table,          false, NULL },
    { "vmo.handle_table_orphan_cleanup", test_vmo_handle_table_orphan_cleanup, false, NULL },
    { "vmo.size_overflow_rejected",    test_vmo_size_overflow_rejected,    false, NULL },
    { "vmo.dup_oom_rollback",          test_vmo_dup_oom_rollback,          false, NULL },
    { "asid.alloc_unique",             test_asid_alloc_unique,             false, NULL },
    { "asid.free_reuses",              test_asid_free_reuses,              false, NULL },
    { "asid.inflight_count",           test_asid_inflight_count,           false, NULL },
    { "directmap.kva_round_trip",      test_directmap_kva_round_trip,      false, NULL },
    { "directmap.alloc_through_directmap",
                                       test_directmap_alloc_through_directmap,
                                                                           false, NULL },
    { "directmap.vmalloc_mmio_smoke",  test_directmap_vmalloc_mmio_smoke,  false, NULL },
    { "elf.parse_minimal_ok",          test_elf_parse_minimal_ok,          false, NULL },
    { "elf.parse_multi_segment_ok",    test_elf_parse_multi_segment_ok,    false, NULL },
    { "elf.header_rejection",          test_elf_header_rejection,          false, NULL },
    { "elf.rwx_rejected",              test_elf_rwx_rejected,              false, NULL },
    { "elf.bounds_rejection",          test_elf_bounds_rejection,          false, NULL },
    { "elf.policy_rejection",          test_elf_policy_rejection,          false, NULL },
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
