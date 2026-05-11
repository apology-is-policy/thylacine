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
void test_sched_idle_in_wfi_observability(void);
void test_sched_notify_idle_peer_smoke(void);
void test_sched_notify_disabled_no_ipi(void);
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
void test_proc_pgtable_alloc_smoke(void);
void test_proc_pgtable_lifecycle_stress(void);
void test_proc_ttbr0_swap_smoke(void);
void test_proc_pgtable_destroy_walk_releases_subtables(void);
void test_vmo_map_proc_smoke(void);
void test_vmo_map_proc_constraints(void);
void test_vmo_map_proc_overlap_rejected(void);
void test_vmo_unmap_proc_smoke(void);
void test_vmo_unmap_proc_no_match(void);
void test_pgtable_install_user_pte_smoke(void);
void test_pgtable_install_user_pte_constraints(void);
void test_pgtable_install_user_pte_idempotent(void);
void test_demand_page_smoke(void);
void test_demand_page_no_vma(void);
void test_demand_page_permission_denied(void);
void test_demand_page_lifecycle_round_trip(void);
void test_exec_setup_smoke(void);
void test_exec_setup_segment_data_copied(void);
void test_exec_setup_constraints(void);
void test_exec_setup_multi_segment(void);
void test_exec_setup_lifecycle_round_trip(void);
void test_syscall_dispatch_unknown(void);
void test_syscall_dispatch_puts_smoke(void);
void test_syscall_dispatch_exits_ok(void);
void test_syscall_dispatch_exits_fail(void);
void test_syscall_dispatch_args_in_x0_to_x5(void);
void test_fault_decode_kernel_data_translation_l2(void);
void test_fault_decode_kernel_data_permission_write(void);
void test_fault_decode_user_data_translation(void);
void test_fault_decode_user_instruction_fetch(void);
void test_fault_decode_access_flag(void);
void test_vma_alloc_free_smoke(void);
void test_vma_alloc_constraints(void);
void test_vma_insert_lookup_smoke(void);
void test_vma_insert_overlap_rejected(void);
void test_vma_insert_sorted_invariant(void);
void test_vma_drain_releases_all(void);
void test_directmap_kva_round_trip(void);
void test_directmap_alloc_through_directmap(void);
void test_directmap_vmalloc_mmio_smoke(void);
void test_elf_parse_minimal_ok(void);
void test_elf_parse_multi_segment_ok(void);
void test_elf_header_rejection(void);
void test_elf_rwx_rejected(void);
void test_elf_bounds_rejection(void);
void test_elf_policy_rejection(void);
void test_dev_boot_registration_smoke(void);
void test_dev_lookup_unknown(void);
void test_dev_devnone_ops_smoke(void);
void test_spoor_alloc_unref_round_trip(void);
void test_spoor_ref_lifecycle(void);
void test_spoor_clone_lifecycle(void);
void test_spoor_clone_copies_state(void);
void test_spoor_clunk_dispatches_close(void);
void test_spoor_alloc_10k_no_leak(void);
void test_trivial_devs_bestiary_smoke(void);
void test_null_attach_open_close(void);
void test_null_read_returns_eof(void);
void test_null_write_consumes(void);
void test_zero_read_fills_zeroes(void);
void test_zero_write_consumes(void);
void test_random_rndr_available(void);
void test_random_read_produces_nonzero_bytes(void);
void test_random_read_varies_across_calls(void);
void test_cons_write_advances(void);
void test_cons_read_returns_eof(void);
void test_trivial_devs_devnull_10k_no_leak(void);
void test_devproc_bestiary_smoke(void);
void test_devproc_attach_returns_dir(void);
void test_devproc_walk_root_to_kproc_dir(void);
void test_devproc_walk_unknown_pid_misses(void);
void test_devproc_walk_to_status_file(void);
void test_devproc_walk_dotdot_to_root(void);
void test_devproc_read_status_format(void);
void test_devproc_read_cmdline_kproc(void);
void test_devproc_read_ns_format(void);
void test_devproc_read_ctl_returns_zero(void);
void test_devproc_write_ctl_consumes(void);
void test_devproc_read_dir_returns_neg1(void);
void test_devproc_read_partial_offset(void);
void test_devctl_bestiary_smoke(void);
void test_devctl_attach_returns_dir(void);
void test_devctl_walk_to_each_leaf(void);
void test_devctl_walk_unknown_misses(void);
void test_devctl_read_procs_format(void);
void test_devctl_read_memory_format(void);
void test_devctl_read_devices_format(void);
void test_devctl_read_kernel_base_format(void);
void test_devctl_read_sched_format(void);
void test_devctl_write_rejected(void);
void test_devctl_read_dir_returns_neg1(void);
void test_cpio_is_valid_recognizes_magic(void);
void test_cpio_iter_empty_archive(void);
void test_cpio_iter_single_entry(void);
void test_cpio_iter_two_entries(void);
void test_cpio_iter_rejects_truncated(void);
void test_cpio_iter_rejects_bad_magic(void);
void test_cpio_count_matches(void);
void test_devramfs_bestiary_smoke(void);
void test_devramfs_initialized_with_files(void);
void test_devramfs_attach_returns_dir(void);
void test_devramfs_walk_to_welcome(void);
void test_devramfs_walk_unknown_misses(void);
void test_devramfs_read_welcome(void);
void test_devramfs_read_version(void);
void test_devramfs_read_partial_offset(void);
void test_devramfs_read_dir_returns_neg1(void);
void test_devramfs_write_rejected(void);
void test_virtio_mmio_probe(void);
void test_virtio_magic_value(void);
void test_virtio_version_modern(void);
void test_virtio_rng_present(void);
void test_virtio_negotiate_features_smoke(void);
void test_virtio_virtqueue_alloc_destroy(void);
void test_virtio_find_by_device_id(void);
void test_irqfwd_create_destroy(void);
void test_irqfwd_refcount_lifecycle(void);
void test_irqfwd_wait_wakes_on_sgi(void);
void test_irqfwd_collapses_concurrent_fires(void);
void test_virtio_pci_init_called(void);
void test_virtio_pci_count_within_bound(void);
void test_virtio_pci_devices_have_vendor(void);
void test_virtio_pci_devices_have_cfg(void);
void test_virtio_pci_find_rng(void);
void test_virtio_pci_find_unknown_returns_null(void);
void test_virtio_pci_cfg_read_bounds(void);
void test_userspace_first_iteration(void);
void test_userspace_second_iteration(void);
void test_userspace_ramfs_hello(void);
void test_userspace_ramfs_hello_rs(void);
void test_mmio_probe_rfork_with_caps(void);
void test_irq_probe_rfork_with_caps(void);
void test_virtio_blk_probe_rfork_with_caps(void);
void test_caps_kproc_has_all(void);
void test_caps_kproc_has_hw_create(void);
void test_caps_rfork_child_has_none(void);
void test_caps_rfork_with_caps_grants_subset(void);
void test_caps_rfork_with_caps_clamps_to_parent(void);
void test_caps_rfork_with_caps_zero_mask(void);
void test_mmio_handle_create_basic(void);
void test_mmio_handle_create_misaligned_rejected(void);
void test_mmio_handle_create_zero_size_rejected(void);
void test_mmio_handle_create_overflow_rejected(void);
void test_mmio_handle_create_overlap_rejected(void);
void test_mmio_handle_create_adjacent_ok(void);
void test_mmio_handle_create_unref_releases_slot(void);
void test_mmio_handle_double_unref_extincts(void);
void test_mmio_handle_create_kernel_reserved_rejected(void);
void test_mmio_handle_virtio_mmio_claimable(void);
void test_mmio_handle_create_out_of_ips_rejected(void);
void test_dma_handle_create_basic(void);
void test_dma_handle_create_zero_size_rejected(void);
void test_dma_handle_create_oversize_rejected(void);
void test_dma_handle_create_round_up_to_page(void);
void test_dma_handle_distinct_pa(void);
void test_dma_handle_unref_releases_chunk(void);
void test_dma_handle_zero_init(void);
void test_burrow_dma_create_basic(void);
void test_burrow_dma_create_null_rejected(void);
void test_burrow_dma_holds_kobj_ref(void);
void test_burrow_dma_lifecycle_round_trip(void);
void test_dma_map_install_vma(void);
void test_dma_map_proc_free_releases_kobj(void);
void test_handle_hw_mmio_dup_rejected(void);
void test_handle_hw_irq_dup_rejected(void);
void test_handle_hw_mmio_close_releases_claim(void);
void test_handle_hw_irq_close_releases_intid(void);
void test_handle_hw_irq_kernel_reserved_rejected(void);
void test_burrow_mmio_create_basic(void);
void test_burrow_mmio_create_null_rejected(void);
void test_burrow_mmio_create_holds_kobj_ref(void);
void test_burrow_mmio_unref_releases_kobj_ref(void);
void test_burrow_mmio_acquire_mapping_works(void);
void test_burrow_mmio_lifecycle_round_trip(void);
void test_mmio_map_install_vma(void);
void test_mmio_map_overlap_rejected(void);
void test_mmio_map_proc_free_releases_kobj(void);

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
    { "scheduler.idle_in_wfi_observability",
                                       test_sched_idle_in_wfi_observability,
                                                                           false, NULL },
    { "scheduler.notify_idle_peer_smoke",
                                       test_sched_notify_idle_peer_smoke,  false, NULL },
    { "scheduler.notify_disabled_no_ipi",
                                       test_sched_notify_disabled_no_ipi,  false, NULL },
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
    { "territory.bind_smoke",          test_namespace_bind_smoke,          false, NULL },
    { "territory.cycle_rejected",      test_namespace_cycle_rejected,      false, NULL },
    { "territory.fork_isolated",       test_namespace_fork_isolated,       false, NULL },
    { "handles.alloc_close_smoke",     test_handles_alloc_close_smoke,     false, NULL },
    { "handles.rights_monotonic",      test_handles_rights_monotonic,      false, NULL },
    { "handles.dup_lifecycle",         test_handles_dup_lifecycle,         false, NULL },
    { "handles.full_table_oom",        test_handles_full_table_oom,        false, NULL },
    { "handles.kind_classifiers",      test_handles_kind_classifiers,      false, NULL },
    { "burrow.create_close_round_trip",   test_vmo_create_close_round_trip,   false, NULL },
    { "burrow.refcount_lifecycle",        test_vmo_refcount_lifecycle,        false, NULL },
    { "burrow.map_unmap_lifecycle",       test_vmo_map_unmap_lifecycle,       false, NULL },
    { "burrow.handles_x_mappings_matrix", test_vmo_handles_x_mappings_matrix, false, NULL },
    { "burrow.via_handle_table",          test_vmo_via_handle_table,          false, NULL },
    { "burrow.handle_table_orphan_cleanup", test_vmo_handle_table_orphan_cleanup, false, NULL },
    { "burrow.size_overflow_rejected",    test_vmo_size_overflow_rejected,    false, NULL },
    { "burrow.dup_oom_rollback",          test_vmo_dup_oom_rollback,          false, NULL },
    { "asid.alloc_unique",             test_asid_alloc_unique,             false, NULL },
    { "asid.free_reuses",              test_asid_free_reuses,              false, NULL },
    { "asid.inflight_count",           test_asid_inflight_count,           false, NULL },
    { "proc.pgtable_alloc_smoke",      test_proc_pgtable_alloc_smoke,      false, NULL },
    { "proc.pgtable_lifecycle_stress", test_proc_pgtable_lifecycle_stress, false, NULL },
    { "proc.ttbr0_swap_smoke",         test_proc_ttbr0_swap_smoke,         false, NULL },
    { "proc.pgtable_destroy_walk_releases_subtables",
                                       test_proc_pgtable_destroy_walk_releases_subtables,
                                                                           false, NULL },
    { "burrow.map_proc_smoke",            test_vmo_map_proc_smoke,            false, NULL },
    { "burrow.map_proc_constraints",      test_vmo_map_proc_constraints,      false, NULL },
    { "burrow.map_proc_overlap_rejected", test_vmo_map_proc_overlap_rejected, false, NULL },
    { "burrow.unmap_proc_smoke",          test_vmo_unmap_proc_smoke,          false, NULL },
    { "burrow.unmap_proc_no_match",       test_vmo_unmap_proc_no_match,       false, NULL },
    { "pgtable.install_user_pte_smoke",
                                       test_pgtable_install_user_pte_smoke,
                                                                           false, NULL },
    { "pgtable.install_user_pte_constraints",
                                       test_pgtable_install_user_pte_constraints,
                                                                           false, NULL },
    { "pgtable.install_user_pte_idempotent",
                                       test_pgtable_install_user_pte_idempotent,
                                                                           false, NULL },
    { "demand_page.smoke",             test_demand_page_smoke,             false, NULL },
    { "demand_page.no_vma",            test_demand_page_no_vma,            false, NULL },
    { "demand_page.permission_denied", test_demand_page_permission_denied, false, NULL },
    { "demand_page.lifecycle_round_trip",
                                       test_demand_page_lifecycle_round_trip,
                                                                           false, NULL },
    { "exec.setup_smoke",              test_exec_setup_smoke,              false, NULL },
    { "exec.setup_segment_data_copied",
                                       test_exec_setup_segment_data_copied,
                                                                           false, NULL },
    { "exec.setup_constraints",        test_exec_setup_constraints,        false, NULL },
    { "exec.setup_multi_segment",      test_exec_setup_multi_segment,      false, NULL },
    { "exec.setup_lifecycle_round_trip",
                                       test_exec_setup_lifecycle_round_trip,
                                                                           false, NULL },
    { "syscall.dispatch_unknown",      test_syscall_dispatch_unknown,      false, NULL },
    { "syscall.dispatch_puts_smoke",   test_syscall_dispatch_puts_smoke,   false, NULL },
    { "syscall.dispatch_exits_ok",     test_syscall_dispatch_exits_ok,     false, NULL },
    { "syscall.dispatch_exits_fail",   test_syscall_dispatch_exits_fail,   false, NULL },
    { "syscall.dispatch_args_in_x0_to_x5",
                                       test_syscall_dispatch_args_in_x0_to_x5,
                                                                           false, NULL },
    { "fault.decode_kernel_data_translation_l2",
                                       test_fault_decode_kernel_data_translation_l2,
                                                                           false, NULL },
    { "fault.decode_kernel_data_permission_write",
                                       test_fault_decode_kernel_data_permission_write,
                                                                           false, NULL },
    { "fault.decode_user_data_translation",
                                       test_fault_decode_user_data_translation,
                                                                           false, NULL },
    { "fault.decode_user_instruction_fetch",
                                       test_fault_decode_user_instruction_fetch,
                                                                           false, NULL },
    { "fault.decode_access_flag",      test_fault_decode_access_flag,      false, NULL },
    { "vma.alloc_free_smoke",          test_vma_alloc_free_smoke,          false, NULL },
    { "vma.alloc_constraints",         test_vma_alloc_constraints,         false, NULL },
    { "vma.insert_lookup_smoke",       test_vma_insert_lookup_smoke,       false, NULL },
    { "vma.insert_overlap_rejected",   test_vma_insert_overlap_rejected,   false, NULL },
    { "vma.insert_sorted_invariant",   test_vma_insert_sorted_invariant,   false, NULL },
    { "vma.drain_releases_all",        test_vma_drain_releases_all,        false, NULL },
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
    { "dev.boot_registration_smoke",   test_dev_boot_registration_smoke,   false, NULL },
    { "dev.lookup_unknown",            test_dev_lookup_unknown,            false, NULL },
    { "dev.devnone_ops_smoke",         test_dev_devnone_ops_smoke,         false, NULL },
    { "spoor.alloc_unref_round_trip",  test_spoor_alloc_unref_round_trip,  false, NULL },
    { "spoor.ref_lifecycle",           test_spoor_ref_lifecycle,           false, NULL },
    { "spoor.clone_lifecycle",         test_spoor_clone_lifecycle,         false, NULL },
    { "spoor.clone_copies_state",      test_spoor_clone_copies_state,      false, NULL },
    { "spoor.clunk_dispatches_close",  test_spoor_clunk_dispatches_close,  false, NULL },
    { "spoor.alloc_10k_no_leak",       test_spoor_alloc_10k_no_leak,       false, NULL },
    { "trivial_devs.bestiary_smoke",   test_trivial_devs_bestiary_smoke,   false, NULL },
    { "null.attach_open_close",        test_null_attach_open_close,        false, NULL },
    { "null.read_returns_eof",         test_null_read_returns_eof,         false, NULL },
    { "null.write_consumes",           test_null_write_consumes,           false, NULL },
    { "zero.read_fills_zeroes",        test_zero_read_fills_zeroes,        false, NULL },
    { "zero.write_consumes",           test_zero_write_consumes,           false, NULL },
    { "random.rndr_available",         test_random_rndr_available,         false, NULL },
    { "random.read_produces_nonzero",  test_random_read_produces_nonzero_bytes, false, NULL },
    { "random.read_varies",            test_random_read_varies_across_calls, false, NULL },
    { "cons.write_advances",           test_cons_write_advances,           false, NULL },
    { "cons.read_returns_eof",         test_cons_read_returns_eof,         false, NULL },
    { "trivial_devs.devnull_10k_no_leak",
                                       test_trivial_devs_devnull_10k_no_leak,
                                                                           false, NULL },
    { "devproc.bestiary_smoke",        test_devproc_bestiary_smoke,        false, NULL },
    { "devproc.attach_returns_dir",    test_devproc_attach_returns_dir,    false, NULL },
    { "devproc.walk_root_to_kproc_dir",
                                       test_devproc_walk_root_to_kproc_dir, false, NULL },
    { "devproc.walk_unknown_pid_misses",
                                       test_devproc_walk_unknown_pid_misses, false, NULL },
    { "devproc.walk_to_status_file",   test_devproc_walk_to_status_file,   false, NULL },
    { "devproc.walk_dotdot_to_root",   test_devproc_walk_dotdot_to_root,   false, NULL },
    { "devproc.read_status_format",    test_devproc_read_status_format,    false, NULL },
    { "devproc.read_cmdline_kproc",    test_devproc_read_cmdline_kproc,    false, NULL },
    { "devproc.read_ns_format",        test_devproc_read_ns_format,        false, NULL },
    { "devproc.read_ctl_returns_zero", test_devproc_read_ctl_returns_zero, false, NULL },
    { "devproc.write_ctl_consumes",    test_devproc_write_ctl_consumes,    false, NULL },
    { "devproc.read_dir_returns_neg1", test_devproc_read_dir_returns_neg1, false, NULL },
    { "devproc.read_partial_offset",   test_devproc_read_partial_offset,   false, NULL },
    { "devctl.bestiary_smoke",         test_devctl_bestiary_smoke,         false, NULL },
    { "devctl.attach_returns_dir",     test_devctl_attach_returns_dir,     false, NULL },
    { "devctl.walk_to_each_leaf",      test_devctl_walk_to_each_leaf,      false, NULL },
    { "devctl.walk_unknown_misses",    test_devctl_walk_unknown_misses,    false, NULL },
    { "devctl.read_procs_format",      test_devctl_read_procs_format,      false, NULL },
    { "devctl.read_memory_format",     test_devctl_read_memory_format,     false, NULL },
    { "devctl.read_devices_format",    test_devctl_read_devices_format,    false, NULL },
    { "devctl.read_kernel_base_format",
                                       test_devctl_read_kernel_base_format, false, NULL },
    { "devctl.read_sched_format",      test_devctl_read_sched_format,      false, NULL },
    { "devctl.write_rejected",         test_devctl_write_rejected,         false, NULL },
    { "devctl.read_dir_returns_neg1",  test_devctl_read_dir_returns_neg1,  false, NULL },
    { "cpio.is_valid_recognizes_magic",
                                       test_cpio_is_valid_recognizes_magic, false, NULL },
    { "cpio.iter_empty_archive",       test_cpio_iter_empty_archive,       false, NULL },
    { "cpio.iter_single_entry",        test_cpio_iter_single_entry,        false, NULL },
    { "cpio.iter_two_entries",         test_cpio_iter_two_entries,         false, NULL },
    { "cpio.iter_rejects_truncated",   test_cpio_iter_rejects_truncated,   false, NULL },
    { "cpio.iter_rejects_bad_magic",   test_cpio_iter_rejects_bad_magic,   false, NULL },
    { "cpio.count_matches",            test_cpio_count_matches,            false, NULL },
    { "devramfs.bestiary_smoke",       test_devramfs_bestiary_smoke,       false, NULL },
    { "devramfs.initialized_with_files",
                                       test_devramfs_initialized_with_files, false, NULL },
    { "devramfs.attach_returns_dir",   test_devramfs_attach_returns_dir,   false, NULL },
    { "devramfs.walk_to_welcome",      test_devramfs_walk_to_welcome,      false, NULL },
    { "devramfs.walk_unknown_misses",  test_devramfs_walk_unknown_misses,  false, NULL },
    { "devramfs.read_welcome",         test_devramfs_read_welcome,         false, NULL },
    { "devramfs.read_version",         test_devramfs_read_version,         false, NULL },
    { "devramfs.read_partial_offset",  test_devramfs_read_partial_offset,  false, NULL },
    { "devramfs.read_dir_returns_neg1",
                                       test_devramfs_read_dir_returns_neg1, false, NULL },
    { "devramfs.write_rejected",       test_devramfs_write_rejected,       false, NULL },
    { "virtio.mmio_probe",             test_virtio_mmio_probe,             false, NULL },
    { "virtio.magic_value",            test_virtio_magic_value,            false, NULL },
    { "virtio.version_modern",         test_virtio_version_modern,         false, NULL },
    { "virtio.rng_present",            test_virtio_rng_present,            false, NULL },
    { "virtio.negotiate_features_smoke",
                                       test_virtio_negotiate_features_smoke, false, NULL },
    { "virtio.virtqueue_alloc_destroy",
                                       test_virtio_virtqueue_alloc_destroy, false, NULL },
    { "virtio.find_by_device_id",      test_virtio_find_by_device_id,      false, NULL },
    { "irqfwd.create_destroy",         test_irqfwd_create_destroy,         false, NULL },
    { "irqfwd.refcount_lifecycle",     test_irqfwd_refcount_lifecycle,     false, NULL },
    { "irqfwd.wait_wakes_on_sgi",      test_irqfwd_wait_wakes_on_sgi,      false, NULL },
    { "irqfwd.collapses_concurrent_fires",
                                       test_irqfwd_collapses_concurrent_fires, false, NULL },
    { "virtio_pci.init_called",        test_virtio_pci_init_called,        false, NULL },
    { "virtio_pci.count_within_bound", test_virtio_pci_count_within_bound, false, NULL },
    { "virtio_pci.devices_have_vendor",
                                       test_virtio_pci_devices_have_vendor, false, NULL },
    { "virtio_pci.devices_have_cfg",   test_virtio_pci_devices_have_cfg,   false, NULL },
    { "virtio_pci.find_rng",           test_virtio_pci_find_rng,           false, NULL },
    { "virtio_pci.find_unknown_returns_null",
                                       test_virtio_pci_find_unknown_returns_null, false, NULL },
    { "virtio_pci.cfg_read_bounds",    test_virtio_pci_cfg_read_bounds,    false, NULL },
    { "userspace.first_iteration",     test_userspace_first_iteration,     false, NULL },
    { "userspace.second_iteration",    test_userspace_second_iteration,    false, NULL },
    { "userspace.ramfs_hello",         test_userspace_ramfs_hello,         false, NULL },
    { "userspace.ramfs_hello_rs",      test_userspace_ramfs_hello_rs,      false, NULL },
    { "userspace.mmio_probe_rfork_with_caps",
                                       test_mmio_probe_rfork_with_caps,    false, NULL },
    { "userspace.irq_probe_rfork_with_caps",
                                       test_irq_probe_rfork_with_caps,     false, NULL },
    { "userspace.virtio_blk_probe_rfork_with_caps",
                                       test_virtio_blk_probe_rfork_with_caps,
                                                                           false, NULL },
    { "caps.kproc_has_all",            test_caps_kproc_has_all,            false, NULL },
    { "caps.kproc_has_hw_create",      test_caps_kproc_has_hw_create,      false, NULL },
    { "caps.rfork_child_has_none",     test_caps_rfork_child_has_none,     false, NULL },
    { "caps.rfork_with_caps_grants_subset",
                                       test_caps_rfork_with_caps_grants_subset,
                                                                           false, NULL },
    { "caps.rfork_with_caps_clamps_to_parent",
                                       test_caps_rfork_with_caps_clamps_to_parent,
                                                                           false, NULL },
    { "caps.rfork_with_caps_zero_mask",
                                       test_caps_rfork_with_caps_zero_mask,
                                                                           false, NULL },
    { "mmio_handle.create_basic",      test_mmio_handle_create_basic,      false, NULL },
    { "mmio_handle.create_misaligned_rejected",
                                       test_mmio_handle_create_misaligned_rejected,
                                                                           false, NULL },
    { "mmio_handle.create_zero_size_rejected",
                                       test_mmio_handle_create_zero_size_rejected,
                                                                           false, NULL },
    { "mmio_handle.create_overflow_rejected",
                                       test_mmio_handle_create_overflow_rejected,
                                                                           false, NULL },
    { "mmio_handle.create_overlap_rejected",
                                       test_mmio_handle_create_overlap_rejected,
                                                                           false, NULL },
    { "mmio_handle.create_adjacent_ok",
                                       test_mmio_handle_create_adjacent_ok,
                                                                           false, NULL },
    { "mmio_handle.create_unref_releases_slot",
                                       test_mmio_handle_create_unref_releases_slot,
                                                                           false, NULL },
    { "mmio_handle.live_count_round_trip",
                                       test_mmio_handle_double_unref_extincts,
                                                                           false, NULL },
    { "mmio_handle.kernel_reserved_rejected",
                                       test_mmio_handle_create_kernel_reserved_rejected,
                                                                           false, NULL },
    { "mmio_handle.virtio_mmio_claimable",
                                       test_mmio_handle_virtio_mmio_claimable,
                                                                           false, NULL },
    { "mmio_handle.out_of_ips_rejected",
                                       test_mmio_handle_create_out_of_ips_rejected,
                                                                           false, NULL },
    { "handle_hw.mmio_dup_rejected",   test_handle_hw_mmio_dup_rejected,   false, NULL },
    { "handle_hw.irq_dup_rejected",    test_handle_hw_irq_dup_rejected,    false, NULL },
    { "handle_hw.mmio_close_releases_claim",
                                       test_handle_hw_mmio_close_releases_claim,
                                                                           false, NULL },
    { "handle_hw.irq_close_releases_intid",
                                       test_handle_hw_irq_close_releases_intid,
                                                                           false, NULL },
    { "handle_hw.irq_kernel_reserved_rejected",
                                       test_handle_hw_irq_kernel_reserved_rejected,
                                                                           false, NULL },
    { "burrow_mmio.create_basic",      test_burrow_mmio_create_basic,      false, NULL },
    { "burrow_mmio.create_null_rejected",
                                       test_burrow_mmio_create_null_rejected, false, NULL },
    { "burrow_mmio.create_holds_kobj_ref",
                                       test_burrow_mmio_create_holds_kobj_ref, false, NULL },
    { "burrow_mmio.unref_releases_kobj_ref",
                                       test_burrow_mmio_unref_releases_kobj_ref, false, NULL },
    { "burrow_mmio.acquire_mapping_works",
                                       test_burrow_mmio_acquire_mapping_works, false, NULL },
    { "burrow_mmio.lifecycle_round_trip",
                                       test_burrow_mmio_lifecycle_round_trip, false, NULL },
    { "mmio_map.install_vma",          test_mmio_map_install_vma,          false, NULL },
    { "mmio_map.overlap_rejected",     test_mmio_map_overlap_rejected,     false, NULL },
    { "mmio_map.proc_free_releases_kobj",
                                       test_mmio_map_proc_free_releases_kobj, false, NULL },
    { "dma_handle.create_basic",       test_dma_handle_create_basic,       false, NULL },
    { "dma_handle.zero_size_rejected", test_dma_handle_create_zero_size_rejected,
                                                                           false, NULL },
    { "dma_handle.oversize_rejected",  test_dma_handle_create_oversize_rejected,
                                                                           false, NULL },
    { "dma_handle.round_up_to_page",   test_dma_handle_create_round_up_to_page,
                                                                           false, NULL },
    { "dma_handle.distinct_pa",        test_dma_handle_distinct_pa,        false, NULL },
    { "dma_handle.unref_releases_chunk",
                                       test_dma_handle_unref_releases_chunk,
                                                                           false, NULL },
    { "dma_handle.zero_init",          test_dma_handle_zero_init,          false, NULL },
    { "burrow_dma.create_basic",       test_burrow_dma_create_basic,       false, NULL },
    { "burrow_dma.create_null_rejected",
                                       test_burrow_dma_create_null_rejected, false, NULL },
    { "burrow_dma.holds_kobj_ref",     test_burrow_dma_holds_kobj_ref,     false, NULL },
    { "burrow_dma.lifecycle_round_trip",
                                       test_burrow_dma_lifecycle_round_trip,
                                                                           false, NULL },
    { "dma_map.install_vma",           test_dma_map_install_vma,           false, NULL },
    { "dma_map.proc_free_releases_kobj",
                                       test_dma_map_proc_free_releases_kobj,
                                                                           false, NULL },
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
