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
void test_fp_cpacr_enabled(void);
void test_fp_round_trip_v_regs_fpsr_fpcr(void);
void test_sched_dispatch_smoke(void);
void test_sched_runnable_count(void);
void test_sched_preemption_smoke(void);
void test_sched_idle_in_wfi_observability(void);
void test_sched_notify_idle_peer_smoke(void);
void test_sched_notify_disabled_no_ipi(void);
void test_rendez_sleep_immediate_cond_true(void);
void test_rendez_basic_handoff(void);
void test_rendez_wakeup_no_waiter(void);
void test_tsleep_fast_path_cond_true(void);
void test_tsleep_no_deadline_degrades(void);
void test_tsleep_past_deadline_immediate(void);
void test_tsleep_woken_before_deadline(void);
void test_tsleep_timeout_via_tick(void);
void test_tsleep_herd_timeout(void);
void test_smp_bringup_smoke(void);
void test_smp_exception_stack_smoke(void);
void test_smp_per_cpu_idle_smoke(void);
void test_smp_ipi_resched_smoke(void);
void test_smp_work_stealing_smoke(void);
void test_smp_secondary_stack_guard_layout(void);
void test_proc_rfork_basic_smoke(void);
void test_proc_rfork_exits_status(void);
void test_proc_rfork_stress_1000(void);
void test_proc_cascading_rfork_wait_smoke(void);
void test_proc_cascading_rfork_stress(void);
void test_proc_orphan_reparent_smoke(void);
void test_proc_console_attached_smoke(void);
void test_proc_stripes_smoke(void);
void test_namespace_bind_smoke(void);
void test_namespace_cycle_rejected(void);
void test_namespace_fork_isolated(void);
void test_territory_mount_smoke(void);
void test_territory_mount_idempotent_same_source(void);
void test_territory_mount_mrepl_replaces(void);
void test_territory_mount_unmount_missing_returns_error(void);
void test_territory_mount_table_full(void);
void test_territory_mount_clone_bumps_refs(void);
void test_territory_mount_destroy_drops_all_refs(void);
void test_handles_alloc_close_smoke(void);
void test_handles_rights_monotonic(void);
void test_handles_dup_lifecycle(void);
void test_handles_full_table_oom(void);
void test_handles_kind_classifiers(void);
void test_handles_srv_kind(void);
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
void test_vmo_map_proc_user_va_top_boundary(void);
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
void test_exec_user_stack_guard(void);
void test_syscall_dispatch_unknown(void);
void test_syscall_dispatch_puts_smoke(void);
void test_syscall_dispatch_exits_ok(void);
void test_syscall_dispatch_exits_fail(void);
void test_syscall_dispatch_args_in_x0_to_x5(void);
void test_uaccess_fixup_table_well_formed(void);
void test_uaccess_fixup_lookup_known(void);
void test_uaccess_fixup_lookup_unknown_returns_zero(void);
void test_uaccess_load_u8_unmapped_user_va_returns_minus1(void);
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
void test_dev_vtable_slot_coverage(void);
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
void test_devsrv_registered(void);
void test_devsrv_post_gate(void);
void test_devsrv_post_basic(void);
void test_devsrv_tombstone(void);
void test_devsrv_registry_full(void);
void test_devsrv_post_rollback(void);
void test_srvconn_create_destroy(void);
void test_srvconn_roundtrip(void);
void test_srvconn_ring_capacity(void);
void test_srvconn_recv_blocks_then_wakes(void);
void test_srvconn_recv_deadline_timeout(void);
void test_srvconn_teardown_eofs(void);
void test_srvconn_teardown_wakes_blocked(void);
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
void test_virtio_blk_rw_rfork_with_caps(void);
void test_virtio_net_probe_rfork_with_caps(void);
void test_virtio_net_arp_rfork_with_caps(void);
void test_virtio_net_loop_rfork_with_caps(void);
void test_virtio_input_probe_rfork_with_caps(void);
void test_virtio_gpu_probe_rfork_with_caps(void);
void test_driver_crash_recovery(void);
void test_9p_wire_primitives_round_trip(void);
void test_9p_wire_primitives_overflow(void);
void test_9p_wire_str_round_trip(void);
void test_9p_wire_qid_round_trip(void);
void test_9p_wire_header_peek(void);
void test_9p_wire_tversion_round_trip(void);
void test_9p_wire_tattach_round_trip(void);
void test_9p_wire_twalk_round_trip(void);
void test_9p_wire_twalk_zero_names_clone(void);
void test_9p_wire_tclunk_round_trip(void);
void test_9p_wire_rlerror_parse(void);
void test_9p_wire_rmsg_size_mismatch_rejected(void);
void test_9p_wire_rmsg_wrong_type_rejected(void);
void test_9p_wire_rwalk_count_cap_enforced(void);
void test_9p_wire_pack_str_overflow(void);
void test_9p_wire_tlopen_round_trip(void);
void test_9p_wire_tlcreate_round_trip(void);
void test_9p_wire_tread_round_trip(void);
void test_9p_wire_twrite_round_trip(void);
void test_9p_wire_rread_data_cap_enforced(void);
void test_9p_wire_rread_size_mismatch_rejected(void);
void test_9p_wire_rlopen_vs_rlcreate_type_strict(void);
void test_9p_wire_tgetattr_round_trip(void);
void test_9p_wire_tsetattr_round_trip(void);
void test_9p_wire_treaddir_round_trip(void);
void test_9p_wire_dirent_unpack(void);
void test_9p_wire_tstatfs_round_trip(void);
void test_9p_wire_tfsync_round_trip(void);
void test_9p_wire_rreaddir_data_cap_enforced(void);
void test_9p_wire_tsymlink_round_trip(void);
void test_9p_wire_tmknod_round_trip(void);
void test_9p_wire_trename_round_trip(void);
void test_9p_wire_treadlink_round_trip(void);
void test_9p_wire_tlink_round_trip(void);
void test_9p_wire_tmkdir_round_trip(void);
void test_9p_wire_trenameat_round_trip(void);
void test_9p_wire_tunlinkat_round_trip(void);
void test_9p_session_init_destroy(void);
void test_9p_session_version_handshake(void);
void test_9p_session_attach_handshake(void);
void test_9p_session_walk_round_trip(void);
void test_9p_session_clunk_round_trip(void);
void test_9p_session_clunk_send_time_unbinds(void);
void test_9p_session_dispatch_rlerror(void);
void test_9p_session_walk_to_root_refused(void);
void test_9p_session_walk_to_bound_fid_refused(void);
void test_9p_session_walk_from_unbound_fid_refused(void);
void test_9p_session_clunk_root_refused(void);
void test_9p_session_clunk_with_inflight_on_fid_refused(void);
void test_9p_session_fid_after_clunk_refused(void);
void test_9p_session_dispatch_wrong_tag_rejected(void);
void test_9p_session_dispatch_wrong_type_rejected(void);
void test_9p_session_close_with_inflight_refused(void);
void test_9p_session_state_gate_send_walk_before_open(void);
void test_9p_session_lopen_round_trip(void);
void test_9p_session_lcreate_round_trip(void);
void test_9p_session_read_round_trip(void);
void test_9p_session_write_round_trip(void);
void test_9p_session_lopen_with_inflight_on_fid_refused(void);
void test_9p_session_read_permits_concurrent(void);
void test_9p_session_io_from_unbound_fid_refused(void);
void test_9p_session_io_before_open_refused(void);
void test_9p_session_getattr_round_trip(void);
void test_9p_session_setattr_round_trip(void);
void test_9p_session_readdir_round_trip(void);
void test_9p_session_statfs_round_trip(void);
void test_9p_session_fsync_round_trip(void);
void test_9p_session_setattr_with_inflight_on_fid_refused(void);
void test_9p_session_getattr_permits_concurrent(void);
void test_9p_session_meta_from_unbound_fid_refused(void);
void test_9p_session_symlink_round_trip(void);
void test_9p_session_mknod_round_trip(void);
void test_9p_session_rename_round_trip(void);
void test_9p_session_readlink_round_trip(void);
void test_9p_session_link_round_trip(void);
void test_9p_session_mkdir_round_trip(void);
void test_9p_session_renameat_round_trip(void);
void test_9p_session_unlinkat_round_trip(void);
void test_9p_session_rename_with_inflight_on_fid_refused(void);
void test_9p_session_unlinkat_permits_concurrent(void);
void test_9p_session_mutation_from_unbound_fid_refused(void);
void test_9p_transport_init_destroy(void);
void test_9p_transport_round_trip(void);
void test_9p_transport_send_frame_size_mismatch_rejected(void);
void test_9p_transport_recv_frame_too_large_rejected(void);
void test_9p_transport_partial_read_aggregation(void);
void test_9p_transport_backend_error_transitions_to_error(void);
void test_9p_transport_close_idempotent(void);
void test_9p_transport_exchange_drives_session_handshake(void);
void test_9p_transport_exchange_drives_session_walk(void);
void test_9p_client_init_destroy(void);
void test_9p_client_handshake(void);
void test_9p_client_walk_and_clunk(void);
void test_9p_client_lopen_read(void);
void test_9p_client_write(void);
void test_9p_client_getattr(void);
void test_9p_client_readdir(void);
void test_9p_client_statfs(void);
void test_9p_client_mkdir(void);
void test_9p_client_unlinkat(void);
void test_9p_client_readlink(void);
void test_9p_client_rlerror_propagates_to_negative_errno(void);
void test_9p_client_op_before_handshake_returns_ebusy(void);
void test_9p_client_lock_released_between_ops(void);
void test_dev9p_registered(void);
void test_dev9p_attach_client_root_spoor(void);
void test_dev9p_walk_one_component(void);
void test_dev9p_walk_clone(void);
void test_dev9p_open_lopens_fid(void);
void test_dev9p_read_routes_through_client(void);
void test_dev9p_write_routes_through_client(void);
void test_dev9p_close_clunks_owned_fid(void);
void test_dev9p_close_does_not_clunk_root_fid(void);
void test_p9_attached_create_destroy(void);
void test_p9_attached_handshake_failure_returns_null(void);
void test_p9_attached_root_spoor_walk_read(void);
void test_p9_attached_query_helpers(void);
void test_spoor_transport_init_destroy(void);
void test_spoor_transport_init_null_rejected(void);
void test_spoor_transport_send_routes_to_tx_dev_write(void);
void test_spoor_transport_recv_routes_to_rx_dev_read(void);
void test_spoor_transport_recv_empty_returns_zero(void);
void test_spoor_transport_close_clunks_when_owned(void);
void test_spoor_transport_close_preserves_when_unowned(void);
void test_spoor_transport_transport_core_round_trip(void);
void test_spoor_transport_end_to_end_handshake(void);
void test_pipe_smoke(void);
void test_pipe_read_on_empty_returns_zero(void);
void test_pipe_write_to_full_returns_zero(void);
void test_pipe_write_short_when_partially_full(void);
void test_pipe_wraparound(void);
void test_pipe_read_on_write_end_rejected(void);
void test_pipe_write_on_read_end_rejected(void);
void test_pipe_close_one_end_keeps_other_alive(void);
void test_pipe_close_both_ends_frees_ring(void);
void test_pipe_compose_with_spoor_transport(void);
void test_pipe_blocking_write_wakes_sleeping_reader(void);
void test_pipe_blocking_read_wakes_sleeping_writer(void);
void test_pipe_blocking_close_write_end_wakes_reader_with_eof(void);
void test_pipe_blocking_close_read_end_wakes_writer_with_epipe(void);
void test_sys_pipe_allocates_two_distinct_spoor_handles(void);
void test_sys_pipe_proc_free_releases_handles(void);
void test_sys_pipe_handle_close_releases_one_end(void);
void test_sys_rw_write_then_read_round_trip(void);
void test_sys_rw_rights_check(void);
void test_sys_rw_zero_length_validates_fd(void);
void test_sys_rw_read_after_close_returns_eof(void);
void test_sys_pipe_dup_spoor_handle_acquires_ref(void);
void test_pipe_probe_round_trip(void);
void test_sys_attach_9p_rejection_paths(void);
void test_sys_mount_happy_path_grafts_pipe_spoor(void);
void test_sys_mount_idempotent_on_duplicate(void);
void test_sys_mount_rejects_bad_fd(void);
void test_sys_mount_rejects_missing_right_read(void);
void test_sys_mount_rejects_invalid_flags(void);
void test_sys_mount_rejects_null_territory(void);
void test_sys_unmount_removes_entry_and_drops_ref(void);
void test_sys_unmount_rejects_nonexistent_target(void);
void test_sys_mount_caller_close_keeps_mount_alive(void);
void test_attach_probe_round_trip(void);
void test_sys_mlockall_cap_gate(void);
void test_sys_set_dumpable_one_way_to_zero(void);
void test_sys_set_traceable_one_way_to_zero(void);
void test_sys_corvus_caps_kproc_has_new_caps(void);
void test_kern_random_seeded_returns_true_on_qemu(void);
void test_kern_random_bytes_produces_nonzero(void);
void test_sys_spawn_happy_path(void);
void test_sys_spawn_rejects_null_name(void);
void test_sys_spawn_rejects_zero_len(void);
void test_sys_spawn_rejects_oversize_name(void);
void test_sys_spawn_rejects_missing_binary(void);
void test_sys_spawn_rejects_embedded_nul(void);
void test_sys_wait_pid_no_children_returns_neg1(void);
void test_sys_spawn_with_fds_rejects_oversize_fd_count(void);
void test_sys_spawn_with_fds_rejects_bad_fd(void);
void test_sys_spawn_with_fds_rejects_non_spoor_fd(void);
void test_sys_spawn_with_fds_rejects_missing_binary(void);
void test_sys_spawn_with_fds_zero_count_succeeds(void);
void test_sys_spawn_with_fds_child_rights_subset_of_parent(void);
void test_sys_spawn_with_caps_happy_path_zero_mask(void);
void test_sys_spawn_with_caps_happy_path_subset_of_parent(void);
void test_sys_spawn_with_caps_clamps_to_parent(void);
void test_sys_spawn_with_caps_rejects_missing_binary(void);
void test_sys_spawn_with_caps_rejects_oversize_name(void);
void test_sys_spawn_full_happy_path_fds_and_caps(void);
void test_sys_spawn_full_zero_count_zero_mask_succeeds(void);
void test_sys_spawn_full_rejects_oversize_fd_count(void);
void test_sys_spawn_full_rejects_bad_fd(void);
void test_sys_spawn_full_rejects_missing_binary(void);
void test_stratumd_stub_round_trip(void);
void test_stub_driver_round_trip(void);
void test_irq_latency_bench(void);
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
void test_handle_hw_irq_out_of_range_rejected(void);
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
    { "fp.cpacr_enabled",              test_fp_cpacr_enabled,              false, NULL },
    { "fp.round_trip_v_regs_fpsr_fpcr",
                                       test_fp_round_trip_v_regs_fpsr_fpcr, false, NULL },
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
    { "tsleep.fast_path_cond_true",
                                       test_tsleep_fast_path_cond_true,
                                                                           false, NULL },
    { "tsleep.no_deadline_degrades",
                                       test_tsleep_no_deadline_degrades,
                                                                           false, NULL },
    { "tsleep.past_deadline_immediate",
                                       test_tsleep_past_deadline_immediate,
                                                                           false, NULL },
    { "tsleep.woken_before_deadline",
                                       test_tsleep_woken_before_deadline,
                                                                           false, NULL },
    { "tsleep.timeout_via_tick",
                                       test_tsleep_timeout_via_tick,
                                                                           false, NULL },
    { "tsleep.herd_timeout",
                                       test_tsleep_herd_timeout,
                                                                           false, NULL },
    { "smp.bringup_smoke",             test_smp_bringup_smoke,             false, NULL },
    { "smp.exception_stack_smoke",     test_smp_exception_stack_smoke,     false, NULL },
    { "smp.per_cpu_idle_smoke",        test_smp_per_cpu_idle_smoke,        false, NULL },
    { "smp.ipi_resched_smoke",         test_smp_ipi_resched_smoke,         false, NULL },
    { "smp.work_stealing_smoke",       test_smp_work_stealing_smoke,       false, NULL },
    { "smp.secondary_stack_guard_layout",
                                       test_smp_secondary_stack_guard_layout,
                                                                           false, NULL },
    { "proc.rfork_basic_smoke",        test_proc_rfork_basic_smoke,        false, NULL },
    { "proc.rfork_exits_status",       test_proc_rfork_exits_status,       false, NULL },
    { "proc.rfork_stress_1000",        test_proc_rfork_stress_1000,        false, NULL },
    { "proc.cascading_rfork_wait_smoke",
                                       test_proc_cascading_rfork_wait_smoke,
                                                                           false, NULL },
    { "proc.cascading_rfork_stress",   test_proc_cascading_rfork_stress,   false, NULL },
    { "proc.orphan_reparent_smoke",    test_proc_orphan_reparent_smoke,    false, NULL },
    { "proc.console_attached_smoke",   test_proc_console_attached_smoke,   false, NULL },
    { "proc.stripes_smoke",            test_proc_stripes_smoke,            false, NULL },
    { "territory.bind_smoke",          test_namespace_bind_smoke,          false, NULL },
    { "territory.cycle_rejected",      test_namespace_cycle_rejected,      false, NULL },
    { "territory.fork_isolated",       test_namespace_fork_isolated,       false, NULL },
    { "territory_mount.smoke",                            test_territory_mount_smoke,                            false, NULL },
    { "territory_mount.idempotent_same_source",           test_territory_mount_idempotent_same_source,           false, NULL },
    { "territory_mount.mrepl_replaces",                   test_territory_mount_mrepl_replaces,                   false, NULL },
    { "territory_mount.unmount_missing_returns_error",    test_territory_mount_unmount_missing_returns_error,    false, NULL },
    { "territory_mount.table_full",                       test_territory_mount_table_full,                       false, NULL },
    { "territory_mount.clone_bumps_refs",                 test_territory_mount_clone_bumps_refs,                 false, NULL },
    { "territory_mount.destroy_drops_all_refs",           test_territory_mount_destroy_drops_all_refs,           false, NULL },
    { "handles.alloc_close_smoke",     test_handles_alloc_close_smoke,     false, NULL },
    { "handles.rights_monotonic",      test_handles_rights_monotonic,      false, NULL },
    { "handles.dup_lifecycle",         test_handles_dup_lifecycle,         false, NULL },
    { "handles.full_table_oom",        test_handles_full_table_oom,        false, NULL },
    { "handles.kind_classifiers",      test_handles_kind_classifiers,      false, NULL },
    { "handles.srv_kind",              test_handles_srv_kind,              false, NULL },
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
    { "burrow.map_proc_user_va_top_boundary",
                                          test_vmo_map_proc_user_va_top_boundary,
                                                                              false, NULL },
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
    { "exec.user_stack_guard",         test_exec_user_stack_guard,         false, NULL },
    { "syscall.dispatch_unknown",      test_syscall_dispatch_unknown,      false, NULL },
    { "syscall.dispatch_puts_smoke",   test_syscall_dispatch_puts_smoke,   false, NULL },
    { "syscall.dispatch_exits_ok",     test_syscall_dispatch_exits_ok,     false, NULL },
    { "syscall.dispatch_exits_fail",   test_syscall_dispatch_exits_fail,   false, NULL },
    { "syscall.dispatch_args_in_x0_to_x5",
                                       test_syscall_dispatch_args_in_x0_to_x5,
                                                                           false, NULL },
    { "uaccess.fixup_table_well_formed",
                                       test_uaccess_fixup_table_well_formed,
                                                                           false, NULL },
    { "uaccess.fixup_lookup_known",    test_uaccess_fixup_lookup_known,    false, NULL },
    { "uaccess.fixup_lookup_unknown_returns_zero",
                                       test_uaccess_fixup_lookup_unknown_returns_zero,
                                                                           false, NULL },
    { "uaccess.load_u8_unmapped_user_va_returns_minus1",
                                       test_uaccess_load_u8_unmapped_user_va_returns_minus1,
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
    { "dev.vtable_slot_coverage",      test_dev_vtable_slot_coverage,      false, NULL },
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
    { "devsrv.registered",             test_devsrv_registered,             false, NULL },
    { "devsrv.post_gate",              test_devsrv_post_gate,              false, NULL },
    { "devsrv.post_basic",             test_devsrv_post_basic,             false, NULL },
    { "devsrv.tombstone",              test_devsrv_tombstone,              false, NULL },
    { "devsrv.registry_full",          test_devsrv_registry_full,          false, NULL },
    { "devsrv.post_rollback",          test_devsrv_post_rollback,          false, NULL },
    { "srvconn.create_destroy",        test_srvconn_create_destroy,        false, NULL },
    { "srvconn.roundtrip",             test_srvconn_roundtrip,             false, NULL },
    { "srvconn.ring_capacity",         test_srvconn_ring_capacity,         false, NULL },
    { "srvconn.recv_blocks_then_wakes",
                                       test_srvconn_recv_blocks_then_wakes,
                                                                           false, NULL },
    { "srvconn.recv_deadline_timeout",
                                       test_srvconn_recv_deadline_timeout, false, NULL },
    { "srvconn.teardown_eofs",         test_srvconn_teardown_eofs,         false, NULL },
    { "srvconn.teardown_wakes_blocked",
                                       test_srvconn_teardown_wakes_blocked,
                                                                           false, NULL },
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
    { "userspace.virtio_blk_rw_rfork_with_caps",
                                       test_virtio_blk_rw_rfork_with_caps, false, NULL },
    { "userspace.virtio_net_probe_rfork_with_caps",
                                       test_virtio_net_probe_rfork_with_caps,
                                                                           false, NULL },
    { "userspace.virtio_net_arp_rfork_with_caps",
                                       test_virtio_net_arp_rfork_with_caps,
                                                                           false, NULL },
    { "userspace.virtio_net_loop_rfork_with_caps",
                                       test_virtio_net_loop_rfork_with_caps,
                                                                           false, NULL },
    { "userspace.virtio_input_probe_rfork_with_caps",
                                       test_virtio_input_probe_rfork_with_caps,
                                                                           false, NULL },
    { "userspace.virtio_gpu_probe_rfork_with_caps",
                                       test_virtio_gpu_probe_rfork_with_caps,
                                                                           false, NULL },
    { "userspace.driver_crash_recovery",
                                       test_driver_crash_recovery,         false, NULL },
    { "9p_wire.primitives_round_trip", test_9p_wire_primitives_round_trip, false, NULL },
    { "9p_wire.primitives_overflow",   test_9p_wire_primitives_overflow,   false, NULL },
    { "9p_wire.str_round_trip",        test_9p_wire_str_round_trip,        false, NULL },
    { "9p_wire.pack_str_overflow",     test_9p_wire_pack_str_overflow,     false, NULL },
    { "9p_wire.qid_round_trip",        test_9p_wire_qid_round_trip,        false, NULL },
    { "9p_wire.header_peek",           test_9p_wire_header_peek,           false, NULL },
    { "9p_wire.tversion_round_trip",   test_9p_wire_tversion_round_trip,   false, NULL },
    { "9p_wire.tattach_round_trip",    test_9p_wire_tattach_round_trip,    false, NULL },
    { "9p_wire.twalk_round_trip",      test_9p_wire_twalk_round_trip,      false, NULL },
    { "9p_wire.twalk_zero_names_clone",test_9p_wire_twalk_zero_names_clone,false, NULL },
    { "9p_wire.tclunk_round_trip",     test_9p_wire_tclunk_round_trip,     false, NULL },
    { "9p_wire.rlerror_parse",         test_9p_wire_rlerror_parse,         false, NULL },
    { "9p_wire.rmsg_size_mismatch_rejected",
                                       test_9p_wire_rmsg_size_mismatch_rejected,
                                                                           false, NULL },
    { "9p_wire.rmsg_wrong_type_rejected",
                                       test_9p_wire_rmsg_wrong_type_rejected,
                                                                           false, NULL },
    { "9p_wire.rwalk_count_cap_enforced",
                                       test_9p_wire_rwalk_count_cap_enforced,
                                                                           false, NULL },
    { "9p_wire.tlopen_round_trip",     test_9p_wire_tlopen_round_trip,     false, NULL },
    { "9p_wire.tlcreate_round_trip",   test_9p_wire_tlcreate_round_trip,   false, NULL },
    { "9p_wire.tread_round_trip",      test_9p_wire_tread_round_trip,      false, NULL },
    { "9p_wire.twrite_round_trip",     test_9p_wire_twrite_round_trip,     false, NULL },
    { "9p_wire.rread_data_cap_enforced",
                                       test_9p_wire_rread_data_cap_enforced,
                                                                           false, NULL },
    { "9p_wire.rread_size_mismatch_rejected",
                                       test_9p_wire_rread_size_mismatch_rejected,
                                                                           false, NULL },
    { "9p_wire.rlopen_vs_rlcreate_type_strict",
                                       test_9p_wire_rlopen_vs_rlcreate_type_strict,
                                                                           false, NULL },
    { "9p_wire.tgetattr_round_trip",   test_9p_wire_tgetattr_round_trip,   false, NULL },
    { "9p_wire.tsetattr_round_trip",   test_9p_wire_tsetattr_round_trip,   false, NULL },
    { "9p_wire.treaddir_round_trip",   test_9p_wire_treaddir_round_trip,   false, NULL },
    { "9p_wire.dirent_unpack",         test_9p_wire_dirent_unpack,         false, NULL },
    { "9p_wire.tstatfs_round_trip",    test_9p_wire_tstatfs_round_trip,    false, NULL },
    { "9p_wire.tfsync_round_trip",     test_9p_wire_tfsync_round_trip,     false, NULL },
    { "9p_wire.rreaddir_data_cap_enforced",
                                       test_9p_wire_rreaddir_data_cap_enforced,
                                                                           false, NULL },
    { "9p_wire.tsymlink_round_trip",   test_9p_wire_tsymlink_round_trip,   false, NULL },
    { "9p_wire.tmknod_round_trip",     test_9p_wire_tmknod_round_trip,     false, NULL },
    { "9p_wire.trename_round_trip",    test_9p_wire_trename_round_trip,    false, NULL },
    { "9p_wire.treadlink_round_trip",  test_9p_wire_treadlink_round_trip,  false, NULL },
    { "9p_wire.tlink_round_trip",      test_9p_wire_tlink_round_trip,      false, NULL },
    { "9p_wire.tmkdir_round_trip",     test_9p_wire_tmkdir_round_trip,     false, NULL },
    { "9p_wire.trenameat_round_trip",  test_9p_wire_trenameat_round_trip,  false, NULL },
    { "9p_wire.tunlinkat_round_trip",  test_9p_wire_tunlinkat_round_trip,  false, NULL },
    { "9p_session.init_destroy",       test_9p_session_init_destroy,       false, NULL },
    { "9p_session.version_handshake",  test_9p_session_version_handshake,  false, NULL },
    { "9p_session.attach_handshake",   test_9p_session_attach_handshake,   false, NULL },
    { "9p_session.walk_round_trip",    test_9p_session_walk_round_trip,    false, NULL },
    { "9p_session.clunk_round_trip",   test_9p_session_clunk_round_trip,   false, NULL },
    { "9p_session.clunk_send_time_unbinds",
                                       test_9p_session_clunk_send_time_unbinds,
                                                                           false, NULL },
    { "9p_session.dispatch_rlerror",   test_9p_session_dispatch_rlerror,   false, NULL },
    { "9p_session.walk_to_root_refused",
                                       test_9p_session_walk_to_root_refused,
                                                                           false, NULL },
    { "9p_session.walk_to_bound_fid_refused",
                                       test_9p_session_walk_to_bound_fid_refused,
                                                                           false, NULL },
    { "9p_session.walk_from_unbound_fid_refused",
                                       test_9p_session_walk_from_unbound_fid_refused,
                                                                           false, NULL },
    { "9p_session.clunk_root_refused", test_9p_session_clunk_root_refused, false, NULL },
    { "9p_session.clunk_with_inflight_on_fid_refused",
                                       test_9p_session_clunk_with_inflight_on_fid_refused,
                                                                           false, NULL },
    { "9p_session.fid_after_clunk_refused",
                                       test_9p_session_fid_after_clunk_refused,
                                                                           false, NULL },
    { "9p_session.dispatch_wrong_tag_rejected",
                                       test_9p_session_dispatch_wrong_tag_rejected,
                                                                           false, NULL },
    { "9p_session.dispatch_wrong_type_rejected",
                                       test_9p_session_dispatch_wrong_type_rejected,
                                                                           false, NULL },
    { "9p_session.close_with_inflight_refused",
                                       test_9p_session_close_with_inflight_refused,
                                                                           false, NULL },
    { "9p_session.state_gate_send_walk_before_open",
                                       test_9p_session_state_gate_send_walk_before_open,
                                                                           false, NULL },
    { "9p_session.lopen_round_trip",   test_9p_session_lopen_round_trip,   false, NULL },
    { "9p_session.lcreate_round_trip", test_9p_session_lcreate_round_trip, false, NULL },
    { "9p_session.read_round_trip",    test_9p_session_read_round_trip,    false, NULL },
    { "9p_session.write_round_trip",   test_9p_session_write_round_trip,   false, NULL },
    { "9p_session.lopen_with_inflight_on_fid_refused",
                                       test_9p_session_lopen_with_inflight_on_fid_refused,
                                                                           false, NULL },
    { "9p_session.read_permits_concurrent",
                                       test_9p_session_read_permits_concurrent,
                                                                           false, NULL },
    { "9p_session.io_from_unbound_fid_refused",
                                       test_9p_session_io_from_unbound_fid_refused,
                                                                           false, NULL },
    { "9p_session.io_before_open_refused",
                                       test_9p_session_io_before_open_refused,
                                                                           false, NULL },
    { "9p_session.getattr_round_trip", test_9p_session_getattr_round_trip, false, NULL },
    { "9p_session.setattr_round_trip", test_9p_session_setattr_round_trip, false, NULL },
    { "9p_session.readdir_round_trip", test_9p_session_readdir_round_trip, false, NULL },
    { "9p_session.statfs_round_trip",  test_9p_session_statfs_round_trip,  false, NULL },
    { "9p_session.fsync_round_trip",   test_9p_session_fsync_round_trip,   false, NULL },
    { "9p_session.setattr_with_inflight_on_fid_refused",
                                       test_9p_session_setattr_with_inflight_on_fid_refused,
                                                                           false, NULL },
    { "9p_session.getattr_permits_concurrent",
                                       test_9p_session_getattr_permits_concurrent,
                                                                           false, NULL },
    { "9p_session.meta_from_unbound_fid_refused",
                                       test_9p_session_meta_from_unbound_fid_refused,
                                                                           false, NULL },
    { "9p_session.symlink_round_trip", test_9p_session_symlink_round_trip, false, NULL },
    { "9p_session.mknod_round_trip",   test_9p_session_mknod_round_trip,   false, NULL },
    { "9p_session.rename_round_trip",  test_9p_session_rename_round_trip,  false, NULL },
    { "9p_session.readlink_round_trip",test_9p_session_readlink_round_trip,false, NULL },
    { "9p_session.link_round_trip",    test_9p_session_link_round_trip,    false, NULL },
    { "9p_session.mkdir_round_trip",   test_9p_session_mkdir_round_trip,   false, NULL },
    { "9p_session.renameat_round_trip",test_9p_session_renameat_round_trip,false, NULL },
    { "9p_session.unlinkat_round_trip",test_9p_session_unlinkat_round_trip,false, NULL },
    { "9p_session.rename_with_inflight_on_fid_refused",
                                       test_9p_session_rename_with_inflight_on_fid_refused,
                                                                           false, NULL },
    { "9p_session.unlinkat_permits_concurrent",
                                       test_9p_session_unlinkat_permits_concurrent,
                                                                           false, NULL },
    { "9p_session.mutation_from_unbound_fid_refused",
                                       test_9p_session_mutation_from_unbound_fid_refused,
                                                                           false, NULL },
    { "9p_transport.init_destroy",     test_9p_transport_init_destroy,     false, NULL },
    { "9p_transport.round_trip",       test_9p_transport_round_trip,       false, NULL },
    { "9p_transport.send_frame_size_mismatch_rejected",
                                       test_9p_transport_send_frame_size_mismatch_rejected,
                                                                           false, NULL },
    { "9p_transport.recv_frame_too_large_rejected",
                                       test_9p_transport_recv_frame_too_large_rejected,
                                                                           false, NULL },
    { "9p_transport.partial_read_aggregation",
                                       test_9p_transport_partial_read_aggregation,
                                                                           false, NULL },
    { "9p_transport.backend_error_transitions_to_error",
                                       test_9p_transport_backend_error_transitions_to_error,
                                                                           false, NULL },
    { "9p_transport.close_idempotent", test_9p_transport_close_idempotent, false, NULL },
    { "9p_transport.exchange_drives_session_handshake",
                                       test_9p_transport_exchange_drives_session_handshake,
                                                                           false, NULL },
    { "9p_transport.exchange_drives_session_walk",
                                       test_9p_transport_exchange_drives_session_walk,
                                                                           false, NULL },
    { "9p_client.init_destroy",        test_9p_client_init_destroy,        false, NULL },
    { "9p_client.handshake",           test_9p_client_handshake,           false, NULL },
    { "9p_client.walk_and_clunk",      test_9p_client_walk_and_clunk,      false, NULL },
    { "9p_client.lopen_read",          test_9p_client_lopen_read,          false, NULL },
    { "9p_client.write",               test_9p_client_write,               false, NULL },
    { "9p_client.getattr",             test_9p_client_getattr,             false, NULL },
    { "9p_client.readdir",             test_9p_client_readdir,             false, NULL },
    { "9p_client.statfs",              test_9p_client_statfs,              false, NULL },
    { "9p_client.mkdir",               test_9p_client_mkdir,               false, NULL },
    { "9p_client.unlinkat",            test_9p_client_unlinkat,            false, NULL },
    { "9p_client.readlink",            test_9p_client_readlink,            false, NULL },
    { "9p_client.rlerror_propagates_to_negative_errno",
                                       test_9p_client_rlerror_propagates_to_negative_errno,
                                                                           false, NULL },
    { "9p_client.op_before_handshake_returns_ebusy",
                                       test_9p_client_op_before_handshake_returns_ebusy,
                                                                           false, NULL },
    { "9p_client.lock_released_between_ops",
                                       test_9p_client_lock_released_between_ops,
                                                                           false, NULL },
    { "dev9p.registered",              test_dev9p_registered,              false, NULL },
    { "dev9p.attach_client_root_spoor",test_dev9p_attach_client_root_spoor,false, NULL },
    { "dev9p.walk_one_component",      test_dev9p_walk_one_component,      false, NULL },
    { "dev9p.walk_clone",              test_dev9p_walk_clone,              false, NULL },
    { "dev9p.open_lopens_fid",         test_dev9p_open_lopens_fid,         false, NULL },
    { "dev9p.read_routes_through_client",
                                       test_dev9p_read_routes_through_client,
                                                                           false, NULL },
    { "dev9p.write_routes_through_client",
                                       test_dev9p_write_routes_through_client,
                                                                           false, NULL },
    { "dev9p.close_clunks_owned_fid",  test_dev9p_close_clunks_owned_fid,  false, NULL },
    { "dev9p.close_does_not_clunk_root_fid",
                                       test_dev9p_close_does_not_clunk_root_fid,
                                                                           false, NULL },
    { "p9_attached.create_destroy",    test_p9_attached_create_destroy,    false, NULL },
    { "p9_attached.handshake_failure_returns_null",
                                       test_p9_attached_handshake_failure_returns_null,
                                                                           false, NULL },
    { "p9_attached.root_spoor_walk_read",
                                       test_p9_attached_root_spoor_walk_read,
                                                                           false, NULL },
    { "p9_attached.query_helpers",     test_p9_attached_query_helpers,     false, NULL },
    { "spoor_transport.init_destroy",                       test_spoor_transport_init_destroy,                       false, NULL },
    { "spoor_transport.init_null_rejected",                 test_spoor_transport_init_null_rejected,                 false, NULL },
    { "spoor_transport.send_routes_to_tx_dev_write",        test_spoor_transport_send_routes_to_tx_dev_write,        false, NULL },
    { "spoor_transport.recv_routes_to_rx_dev_read",         test_spoor_transport_recv_routes_to_rx_dev_read,         false, NULL },
    { "spoor_transport.recv_empty_returns_zero",            test_spoor_transport_recv_empty_returns_zero,            false, NULL },
    { "spoor_transport.close_clunks_when_owned",            test_spoor_transport_close_clunks_when_owned,            false, NULL },
    { "spoor_transport.close_preserves_when_unowned",       test_spoor_transport_close_preserves_when_unowned,       false, NULL },
    { "spoor_transport.transport_core_round_trip",          test_spoor_transport_transport_core_round_trip,          false, NULL },
    { "spoor_transport.end_to_end_handshake",               test_spoor_transport_end_to_end_handshake,               false, NULL },
    { "pipe.smoke",                                         test_pipe_smoke,                                         false, NULL },
    { "pipe.read_on_empty_returns_zero",                    test_pipe_read_on_empty_returns_zero,                    false, NULL },
    { "pipe.write_to_full_returns_zero",                    test_pipe_write_to_full_returns_zero,                    false, NULL },
    { "pipe.write_short_when_partially_full",               test_pipe_write_short_when_partially_full,               false, NULL },
    { "pipe.wraparound",                                    test_pipe_wraparound,                                    false, NULL },
    { "pipe.read_on_write_end_rejected",                    test_pipe_read_on_write_end_rejected,                    false, NULL },
    { "pipe.write_on_read_end_rejected",                    test_pipe_write_on_read_end_rejected,                    false, NULL },
    { "pipe.close_one_end_keeps_other_alive",               test_pipe_close_one_end_keeps_other_alive,               false, NULL },
    { "pipe.close_both_ends_frees_ring",                    test_pipe_close_both_ends_frees_ring,                    false, NULL },
    { "pipe.compose_with_spoor_transport",                  test_pipe_compose_with_spoor_transport,                  false, NULL },
    { "pipe_blocking.write_wakes_sleeping_reader",          test_pipe_blocking_write_wakes_sleeping_reader,          false, NULL },
    { "pipe_blocking.read_wakes_sleeping_writer",           test_pipe_blocking_read_wakes_sleeping_writer,           false, NULL },
    { "pipe_blocking.close_write_end_wakes_reader_with_eof", test_pipe_blocking_close_write_end_wakes_reader_with_eof, false, NULL },
    { "pipe_blocking.close_read_end_wakes_writer_with_epipe", test_pipe_blocking_close_read_end_wakes_writer_with_epipe, false, NULL },
    { "sys_pipe.allocates_two_distinct_spoor_handles", test_sys_pipe_allocates_two_distinct_spoor_handles, false, NULL },
    { "sys_pipe.proc_free_releases_handles",           test_sys_pipe_proc_free_releases_handles,           false, NULL },
    { "sys_pipe.handle_close_releases_one_end",        test_sys_pipe_handle_close_releases_one_end,        false, NULL },
    { "sys_rw.write_then_read_round_trip",             test_sys_rw_write_then_read_round_trip,             false, NULL },
    { "sys_rw.rights_check",                           test_sys_rw_rights_check,                           false, NULL },
    { "sys_rw.zero_length_validates_fd",               test_sys_rw_zero_length_validates_fd,               false, NULL },
    { "sys_rw.read_after_close_returns_eof",           test_sys_rw_read_after_close_returns_eof,           false, NULL },
    { "sys_pipe.dup_spoor_handle_acquires_ref",        test_sys_pipe_dup_spoor_handle_acquires_ref,        false, NULL },
    { "userspace.pipe_probe_round_trip",               test_pipe_probe_round_trip,                         false, NULL },
    { "sys_attach_9p.rejection_paths",                 test_sys_attach_9p_rejection_paths,                 false, NULL },
    { "sys_mount.happy_path_grafts_pipe_spoor",        test_sys_mount_happy_path_grafts_pipe_spoor,        false, NULL },
    { "sys_mount.idempotent_on_duplicate",             test_sys_mount_idempotent_on_duplicate,             false, NULL },
    { "sys_mount.rejects_bad_fd",                      test_sys_mount_rejects_bad_fd,                      false, NULL },
    { "sys_mount.rejects_missing_right_read",          test_sys_mount_rejects_missing_right_read,          false, NULL },
    { "sys_mount.rejects_invalid_flags",               test_sys_mount_rejects_invalid_flags,               false, NULL },
    { "sys_mount.rejects_null_territory",              test_sys_mount_rejects_null_territory,              false, NULL },
    { "sys_unmount.removes_entry_and_drops_ref",       test_sys_unmount_removes_entry_and_drops_ref,       false, NULL },
    { "sys_unmount.rejects_nonexistent_target",        test_sys_unmount_rejects_nonexistent_target,        false, NULL },
    { "sys_mount.caller_close_keeps_mount_alive",      test_sys_mount_caller_close_keeps_mount_alive,      false, NULL },
    { "userspace.attach_probe_round_trip",             test_attach_probe_round_trip,                       false, NULL },
    { "sys_mlockall.cap_gate",                         test_sys_mlockall_cap_gate,                         false, NULL },
    { "sys_set_dumpable.one_way_to_zero",              test_sys_set_dumpable_one_way_to_zero,              false, NULL },
    { "sys_set_traceable.one_way_to_zero",             test_sys_set_traceable_one_way_to_zero,             false, NULL },
    { "sys_corvus_caps.kproc_has_new_caps",            test_sys_corvus_caps_kproc_has_new_caps,            false, NULL },
    { "kern_random.seeded_returns_true_on_qemu",       test_kern_random_seeded_returns_true_on_qemu,       false, NULL },
    { "kern_random.bytes_produces_nonzero",            test_kern_random_bytes_produces_nonzero,            false, NULL },
    { "sys_spawn.happy_path",                          test_sys_spawn_happy_path,                          false, NULL },
    { "sys_spawn.rejects_null_name",                   test_sys_spawn_rejects_null_name,                   false, NULL },
    { "sys_spawn.rejects_zero_len",                    test_sys_spawn_rejects_zero_len,                    false, NULL },
    { "sys_spawn.rejects_oversize_name",               test_sys_spawn_rejects_oversize_name,               false, NULL },
    { "sys_spawn.rejects_missing_binary",              test_sys_spawn_rejects_missing_binary,              false, NULL },
    { "sys_spawn.rejects_embedded_nul",                test_sys_spawn_rejects_embedded_nul,                false, NULL },
    { "sys_wait_pid.no_children_returns_neg1",         test_sys_wait_pid_no_children_returns_neg1,         false, NULL },
    { "sys_spawn_with_fds.rejects_oversize_fd_count",  test_sys_spawn_with_fds_rejects_oversize_fd_count,  false, NULL },
    { "sys_spawn_with_fds.rejects_bad_fd",             test_sys_spawn_with_fds_rejects_bad_fd,             false, NULL },
    { "sys_spawn_with_fds.rejects_non_spoor_fd",       test_sys_spawn_with_fds_rejects_non_spoor_fd,       false, NULL },
    { "sys_spawn_with_fds.rejects_missing_binary",     test_sys_spawn_with_fds_rejects_missing_binary,     false, NULL },
    { "sys_spawn_with_fds.zero_count_succeeds",        test_sys_spawn_with_fds_zero_count_succeeds,        false, NULL },
    { "sys_spawn_with_fds.child_rights_subset_of_parent", test_sys_spawn_with_fds_child_rights_subset_of_parent, false, NULL },
    { "sys_spawn_with_caps.happy_path_zero_mask",      test_sys_spawn_with_caps_happy_path_zero_mask,      false, NULL },
    { "sys_spawn_with_caps.happy_path_subset_of_parent", test_sys_spawn_with_caps_happy_path_subset_of_parent, false, NULL },
    { "sys_spawn_with_caps.clamps_to_parent",          test_sys_spawn_with_caps_clamps_to_parent,          false, NULL },
    { "sys_spawn_with_caps.rejects_missing_binary",    test_sys_spawn_with_caps_rejects_missing_binary,    false, NULL },
    { "sys_spawn_with_caps.rejects_oversize_name",     test_sys_spawn_with_caps_rejects_oversize_name,     false, NULL },
    { "sys_spawn_full.happy_path_fds_and_caps",        test_sys_spawn_full_happy_path_fds_and_caps,        false, NULL },
    { "sys_spawn_full.zero_count_zero_mask_succeeds",  test_sys_spawn_full_zero_count_zero_mask_succeeds,  false, NULL },
    { "sys_spawn_full.rejects_oversize_fd_count",      test_sys_spawn_full_rejects_oversize_fd_count,      false, NULL },
    { "sys_spawn_full.rejects_bad_fd",                 test_sys_spawn_full_rejects_bad_fd,                 false, NULL },
    { "sys_spawn_full.rejects_missing_binary",         test_sys_spawn_full_rejects_missing_binary,         false, NULL },
    { "userspace.stratumd_stub_round_trip",            test_stratumd_stub_round_trip,                      false, NULL },
    { "userspace.stub_driver_round_trip",              test_stub_driver_round_trip,                        false, NULL },
    { "userspace.irq_latency_bench",   test_irq_latency_bench,             false, NULL },
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
    { "handle_hw.irq_out_of_range_rejected",
                                       test_handle_hw_irq_out_of_range_rejected,
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
