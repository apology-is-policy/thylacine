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

#include <thylacine/sched.h>   // DEBUG (#857): sched_dump_runnable on any test failure
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
void test_slub_kmalloc_overflow_guard(void);
void test_slub_cache_destroy_guards(void);
void test_gic_init_smoke(void);
void test_timer_tick_increments(void);
void test_clock_monotonic_advances(void);
void test_clock_realtime_anchored(void);
void test_clock_wallclock_offset_math(void);
void test_clock_identity_syscalls(void);
void test_clock_gettime_errors(void);
void test_hardening_detect_smoke(void);
void test_alternatives_patch_applied(void);
void test_alternatives_atomics_correct(void);
void test_context_create_destroy(void);
void test_context_round_trip(void);
void test_fp_cpacr_enabled(void);
void test_fp_round_trip_v_regs_fpsr_fpcr(void);
void test_sched_dispatch_smoke(void);
void test_sched_runnable_count(void);
void test_sched_runnable_count_excludes_idle(void);
void test_sched_preemption_smoke(void);
void test_sched_idle_in_wfi_observability(void);
void test_sched_notify_idle_peer_smoke(void);
void test_sched_notify_disabled_no_ipi(void);
void test_sched_capacity_normalize_synthetic_dtb(void);
void test_sched_place_by_capacity_synthetic_dtb(void);
void test_sched_select_target_cpu_homogeneous_is_prev(void);
void test_sched_ready_on_cross_cpu_enqueue(void);
void test_sched_ready_on_clamps_stale_vd(void);
void test_sched_wake_preempts_policy(void);
void test_sched_wake_preempt_same_cpu(void);
void test_rendez_sleep_immediate_cond_true(void);
void test_rendez_basic_handoff(void);
void test_rendez_death_interrupts_sleep(void);
void test_rendez_wakeup_no_waiter(void);
void test_rendez_intr_terminate_interrupts_sleep(void);
void test_rendez_intr_terminate_register_observe(void);
void test_rendez_intr_terminate_masked_sleeps_through(void);
void test_rendez_intr_terminate_interrupts_tsleep(void);
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
void test_proc_orphan_reparent_to_init(void);
void test_proc_orphan_reparent_zombie_to_init(void);
void test_proc_console_attached_smoke(void);
void test_proc_stripes_smoke(void);
void test_proc_group_terminate_smoke(void);
void test_proc_legate_scope_teardown(void);
void test_proc_legate_teardown_except_and_zero(void);
void test_proc_legate_teardown_from_zombie_chokepoint(void);
void test_resource_exempt_only_system(void);
void test_resource_page_charge_caps(void);
void test_resource_thread_cap_ok(void);
void test_resource_child_cap_ok(void);
void test_resource_child_count_tracks_list(void);
void test_resource_child_count_rfork_reap(void);
void test_resource_page_cap_attach_enforced(void);

void test_exec_ns_resolve_absolute_ok(void);
void test_exec_ns_resolve_relative_ok(void);
void test_exec_ns_miss_returns_null(void);
void test_exec_ns_non_executable_denied(void);
void test_namespace_layout_proc_ctl_cross(void);
void test_proc_identity_kproc_is_system(void);
void test_proc_identity_rfork_inherits(void);
void test_proc_identity_apply_sets_fields(void);
void test_proc_identity_spawn_set_rejected_without_cap(void);
void test_proc_identity_spawn_set_accepted_with_cap(void);
void test_proc_identity_set_rejects_reserved(void);
void test_proc_identity_set_rejects_system_supp_gid(void);
void test_proc_identity_peer_snapshot_by_stripes(void);
void test_proc_wait_pid_for_no_match(void);
void test_proc_wait_pid_for_wnohang_alive_then_reap(void);
void test_proc_wait_pid_for_selects_target(void);
void test_namespace_bind_smoke(void);
void test_namespace_cycle_rejected(void);
void test_namespace_fork_isolated(void);
void test_territory_cwd_lexical(void);
void test_territory_cwd_dot(void);
void test_territory_mount_smoke(void);
void test_territory_mount_idempotent_same_source(void);
void test_territory_mount_mrepl_replaces(void);
void test_territory_mount_unmount_missing_returns_error(void);
void test_territory_mount_table_full(void);
void test_territory_mount_clone_bumps_refs(void);
void test_territory_mount_destroy_drops_all_refs(void);
void test_territory_mount_devno_disambiguates(void);
void test_territory_mount_rejects_cycle(void);
void test_territory_mount_mp_path_lifecycle(void);
void test_territory_mount_format_ns(void);
void test_territory_mount_lookup_ref_survives_unmount(void);
void test_territory_root_ref_survives_pivot(void);
void test_territory_chroot_smoke(void);
void test_territory_chroot_idempotent_same_spoor(void);
void test_territory_chroot_replace_clunks_old(void);
void test_territory_chroot_clone_bumps_ref(void);
void test_territory_chroot_destroy_drops_ref(void);
void test_territory_chroot_null_returns_error(void);
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
void test_asid_width_valid(void);
void test_asid_resolve_reuse(void);
void test_asid_distinct_active(void);
void test_asid_rollover_preserves(void);
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
void test_exec_setup_auxv(void);
void test_exec_setup_auxv_no_phdr_segment(void);
void test_syscall_dispatch_unknown(void);
void test_syscall_dispatch_puts_smoke(void);
void test_syscall_dispatch_exits_ok(void);
void test_syscall_dispatch_exits_fail(void);
void test_syscall_dispatch_args_in_x0_to_x5(void);
void test_syscall_dispatch_set_tid_address(void);
void test_syscall_opath_walkonly_no_byte_io(void);
void test_uaccess_fixup_table_well_formed(void);
void test_uaccess_fixup_lookup_known(void);
void test_uaccess_fixup_lookup_unknown_returns_zero(void);
void test_uaccess_load_u8_unmapped_user_va_returns_minus1(void);
void test_fault_decode_kernel_data_translation_l2(void);
void test_fault_decode_kernel_data_permission_write(void);
void test_fault_decode_user_data_translation(void);
void test_fault_decode_user_instruction_fetch(void);
void test_fault_decode_access_flag(void);
void test_halls_fp_sane_accepts_valid(void);
void test_halls_fp_sane_rejects_misaligned(void);
void test_halls_fp_sane_rejects_non_increasing(void);
void test_halls_fp_sane_rejects_out_of_range(void);
void test_halls_link_addr_removes_slide(void);
void test_halls_link_addr_underflow_guarded(void);
void test_halls_frame_enter_leave_nesting(void);
void test_halls_frame_is_live_gate(void);
void test_halls_symbolize_table(void);
void test_vma_alloc_free_smoke(void);
void test_vma_alloc_constraints(void);
void test_vma_insert_lookup_smoke(void);
void test_vma_insert_overlap_rejected(void);
void test_vma_insert_sorted_invariant(void);
void test_vma_drain_releases_all(void);
void test_vma_find_gap_smoke(void);
void test_vma_find_gap_no_fit(void);
void test_vma_find_gap_constraints(void);
void test_vma_find_gap_straddle(void);
void test_sys_burrow_attach_returns_window_va(void);
void test_sys_burrow_attach_detach_round_trip(void);
void test_sys_burrow_attach_distinct(void);
void test_sys_burrow_attach_rounds_up(void);
void test_sys_burrow_attach_rejects_bad_length(void);
void test_sys_burrow_detach_rejects(void);
void test_sys_burrow_detach_window_confined(void);
void test_torpor_wait_rejects_bad_args(void);
void test_torpor_wait_rejects_unmapped_va(void);
void test_torpor_wake_rejects_bad_args(void);
void test_torpor_wake_empty_bucket_returns_zero(void);
void test_torpor_wait_value_mismatch_fast_path(void);
void test_torpor_wait_timeout_zero_returns_etimedout(void);
void test_torpor_wait_wake_handoff(void);
void test_torpor_wake_two_waiters_count_bound(void);
void test_loom_create_geometry(void);
void test_loom_create_rejects_bad_args(void);
void test_loom_refcount_lifecycle(void);
void test_loom_setup_via_proc(void);
void test_loom_setup_rejects(void);
void test_loom_register_handles(void);
void test_loom_register_rejects(void);
void test_loom_register_replaces(void);
void test_loom_register_buffers(void);
void test_loom_register_buffers_rejects(void);
void test_loom_register_buffers_replace(void);
void test_loom_post_cqe_back_pressure(void);
void test_loom_post_cqe_ignores_hostile_header(void);
void test_loom_dup_rejected(void);
void test_loom_enter_nop(void);
void test_loom_enter_submit_rejects(void);
void test_loom_enter_flags_and_bad_index(void);
void test_loom_enter_cq_admission_backpressure(void);
void test_loom_cq_waiter_wake(void);
void test_loom_cq_waiter_no_spurious_wake_on_full(void);
void test_loom_enter_inline_min_complete(void);
void test_loom_enter_min_complete_no_inflight(void);
void test_loom_sqpoll_setup_and_teardown(void);
void test_loom_sqpoll_drains_sq(void);
void test_loom_sqpoll_parks_on_cq_full(void);
void test_thread_create_user_ctx_layout(void);
void test_thread_exit_self_marks_exiting(void);
void test_thread_exit_self_last_thread_zombies(void);
void test_proc_multi_thread_reap(void);
void test_proc_wait_pid_concurrent_waiter_refused(void);
void test_notes_queue_alloc_free_smoke(void);
void test_notes_post_dequeue_smoke(void);
void test_notes_post_ordering(void);
void test_notes_unknown_name_rejected(void);
void test_notes_snare_forge_rejected(void);
void test_notes_queue_full_returns_minus1(void);
void test_notes_coalesce_synthetic(void);
void test_notes_mask_defers(void);
void test_notes_kill_dequeue_smoke(void);
void test_notes_kill_bypasses_mask(void);
void test_notes_reenqueue_head_smoke(void);
void test_notes_fd_read_skips_kill(void);
void test_notes_fd_peek_skips_kill(void);
void test_notes_post_child_exit_helper(void);
void test_notes_post_pipe_helper(void);
void test_notes_proc_lifecycle(void);
void test_notes_peek_does_not_pop(void);
void test_notes_interrupt_terminate_gate(void);
void test_notes_self_managing_flag(void);
void test_notes_intr_latch_lifecycle(void);
void test_notes_die_pending_predicate(void);
void test_directmap_kva_round_trip(void);
void test_directmap_alloc_through_directmap(void);
void test_directmap_vmalloc_mmio_smoke(void);
void test_directmap_pagemapped_808(void);
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
void test_full_attach_open_close(void);
void test_full_read_fills_zeroes(void);
void test_full_write_returns_minus1(void);
void test_random_rndr_available(void);
void test_random_read_produces_nonzero_bytes(void);
void test_random_read_varies_across_calls(void);
void test_cons_write_advances(void);
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
void test_devproc_write_ctl_rejects(void);
void test_devproc_read_dir_returns_neg1(void);
void test_devproc_read_partial_offset(void);
void test_devproc_kill_authorized_predicate(void);
void test_devproc_stat_native_ctl_owner(void);
void test_devproc_write_ctl_kill_dispatch(void);
void test_cons_blocking_read_wakeup(void);
void test_cons_ring_fill_drain(void);
void test_cons_ring_overflow_drop(void);
void test_cons_ctrlc_consumed(void);
void test_cons_break_sets_sak(void);
void test_cons_read_busy_guard(void);
void test_cons_read_bad_args(void);
void test_cons_console_owner_intr(void);
void test_proc_revoke_console_attached(void);
void test_cons_sak_revoke_regrant(void);
void test_cons_sak_failsafe_revoke_only(void);
void test_cons_sak_idempotent_flood(void);
void test_cons_sak_via_console_mgr(void);
void test_cons_sak_does_not_terminate_trusted(void);
void test_cons_sak_attaches_from_relinquished_state(void);
void test_proc_console_relinquish(void);
void test_proc_console_relinquish_other_owner(void);
void test_cons_console_open(void);
void test_uart_rx_path_enabled(void);   // #943 console-RX guard
void test_cons_poll_readiness(void);     // LS-8a
void test_cons_poll_deferred_wake(void); // LS-8a
void test_cons_termios_default(void);            // LS-8b
void test_cons_cook_canonical_line(void);        // LS-8b
void test_cons_cook_echo_off_no_output(void);    // LS-8b
void test_cons_cook_isig_toggle(void);           // LS-8b
void test_cons_cook_icrnl(void);                 // LS-8b
void test_cons_cook_onlcr_output(void);          // LS-8b
void test_cons_consctl_parse(void);              // LS-8b
void test_cons_consctl_render(void);             // LS-8b
void test_cons_cook_line_overflow(void);         // LS-8b
void test_devctl_bestiary_smoke(void);
void test_devctl_attach_returns_dir(void);
void test_devctl_walk_to_each_leaf(void);
void test_devctl_walk_unknown_misses(void);
void test_devctl_read_procs_format(void);
void test_devctl_read_memory_format(void);
void test_devctl_read_devices_format(void);
void test_devctl_read_kernel_base_format(void);
void test_devctl_kernel_base_gated(void);
void test_devctl_read_sched_format(void);
void test_devctl_write_rejected(void);
void test_devctl_read_dir_returns_neg1(void);
void test_devdev_bestiary_smoke(void);
void test_devdev_attach_returns_dir(void);
void test_devdev_walk_to_each_leaf(void);
void test_devdev_walk_unknown_misses(void);
void test_devdev_trivial_leaves(void);
void test_devdev_cons_gate(void);
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
void test_devramfs_stat_native_system_owned(void);
void test_devramfs_readdir_enumerates_root(void);
void test_devramfs_readdir_file_returns_neg1(void);
void test_devramfs_readdir_buffer_too_small_errs(void);
void test_devramfs_readdir_synth_dir_empty(void);
void test_devramfs_readdir_paginates_no_dup_no_skip(void);
void test_perm_check_owner_group_other(void);
void test_perm_check_owner_first_authoritative(void);
void test_perm_check_hostowner_override(void);
void test_perm_in_group(void);
void test_perm_want_for_omode(void);
void test_perm_rights_for_omode(void);
void test_perm_oexec_no_read_leak(void);
void test_perm_wstat_policy(void);
void test_perm_devramfs_enforced_real_metadata(void);
void test_perm_dev_flags(void);
void test_perm_check_dac_override_cap(void);
void test_perm_wstat_chown_cap(void);
void test_perm_check_want_zero_denied(void);
void test_perm_wstat_rejects_unknown_valid_bit(void);
void test_path_make_root(void);
void test_path_addelem_forms(void);
void test_path_parent_forms(void);
void test_path_addelem_overflow_null(void);
void test_path_ref_balance(void);
void test_path_spoor_clone_shares(void);
void test_path_spoor_extend(void);
void test_path_spoor_transplant(void);
void test_stalk_resolve_multi(void);
void test_stalk_resolve_deep(void);
void test_stalk_leading_and_double_slash(void);
void test_stalk_dot_noop(void);
void test_stalk_dotdot_pop(void);
void test_stalk_dotdot_containment(void);
void test_stalk_xsearch_deny(void);
void test_stalk_missing_component(void);
void test_stalk_opath_no_open(void);
void test_stalk_open_root(void);
void test_stalk_open_replace(void);
void test_stalk_depth_cap(void);
void test_stalk_lifetime_no_leak(void);
void test_stalk_cross_mount(void);
void test_stalk_cross_mount_final_quarry(void);
void test_stalk_cross_mount_xsearch_deny(void);
void test_stalk_mount_amode_no_cross(void);
void test_stalk_cross_mount_chain(void);
void test_stalk_cross_mount_no_leak(void);
void test_stalk_path_accumulate(void);
void test_stalk_path_dotdot(void);
void test_stalk_path_cross_transplant(void);
void test_stalk_path_adopt_transplant(void);
void test_devsrv_registered(void);
void test_devsrv_open_root_dir(void);
void test_devsrv_stat_native_root(void);
void test_devsrv_post_gate(void);
void test_devsrv_post_basic(void);
void test_devsrv_tombstone(void);
void test_devsrv_registry_full(void);
void test_devsrv_post_rollback(void);
void test_devsrv_registry_lifecycle(void);
void test_devsrv_svc_ref_holds_registry(void);
void test_devsrv_post_listener(void);
void test_devcap_registered(void);
void test_devcap_walk_grant_use(void);
void test_devcap_walk_unknown(void);
void test_devcap_open_writeonly(void);
void test_devcap_grant_gate_no_cap(void);
void test_devcap_grant_gate_bad_args(void);
void test_devcap_grant_replace(void);
void test_devcap_grant_table_full(void);
void test_devcap_use_gate_no_console(void);
void test_devcap_use_no_pending(void);
void test_devcap_use_basic(void);
void test_devcap_use_one_shot(void);
void test_devcap_use_mismatched_cap(void);
void test_devcap_exit_clears_grant(void);
void test_devcap_clearance_grant_gate_no_cap(void);
void test_devcap_clearance_grant_bad_args(void);
void test_devcap_clearance_redeem_basic(void);
void test_devcap_clearance_self_restriction(void);
void test_devcap_clearance_redeem_beyond_grant(void);
void test_devcap_clearance_one_shot(void);
void test_devcap_clearance_cross_stripes(void);
void test_devcap_clearance_valid_until(void);
void test_devcap_clearance_kind_isolation(void);
void test_srvconn_create_destroy(void);
void test_srvconn_roundtrip(void);
void test_srvconn_ring_capacity(void);
void test_srvconn_recv_blocks_then_wakes(void);
void test_srvconn_recv_deadline_timeout(void);
void test_srvconn_teardown_eofs(void);
void test_srvconn_teardown_wakes_blocked(void);
void test_devsrv_walk_service(void);
void test_devsrv_open_connect_byte(void);
void test_devsrv_kernel_attached_io_refused(void);
void test_devsrv_kernel_attached_server_close_eofs(void);
void test_devsrv_accept_immediate(void);
void test_devsrv_accept_blocks_then_wakes(void);
void test_devsrv_conn_io(void);
void test_devsrv_conn_release(void);
void test_devsrv_poster_exit_drains_backlog(void);
void test_devsrv_srv_peer_identity(void);
void test_devsrv_srv_peer_dead_peer(void);
void test_devsrv_srv_peer_gate(void);
void test_devsrv_srv_peer_bad_args(void);
void test_srv_client_no_per_proc_cap(void);
void test_srv_client_byte_mode_propagates_to_conn(void);
void test_srv_client_byte_mode_conn_dispatch(void);
void test_srv_client_byte_mode_mode_change_rebind_refused(void);
void test_srv_client_byte_mode_server_recv_blocking_eof(void);
void test_virtio_mmio_probe(void);
void test_virtio_magic_value(void);
void test_virtio_version_modern(void);
void test_virtio_rng_present(void);
void test_virtio_negotiate_features_smoke(void);
void test_virtio_virtqueue_alloc_destroy(void);
void test_virtio_find_by_device_id(void);
void test_virtio_reset_in_range_no_match(void);
void test_virtio_vq_size_for(void);
void test_virtio_proc_death_quiesces_device(void);
void test_virtio_proc_death_quiesces_vma_only_device(void);
void test_irqfwd_create_destroy(void);
void test_irqfwd_refcount_lifecycle(void);
void test_irqfwd_wait_wakes_on_sgi(void);
void test_irqfwd_collapses_concurrent_fires(void);
void test_irqfwd_second_waiter_refused(void);
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
void test_9p_wire_tflush_round_trip(void);
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
void test_9p_session_walk_fid_full_no_latch(void);
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
void test_9p_session_flush_reclaims_both(void);
void test_9p_session_late_reply_does_not_free_awaiting_flush(void);
void test_9p_transport_init_destroy(void);
void test_9p_transport_round_trip(void);
void test_9p_transport_send_frame_size_mismatch_rejected(void);
void test_9p_transport_recv_frame_too_large_rejected(void);
void test_9p_transport_partial_read_aggregation(void);
void test_9p_transport_backend_error_transitions_to_error(void);
void test_9p_transport_close_idempotent(void);
void test_9p_transport_exchange_drives_session_handshake(void);
void test_9p_transport_exchange_drives_session_walk(void);
void test_9p_transport_deadline_idle_vs_eof(void);
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
void test_9p_client_rlerror_hostile_ecode_bounded(void);
void test_9p_client_op_before_handshake_returns_ebusy(void);
void test_9p_client_lock_released_between_ops(void);
void test_9p_client_async_op_posts_cqe(void);
void test_9p_client_async_session_death_posts_error_cqe(void);
void test_9p_client_async_handoff_skips_async(void);
void test_9p_client_pump_deadline_idle(void);
void test_9p_client_pump_deadline_data_ready_progresses(void);
void test_9p_client_pump_deadline_chunked_frame_completes(void);
void test_9p_client_pump_deadline_busy_when_reader_active(void);
void test_9p_client_loom_fsync_e2e(void);
void test_9p_client_loom_rights_deny(void);
void test_9p_client_loom_quiesce_abandons_inflight(void);
void test_9p_client_loom_multishot_stream(void);
void test_9p_client_loom_multishot_backpressure(void);
void test_9p_client_loom_link_cancel_cascade(void);
void test_9p_client_loom_link_success_ordering(void);
void test_9p_client_loom_drain_barrier(void);
void test_9p_client_loom_independent_past_held(void);
void test_9p_client_loom_drain_waits_for_rearm_pending(void);
void test_9p_client_loom_read_e2e(void);
void test_9p_client_loom_write_e2e(void);
void test_9p_client_loom_rw_rejects(void);
void test_9p_client_loom_readdir_e2e(void);
void test_9p_client_loom_readlink_e2e(void);
void test_9p_client_loom_getattr_e2e(void);
void test_9p_client_loom_statfs_e2e(void);
void test_9p_client_loom_metaread_rejects(void);
void test_9p_client_loom_mkdir_e2e(void);
void test_9p_client_loom_setattr_e2e(void);
void test_9p_client_loom_renameat_e2e(void);
void test_9p_client_loom_mutation_rejects(void);
void test_9p_client_loom_multi_inflight_e2e(void);
void test_9p_client_loom_multi_inflight_read_e2e(void);
void test_dev9p_registered(void);
void test_dev9p_attach_client_root_spoor(void);
void test_dev9p_walk_one_component(void);
void test_dev9p_walk_clone(void);
void test_dev9p_open_lopens_fid(void);
void test_dev9p_read_routes_through_client(void);
void test_dev9p_write_routes_through_client(void);
void test_dev9p_close_clunks_owned_fid(void);
void test_dev9p_close_does_not_clunk_root_fid(void);
void test_dev9p_create_file(void);
void test_dev9p_create_dir(void);
void test_dev9p_fsync(void);
void test_dev9p_readdir(void);
void test_dev9p_readdir_cookie_high_bit(void);
void test_dev9p_rename(void);
void test_dev9p_unlink(void);
void test_dev9p_stat_native_maps_getattr(void);
void test_dev9p_wstat_native_drives_setattr(void);
void test_dev9p_perm_enforced_deny_allow(void);
void test_p9_attached_create_destroy(void);
void test_p9_attached_handshake_failure_returns_null(void);
void test_p9_attached_handshake_rlerror_ecode_overflow_clamped(void);
void test_p9_attached_root_spoor_walk_read(void);
void test_p9_attached_query_helpers(void);
void test_p9_attached_walked_outlives_root_no_uaf(void);
void test_sys_walk_open_max_length_name_nul_terminated(void);
void test_spoor_transport_init_destroy(void);
void test_spoor_transport_init_null_rejected(void);
void test_spoor_transport_send_routes_to_tx_dev_write(void);
void test_spoor_transport_recv_routes_to_rx_dev_read(void);
void test_spoor_transport_recv_empty_returns_zero(void);
void test_spoor_transport_close_clunks_when_owned(void);
void test_spoor_transport_close_preserves_when_unowned(void);
void test_spoor_transport_transport_core_round_trip(void);
void test_spoor_transport_end_to_end_handshake(void);
void test_9p_srvconn_transport_init_destroy(void);
void test_9p_srvconn_transport_init_null_rejected(void);
void test_9p_srvconn_transport_send_routes_to_c2s_ring(void);
void test_9p_srvconn_transport_recv_routes_from_s2c_ring(void);
void test_9p_srvconn_transport_large_frame_roundtrip(void);
void test_9p_srvconn_transport_close_drops_srvconn_ref(void);
void test_9p_srvconn_transport_kernel_attached_skips_teardown_on_handle_close(void);
void test_9p_srvconn_transport_send_preserves_caller_deadline(void);
void test_9p_srvconn_transport_deadline_vtable_routes(void);
void test_territory_pivot_root_smoke(void);
void test_territory_pivot_root_rejects_no_initial_root(void);
void test_territory_pivot_root_idempotent_same_spoor(void);
void test_territory_pivot_root_null_source_rejected(void);
void test_territory_pivot_root_does_not_touch_mounts(void);
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
void test_poll_ready_immediately_pollin(void);
void test_poll_ready_immediately_pollout(void);
void test_poll_timeout_zero_not_ready(void);
void test_poll_timeout_positive_fires(void);
void test_poll_block_then_wake_pollin(void);
void test_poll_pollhup_on_close_write_end(void);
void test_poll_multi_fd_one_ready(void);
void test_poll_bad_fd_revents_pollnval(void);
void test_poll_bad_args_rejected(void);
void test_poll_always_ready_null_dev_poll(void);
void test_poll_pollerr_on_write_after_read_close(void);
void test_poll_unregister_after_fast_path(void);
void test_poll_devsrv_listener_immediate_pollin(void);
void test_poll_devsrv_listener_empty_not_ready(void);
void test_poll_devsrv_listener_block_then_wake(void);
void test_poll_devsrv_listener_pollhup_on_tombstone(void);
void test_poll_devsrv_conn_pollin_on_send(void);
void test_poll_devsrv_conn_pollout_immediate(void);
void test_poll_devsrv_conn_pollhup_on_teardown(void);
void test_poll_devsrv_conn_block_then_wake_pollin(void);
void test_poll_null_obj_spoor_pollnval(void);
void test_poll_mixed_spoor_and_srv(void);
void test_poll_max_nfds(void);
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
void test_chacha20_block_vector(void);
void test_chacha20_keystream_continuity(void);
void test_kern_random_two_reads_differ(void);
void test_kern_random_large_read_nonzero(void);
void test_kern_random_virtio_reseed(void);
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
void test_sys_spawn_with_perms_zero_perm_is_spawn_full(void);
void test_sys_spawn_with_perms_console_attached_grants_may_post(void);
void test_sys_spawn_with_perms_rejects_non_console_attached_parent(void);
void test_sys_spawn_with_perms_rejects_unknown_perm_bits(void);
void test_sys_spawn_with_perms_holder_delegates_may_post(void);
void test_sys_spawn_with_perms_console_trusted_not_delegable(void);
void test_sys_spawn_with_perms_console_owner_grant_gate(void);
void test_sys_spawn_with_perms_console_owner_set_wiring(void);
void test_sys_spawn_full_argv_no_argv_acts_as_spawn_with_perms(void);
void test_sys_spawn_full_argv_golden_argc4(void);
void test_sys_spawn_full_argv_rejects_argc_over_max(void);
void test_sys_spawn_full_argv_rejects_data_len_over_max(void);
void test_sys_spawn_full_argv_rejects_missing_trailing_nul(void);
void test_sys_spawn_full_argv_rejects_nul_count_mismatch(void);
void test_sys_spawn_full_argv_rejects_argc_with_zero_data_len(void);
void test_sys_spawn_full_argv_rejects_zero_argc_with_nonzero_data(void);
void test_sys_spawn_full_argv_validate_req_golden(void);
void test_sys_spawn_full_argv_validate_req_rejects_pad_envp(void);
void test_sys_spawn_full_argv_validate_req_rejects_unknown_perm_bits(void);
void test_sys_spawn_full_argv_validate_req_rejects_oversize_fields(void);
void test_sys_spawn_full_argv_rejects_non_console_attached_perm_flags(void);
void test_stratumd_stub_round_trip(void);
void test_stratumd_stub_fs_round_trip(void);
void test_stratumd_stub_walk_round_trip(void);
void test_stub_driver_round_trip(void);
void test_irq_latency_bench(void);
void test_caps_kproc_has_all(void);
void test_caps_kproc_has_hw_create(void);
void test_caps_rfork_child_has_none(void);
void test_caps_rfork_with_caps_grants_subset(void);
void test_caps_rfork_with_caps_clamps_to_parent(void);
void test_caps_rfork_with_caps_zero_mask(void);
void test_caps_rfork_strips_elevation_only(void);
void test_caps_rfork_inherits_legate_scope(void);
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
    { "slub.kmalloc_overflow_guard",   test_slub_kmalloc_overflow_guard,   false, NULL },
    { "slub.cache_destroy_guards",     test_slub_cache_destroy_guards,     false, NULL },
    { "gic.init_smoke",                test_gic_init_smoke,                false, NULL },
    { "timer.tick_increments",         test_timer_tick_increments,         false, NULL },
    { "clock.monotonic_advances",      test_clock_monotonic_advances,      false, NULL },
    { "clock.realtime_anchored",       test_clock_realtime_anchored,       false, NULL },
    { "clock.wallclock_offset_math",   test_clock_wallclock_offset_math,   false, NULL },
    { "clock.identity_syscalls",       test_clock_identity_syscalls,       false, NULL },
    { "clock.gettime_errors",          test_clock_gettime_errors,          false, NULL },
    { "hardening.detect_smoke",        test_hardening_detect_smoke,        false, NULL },
    { "alternatives.patch_applied",    test_alternatives_patch_applied,    false, NULL },
    { "alternatives.atomics_correct",  test_alternatives_atomics_correct,  false, NULL },
    { "context.create_destroy",        test_context_create_destroy,        false, NULL },
    { "context.round_trip",            test_context_round_trip,            false, NULL },
    { "fp.cpacr_enabled",              test_fp_cpacr_enabled,              false, NULL },
    { "fp.round_trip_v_regs_fpsr_fpcr",
                                       test_fp_round_trip_v_regs_fpsr_fpcr, false, NULL },
    { "scheduler.dispatch_smoke",      test_sched_dispatch_smoke,          false, NULL },
    { "scheduler.runnable_count",      test_sched_runnable_count,          false, NULL },
    { "scheduler.runnable_count_excludes_idle",
                                       test_sched_runnable_count_excludes_idle, false, NULL },
    { "scheduler.preemption_smoke",    test_sched_preemption_smoke,        false, NULL },
    { "scheduler.idle_in_wfi_observability",
                                       test_sched_idle_in_wfi_observability,
                                                                           false, NULL },
    { "scheduler.notify_idle_peer_smoke",
                                       test_sched_notify_idle_peer_smoke,  false, NULL },
    { "scheduler.notify_disabled_no_ipi",
                                       test_sched_notify_disabled_no_ipi,  false, NULL },
    { "scheduler.capacity_normalize_synthetic_dtb",
                                       test_sched_capacity_normalize_synthetic_dtb, false, NULL },
    { "scheduler.place_by_capacity_synthetic_dtb",
                                       test_sched_place_by_capacity_synthetic_dtb, false, NULL },
    { "scheduler.select_target_cpu_homogeneous_is_prev",
                                       test_sched_select_target_cpu_homogeneous_is_prev, false, NULL },
    { "scheduler.ready_on_cross_cpu_enqueue",
                                       test_sched_ready_on_cross_cpu_enqueue, false, NULL },
    { "scheduler.ready_on_clamps_stale_vd",
                                       test_sched_ready_on_clamps_stale_vd, false, NULL },
    { "scheduler.wake_preempts_policy",
                                       test_sched_wake_preempts_policy,    false, NULL },
    { "scheduler.wake_preempt_same_cpu",
                                       test_sched_wake_preempt_same_cpu,   false, NULL },
    { "rendez.sleep_immediate_cond_true",
                                       test_rendez_sleep_immediate_cond_true,
                                                                           false, NULL },
    { "rendez.basic_handoff",          test_rendez_basic_handoff,          false, NULL },
    { "rendez.death_interrupts_sleep", test_rendez_death_interrupts_sleep, false, NULL },
    { "rendez.wakeup_no_waiter",       test_rendez_wakeup_no_waiter,       false, NULL },
    { "rendez.intr_terminate_interrupts_sleep",
                                       test_rendez_intr_terminate_interrupts_sleep,  false, NULL },
    { "rendez.intr_terminate_register_observe",
                                       test_rendez_intr_terminate_register_observe,  false, NULL },
    { "rendez.intr_terminate_masked_sleeps_through",
                                       test_rendez_intr_terminate_masked_sleeps_through, false, NULL },
    { "rendez.intr_terminate_interrupts_tsleep",
                                       test_rendez_intr_terminate_interrupts_tsleep, false, NULL },
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
    { "proc.orphan_reparent_to_init",  test_proc_orphan_reparent_to_init,  false, NULL },
    { "proc.orphan_reparent_zombie_to_init",
                                       test_proc_orphan_reparent_zombie_to_init, false, NULL },
    { "proc.console_attached_smoke",   test_proc_console_attached_smoke,   false, NULL },
    { "proc.stripes_smoke",            test_proc_stripes_smoke,            false, NULL },
    { "proc.group_terminate_smoke",    test_proc_group_terminate_smoke,    false, NULL },
    { "proc.legate_scope_teardown",    test_proc_legate_scope_teardown,    false, NULL },
    { "proc.legate_teardown_except_and_zero",
                                       test_proc_legate_teardown_except_and_zero, false, NULL },
    { "proc.legate_teardown_from_zombie_chokepoint",
                                       test_proc_legate_teardown_from_zombie_chokepoint, false, NULL },
    { "proc.wait_pid_for_no_match",    test_proc_wait_pid_for_no_match,    false, NULL },
    { "proc.wait_pid_for_wnohang_alive_then_reap",
                                       test_proc_wait_pid_for_wnohang_alive_then_reap, false, NULL },
    { "proc.wait_pid_for_selects_target",
                                       test_proc_wait_pid_for_selects_target, false, NULL },
    { "resource.exempt_only_system",   test_resource_exempt_only_system,   false, NULL },
    { "resource.page_charge_caps",     test_resource_page_charge_caps,     false, NULL },
    { "resource.thread_cap_ok",        test_resource_thread_cap_ok,        false, NULL },
    { "resource.child_cap_ok",         test_resource_child_cap_ok,         false, NULL },
    { "resource.child_count_tracks_list",
                                       test_resource_child_count_tracks_list, false, NULL },
    { "resource.child_count_rfork_reap",
                                       test_resource_child_count_rfork_reap, false, NULL },
    { "exec_ns.resolve_absolute_ok",   test_exec_ns_resolve_absolute_ok,   false, NULL },
    { "exec_ns.resolve_relative_ok",   test_exec_ns_resolve_relative_ok,   false, NULL },
    { "exec_ns.miss_returns_null",     test_exec_ns_miss_returns_null,     false, NULL },
    { "exec_ns.non_executable_denied", test_exec_ns_non_executable_denied, false, NULL },
    { "namespace_layout.proc_ctl_cross", test_namespace_layout_proc_ctl_cross, false, NULL },
    { "resource.page_cap_attach_enforced",
                                       test_resource_page_cap_attach_enforced, false, NULL },
    { "proc_identity.kproc_is_system", test_proc_identity_kproc_is_system, false, NULL },
    { "proc_identity.rfork_inherits",  test_proc_identity_rfork_inherits,  false, NULL },
    { "proc_identity.apply_sets_fields",
                                       test_proc_identity_apply_sets_fields,
                                       false, NULL },
    { "proc_identity.spawn_set_rejected_without_cap",
                                       test_proc_identity_spawn_set_rejected_without_cap,
                                       false, NULL },
    { "proc_identity.spawn_set_accepted_with_cap",
                                       test_proc_identity_spawn_set_accepted_with_cap,
                                       false, NULL },
    { "proc_identity.set_rejects_reserved",
                                       test_proc_identity_set_rejects_reserved,
                                       false, NULL },
    { "proc_identity.set_rejects_system_supp_gid",
                                       test_proc_identity_set_rejects_system_supp_gid,
                                       false, NULL },
    { "proc_identity.peer_snapshot_by_stripes",
                                       test_proc_identity_peer_snapshot_by_stripes,
                                       false, NULL },
    { "territory.bind_smoke",          test_namespace_bind_smoke,          false, NULL },
    { "territory.cycle_rejected",      test_namespace_cycle_rejected,      false, NULL },
    { "territory.fork_isolated",       test_namespace_fork_isolated,       false, NULL },
    { "territory.cwd_lexical",         test_territory_cwd_lexical,         false, NULL },
    { "territory.cwd_dot",             test_territory_cwd_dot,             false, NULL },
    { "territory_mount.smoke",                            test_territory_mount_smoke,                            false, NULL },
    { "territory_mount.idempotent_same_source",           test_territory_mount_idempotent_same_source,           false, NULL },
    { "territory_mount.mrepl_replaces",                   test_territory_mount_mrepl_replaces,                   false, NULL },
    { "territory_mount.unmount_missing_returns_error",    test_territory_mount_unmount_missing_returns_error,    false, NULL },
    { "territory_mount.table_full",                       test_territory_mount_table_full,                       false, NULL },
    { "territory_mount.clone_bumps_refs",                 test_territory_mount_clone_bumps_refs,                 false, NULL },
    { "territory_mount.destroy_drops_all_refs",           test_territory_mount_destroy_drops_all_refs,           false, NULL },
    { "territory_mount.devno_disambiguates",              test_territory_mount_devno_disambiguates,              false, NULL },
    { "territory_mount.rejects_cycle",                    test_territory_mount_rejects_cycle,                    false, NULL },
    { "territory_mount.mp_path_lifecycle",                test_territory_mount_mp_path_lifecycle,                false, NULL },
    { "territory_mount.format_ns",                        test_territory_mount_format_ns,                        false, NULL },
    { "territory_mount.lookup_ref_survives_unmount",      test_territory_mount_lookup_ref_survives_unmount,      false, NULL },
    { "territory_mount.root_ref_survives_pivot",          test_territory_root_ref_survives_pivot,                false, NULL },
    { "territory.chroot_smoke",                           test_territory_chroot_smoke,                           false, NULL },
    { "territory.chroot_idempotent_same_spoor",           test_territory_chroot_idempotent_same_spoor,           false, NULL },
    { "territory.chroot_replace_clunks_old",              test_territory_chroot_replace_clunks_old,              false, NULL },
    { "territory.chroot_clone_bumps_ref",                 test_territory_chroot_clone_bumps_ref,                 false, NULL },
    { "territory.chroot_destroy_drops_ref",               test_territory_chroot_destroy_drops_ref,               false, NULL },
    { "territory.chroot_null_returns_error",              test_territory_chroot_null_returns_error,              false, NULL },
    { "territory.pivot_root_smoke",                       test_territory_pivot_root_smoke,                       false, NULL },
    { "territory.pivot_root_rejects_no_initial_root",     test_territory_pivot_root_rejects_no_initial_root,     false, NULL },
    { "territory.pivot_root_idempotent_same_spoor",       test_territory_pivot_root_idempotent_same_spoor,       false, NULL },
    { "territory.pivot_root_null_source_rejected",        test_territory_pivot_root_null_source_rejected,        false, NULL },
    { "territory.pivot_root_does_not_touch_mounts",       test_territory_pivot_root_does_not_touch_mounts,       false, NULL },
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
    { "asid.width_valid",              test_asid_width_valid,              false, NULL },
    { "asid.resolve_reuse",            test_asid_resolve_reuse,            false, NULL },
    { "asid.distinct_active",          test_asid_distinct_active,          false, NULL },
    { "asid.rollover_preserves",       test_asid_rollover_preserves,       false, NULL },
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
    { "exec.setup_auxv",
                                       test_exec_setup_auxv,
                                                                           false, NULL },
    { "exec.setup_auxv_no_phdr_segment",
                                       test_exec_setup_auxv_no_phdr_segment,
                                                                           false, NULL },
    { "syscall.dispatch_unknown",      test_syscall_dispatch_unknown,      false, NULL },
    { "syscall.dispatch_puts_smoke",   test_syscall_dispatch_puts_smoke,   false, NULL },
    { "syscall.dispatch_exits_ok",     test_syscall_dispatch_exits_ok,     false, NULL },
    { "syscall.dispatch_exits_fail",   test_syscall_dispatch_exits_fail,   false, NULL },
    { "syscall.dispatch_args_in_x0_to_x5",
                                       test_syscall_dispatch_args_in_x0_to_x5,
                                                                           false, NULL },
    { "syscall.dispatch_set_tid_address",
                                       test_syscall_dispatch_set_tid_address,
                                                                           false, NULL },
    { "syscall.opath_walkonly_no_byte_io",
                                       test_syscall_opath_walkonly_no_byte_io,
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
    { "halls.fp_sane_accepts_valid",       test_halls_fp_sane_accepts_valid,       false, NULL },
    { "halls.fp_sane_rejects_misaligned",  test_halls_fp_sane_rejects_misaligned,  false, NULL },
    { "halls.fp_sane_rejects_non_increasing",
                                       test_halls_fp_sane_rejects_non_increasing,  false, NULL },
    { "halls.fp_sane_rejects_out_of_range",
                                       test_halls_fp_sane_rejects_out_of_range,    false, NULL },
    { "halls.link_addr_removes_slide",     test_halls_link_addr_removes_slide,     false, NULL },
    { "halls.link_addr_underflow_guarded", test_halls_link_addr_underflow_guarded, false, NULL },
    { "halls.frame_enter_leave_nesting",   test_halls_frame_enter_leave_nesting,   false, NULL },
    { "halls.frame_is_live_gate",          test_halls_frame_is_live_gate,          false, NULL },
    { "halls.symbolize_table",             test_halls_symbolize_table,             false, NULL },
    { "vma.alloc_free_smoke",          test_vma_alloc_free_smoke,          false, NULL },
    { "vma.alloc_constraints",         test_vma_alloc_constraints,         false, NULL },
    { "vma.insert_lookup_smoke",       test_vma_insert_lookup_smoke,       false, NULL },
    { "vma.insert_overlap_rejected",   test_vma_insert_overlap_rejected,   false, NULL },
    { "vma.insert_sorted_invariant",   test_vma_insert_sorted_invariant,   false, NULL },
    { "vma.drain_releases_all",        test_vma_drain_releases_all,        false, NULL },
    { "vma.find_gap_smoke",            test_vma_find_gap_smoke,            false, NULL },
    { "vma.find_gap_no_fit",           test_vma_find_gap_no_fit,           false, NULL },
    { "vma.find_gap_constraints",      test_vma_find_gap_constraints,      false, NULL },
    { "vma.find_gap_straddle",         test_vma_find_gap_straddle,         false, NULL },
    { "sys_burrow.attach_returns_window_va",  test_sys_burrow_attach_returns_window_va,  false, NULL },
    { "sys_burrow.attach_detach_round_trip",  test_sys_burrow_attach_detach_round_trip,  false, NULL },
    { "sys_burrow.attach_distinct",           test_sys_burrow_attach_distinct,           false, NULL },
    { "sys_burrow.attach_rounds_up",          test_sys_burrow_attach_rounds_up,          false, NULL },
    { "sys_burrow.attach_rejects_bad_length", test_sys_burrow_attach_rejects_bad_length, false, NULL },
    { "sys_burrow.detach_rejects",            test_sys_burrow_detach_rejects,            false, NULL },
    { "sys_burrow.detach_window_confined",    test_sys_burrow_detach_window_confined,    false, NULL },
    { "torpor.wait_rejects_bad_args",          test_torpor_wait_rejects_bad_args,          false, NULL },
    { "torpor.wait_rejects_unmapped_va",       test_torpor_wait_rejects_unmapped_va,       false, NULL },
    { "torpor.wake_rejects_bad_args",          test_torpor_wake_rejects_bad_args,          false, NULL },
    { "torpor.wake_empty_bucket_returns_zero", test_torpor_wake_empty_bucket_returns_zero, false, NULL },
    { "torpor.wait_value_mismatch_fast_path",  test_torpor_wait_value_mismatch_fast_path,  false, NULL },
    { "torpor.wait_timeout_zero_returns_etimedout", test_torpor_wait_timeout_zero_returns_etimedout, false, NULL },
    { "torpor.wait_wake_handoff",              test_torpor_wait_wake_handoff,              false, NULL },
    { "torpor.wake_two_waiters_count_bound",   test_torpor_wake_two_waiters_count_bound,   false, NULL },
    { "loom.create_geometry",            test_loom_create_geometry,            false, NULL },
    { "loom.create_rejects_bad_args",    test_loom_create_rejects_bad_args,    false, NULL },
    { "loom.refcount_lifecycle",         test_loom_refcount_lifecycle,         false, NULL },
    { "loom.setup_via_proc",             test_loom_setup_via_proc,             false, NULL },
    { "loom.setup_rejects",              test_loom_setup_rejects,              false, NULL },
    { "loom.register_handles",           test_loom_register_handles,           false, NULL },
    { "loom.register_rejects",           test_loom_register_rejects,           false, NULL },
    { "loom.register_replaces",          test_loom_register_replaces,          false, NULL },
    { "loom.register_buffers",           test_loom_register_buffers,           false, NULL },
    { "loom.register_buffers_rejects",   test_loom_register_buffers_rejects,   false, NULL },
    { "loom.register_buffers_replace",   test_loom_register_buffers_replace,   false, NULL },
    { "loom.post_cqe_back_pressure",     test_loom_post_cqe_back_pressure,     false, NULL },
    { "loom.post_cqe_ignores_hostile_header", test_loom_post_cqe_ignores_hostile_header, false, NULL },
    { "loom.dup_rejected",               test_loom_dup_rejected,               false, NULL },
    { "loom.enter_nop",                  test_loom_enter_nop,                  false, NULL },
    { "loom.enter_submit_rejects",       test_loom_enter_submit_rejects,       false, NULL },
    { "loom.enter_flags_and_bad_index",  test_loom_enter_flags_and_bad_index,  false, NULL },
    { "loom.enter_cq_admission_backpressure", test_loom_enter_cq_admission_backpressure, false, NULL },
    { "loom.cq_waiter_wake",             test_loom_cq_waiter_wake,             false, NULL },
    { "loom.cq_waiter_no_spurious_wake_on_full", test_loom_cq_waiter_no_spurious_wake_on_full, false, NULL },
    { "loom.enter_inline_min_complete",  test_loom_enter_inline_min_complete,  false, NULL },
    { "loom.enter_min_complete_no_inflight", test_loom_enter_min_complete_no_inflight, false, NULL },
    { "loom.sqpoll_setup_and_teardown",  test_loom_sqpoll_setup_and_teardown,  false, NULL },
    { "loom.sqpoll_drains_sq",           test_loom_sqpoll_drains_sq,           false, NULL },
    { "loom.sqpoll_parks_on_cq_full",    test_loom_sqpoll_parks_on_cq_full,    false, NULL },
    { "thread.create_user_ctx_layout",         test_thread_create_user_ctx_layout,         false, NULL },
    { "thread.exit_self_marks_exiting",        test_thread_exit_self_marks_exiting,        false, NULL },
    { "thread.exit_self_last_thread_zombies",  test_thread_exit_self_last_thread_zombies,  false, NULL },
    { "proc.multi_thread_reap",                test_proc_multi_thread_reap,                false, NULL },
    { "proc.wait_pid_concurrent_waiter_refused", test_proc_wait_pid_concurrent_waiter_refused, false, NULL },
    { "notes.queue_alloc_free_smoke",          test_notes_queue_alloc_free_smoke,          false, NULL },
    { "notes.post_dequeue_smoke",              test_notes_post_dequeue_smoke,              false, NULL },
    { "notes.post_ordering",                   test_notes_post_ordering,                   false, NULL },
    { "notes.unknown_name_rejected",           test_notes_unknown_name_rejected,           false, NULL },
    { "notes.snare_forge_rejected",            test_notes_snare_forge_rejected,            false, NULL },
    { "notes.queue_full_returns_minus1",       test_notes_queue_full_returns_minus1,       false, NULL },
    { "notes.coalesce_synthetic",              test_notes_coalesce_synthetic,              false, NULL },
    { "notes.mask_defers",                     test_notes_mask_defers,                     false, NULL },
    { "notes.kill_dequeue_smoke",              test_notes_kill_dequeue_smoke,              false, NULL },
    { "notes.kill_bypasses_mask",              test_notes_kill_bypasses_mask,              false, NULL },
    { "notes.reenqueue_head_smoke",            test_notes_reenqueue_head_smoke,            false, NULL },
    { "notes.fd_read_skips_kill",              test_notes_fd_read_skips_kill,              false, NULL },
    { "notes.fd_peek_skips_kill",              test_notes_fd_peek_skips_kill,              false, NULL },
    { "notes.post_child_exit_helper",          test_notes_post_child_exit_helper,          false, NULL },
    { "notes.post_pipe_helper",                test_notes_post_pipe_helper,                false, NULL },
    { "notes.proc_lifecycle",                  test_notes_proc_lifecycle,                  false, NULL },
    { "notes.peek_does_not_pop",               test_notes_peek_does_not_pop,               false, NULL },
    { "notes.interrupt_terminate_gate",        test_notes_interrupt_terminate_gate,        false, NULL },
    { "notes.self_managing_flag",              test_notes_self_managing_flag,              false, NULL },
    { "notes.intr_latch_lifecycle",            test_notes_intr_latch_lifecycle,            false, NULL },
    { "notes.die_pending_predicate",           test_notes_die_pending_predicate,           false, NULL },
    { "directmap.kva_round_trip",      test_directmap_kva_round_trip,      false, NULL },
    { "directmap.alloc_through_directmap",
                                       test_directmap_alloc_through_directmap,
                                                                           false, NULL },
    { "directmap.vmalloc_mmio_smoke",  test_directmap_vmalloc_mmio_smoke,  false, NULL },
    { "directmap.pagemapped_808",      test_directmap_pagemapped_808,      false, NULL },
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
    { "full.attach_open_close",        test_full_attach_open_close,        false, NULL },
    { "full.read_fills_zeroes",        test_full_read_fills_zeroes,        false, NULL },
    { "full.write_returns_minus1",     test_full_write_returns_minus1,     false, NULL },
    { "random.rndr_available",         test_random_rndr_available,         false, NULL },
    { "random.read_produces_nonzero",  test_random_read_produces_nonzero_bytes, false, NULL },
    { "random.read_varies",            test_random_read_varies_across_calls, false, NULL },
    { "cons.write_advances",           test_cons_write_advances,           false, NULL },
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
    { "devproc.write_ctl_rejects",     test_devproc_write_ctl_rejects,     false, NULL },
    { "devproc.read_dir_returns_neg1", test_devproc_read_dir_returns_neg1, false, NULL },
    { "devproc.read_partial_offset",   test_devproc_read_partial_offset,   false, NULL },
    { "devproc.kill_authorized_predicate", test_devproc_kill_authorized_predicate, false, NULL },
    { "devproc.stat_native_ctl_owner",     test_devproc_stat_native_ctl_owner,     false, NULL },
    { "devproc.write_ctl_kill_dispatch",   test_devproc_write_ctl_kill_dispatch,   false, NULL },
    { "cons.blocking_read_wakeup",     test_cons_blocking_read_wakeup,     false, NULL },
    { "cons.ring_fill_drain",          test_cons_ring_fill_drain,          false, NULL },
    { "cons.ring_overflow_drop",       test_cons_ring_overflow_drop,       false, NULL },
    { "cons.ctrlc_consumed",           test_cons_ctrlc_consumed,           false, NULL },
    { "cons.break_sets_sak",           test_cons_break_sets_sak,           false, NULL },
    { "cons.read_busy_guard",          test_cons_read_busy_guard,          false, NULL },
    { "cons.read_bad_args",            test_cons_read_bad_args,            false, NULL },
    { "cons.console_owner_intr",       test_cons_console_owner_intr,       false, NULL },
    { "proc.revoke_console_attached",  test_proc_revoke_console_attached,  false, NULL },
    { "cons.sak_revoke_regrant",       test_cons_sak_revoke_regrant,       false, NULL },
    { "cons.sak_failsafe_revoke_only", test_cons_sak_failsafe_revoke_only, false, NULL },
    { "cons.sak_idempotent_flood",     test_cons_sak_idempotent_flood,     false, NULL },
    { "cons.sak_via_console_mgr",      test_cons_sak_via_console_mgr,      false, NULL },
    { "cons.sak_does_not_terminate_trusted",
                                       test_cons_sak_does_not_terminate_trusted, false, NULL },
    { "cons.sak_attaches_from_relinquished_state",
                                       test_cons_sak_attaches_from_relinquished_state, false, NULL },
    { "proc.console_relinquish",       test_proc_console_relinquish,       false, NULL },
    { "proc.console_relinquish_other", test_proc_console_relinquish_other_owner, false, NULL },
    { "cons.console_open",             test_cons_console_open,             false, NULL },
    { "uart.rx_path_enabled",          test_uart_rx_path_enabled,          false, NULL },
    { "cons.poll_readiness",           test_cons_poll_readiness,           false, NULL },
    { "cons.poll_deferred_wake",       test_cons_poll_deferred_wake,       false, NULL },
    { "cons.termios_default",          test_cons_termios_default,          false, NULL },
    { "cons.cook_canonical_line",      test_cons_cook_canonical_line,      false, NULL },
    { "cons.cook_echo_off_no_output",  test_cons_cook_echo_off_no_output,  false, NULL },
    { "cons.cook_isig_toggle",         test_cons_cook_isig_toggle,         false, NULL },
    { "cons.cook_icrnl",               test_cons_cook_icrnl,               false, NULL },
    { "cons.cook_onlcr_output",        test_cons_cook_onlcr_output,        false, NULL },
    { "cons.consctl_parse",            test_cons_consctl_parse,            false, NULL },
    { "cons.consctl_render",           test_cons_consctl_render,           false, NULL },
    { "cons.cook_line_overflow",       test_cons_cook_line_overflow,       false, NULL },
    { "devctl.bestiary_smoke",         test_devctl_bestiary_smoke,         false, NULL },
    { "devctl.attach_returns_dir",     test_devctl_attach_returns_dir,     false, NULL },
    { "devctl.walk_to_each_leaf",      test_devctl_walk_to_each_leaf,      false, NULL },
    { "devctl.walk_unknown_misses",    test_devctl_walk_unknown_misses,    false, NULL },
    { "devctl.read_procs_format",      test_devctl_read_procs_format,      false, NULL },
    { "devctl.read_memory_format",     test_devctl_read_memory_format,     false, NULL },
    { "devctl.read_devices_format",    test_devctl_read_devices_format,    false, NULL },
    { "devctl.read_kernel_base_format",
                                       test_devctl_read_kernel_base_format, false, NULL },
    { "devctl.kernel_base_gated",      test_devctl_kernel_base_gated,      false, NULL },
    { "devctl.read_sched_format",      test_devctl_read_sched_format,      false, NULL },
    { "devctl.write_rejected",         test_devctl_write_rejected,         false, NULL },
    { "devctl.read_dir_returns_neg1",  test_devctl_read_dir_returns_neg1,  false, NULL },
    { "devdev.bestiary_smoke",         test_devdev_bestiary_smoke,         false, NULL },
    { "devdev.attach_returns_dir",     test_devdev_attach_returns_dir,     false, NULL },
    { "devdev.walk_to_each_leaf",      test_devdev_walk_to_each_leaf,      false, NULL },
    { "devdev.walk_unknown_misses",    test_devdev_walk_unknown_misses,    false, NULL },
    { "devdev.trivial_leaves",         test_devdev_trivial_leaves,         false, NULL },
    { "devdev.cons_gate",              test_devdev_cons_gate,              false, NULL },
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
    { "devramfs.stat_native_system_owned",
                                       test_devramfs_stat_native_system_owned, false, NULL },
    { "devramfs.write_rejected",       test_devramfs_write_rejected,       false, NULL },
    { "devramfs.readdir_enumerates_root",
                                       test_devramfs_readdir_enumerates_root, false, NULL },
    { "devramfs.readdir_file_returns_neg1",
                                       test_devramfs_readdir_file_returns_neg1, false, NULL },
    { "devramfs.readdir_buffer_too_small_errs",
                                       test_devramfs_readdir_buffer_too_small_errs, false, NULL },
    { "devramfs.readdir_synth_dir_empty",
                                       test_devramfs_readdir_synth_dir_empty, false, NULL },
    { "devramfs.readdir_paginates_no_dup_no_skip",
                                       test_devramfs_readdir_paginates_no_dup_no_skip, false, NULL },
    { "devsrv.registered",             test_devsrv_registered,             false, NULL },
    { "devsrv.open_root_dir",          test_devsrv_open_root_dir,          false, NULL },
    { "devsrv.stat_native_root",       test_devsrv_stat_native_root,       false, NULL },
    { "devsrv.post_gate",              test_devsrv_post_gate,              false, NULL },
    { "devsrv.post_basic",             test_devsrv_post_basic,             false, NULL },
    { "devsrv.tombstone",              test_devsrv_tombstone,              false, NULL },
    { "devsrv.registry_full",          test_devsrv_registry_full,          false, NULL },
    { "devsrv.post_rollback",          test_devsrv_post_rollback,          false, NULL },
    { "devsrv.registry_lifecycle",     test_devsrv_registry_lifecycle,     false, NULL },
    { "devsrv.svc_ref_holds_registry", test_devsrv_svc_ref_holds_registry, false, NULL },
    { "devsrv.post_listener",          test_devsrv_post_listener,          false, NULL },
    { "devcap.registered",             test_devcap_registered,             false, NULL },
    { "devcap.walk_grant_use",         test_devcap_walk_grant_use,         false, NULL },
    { "devcap.walk_unknown",           test_devcap_walk_unknown,           false, NULL },
    { "devcap.open_writeonly",         test_devcap_open_writeonly,         false, NULL },
    { "devcap.grant_gate_no_cap",      test_devcap_grant_gate_no_cap,      false, NULL },
    { "devcap.grant_gate_bad_args",    test_devcap_grant_gate_bad_args,    false, NULL },
    { "devcap.grant_replace",          test_devcap_grant_replace,          false, NULL },
    { "devcap.grant_table_full",       test_devcap_grant_table_full,       false, NULL },
    { "devcap.use_gate_no_console",    test_devcap_use_gate_no_console,    false, NULL },
    { "devcap.use_no_pending",         test_devcap_use_no_pending,         false, NULL },
    { "devcap.use_basic",              test_devcap_use_basic,              false, NULL },
    { "devcap.use_one_shot",           test_devcap_use_one_shot,           false, NULL },
    { "devcap.use_mismatched_cap",     test_devcap_use_mismatched_cap,     false, NULL },
    { "devcap.exit_clears_grant",      test_devcap_exit_clears_grant,      false, NULL },
    { "devcap.clearance_grant_gate_no_cap",   test_devcap_clearance_grant_gate_no_cap,   false, NULL },
    { "devcap.clearance_grant_bad_args",      test_devcap_clearance_grant_bad_args,      false, NULL },
    { "devcap.clearance_redeem_basic",        test_devcap_clearance_redeem_basic,        false, NULL },
    { "devcap.clearance_self_restriction",    test_devcap_clearance_self_restriction,    false, NULL },
    { "devcap.clearance_redeem_beyond_grant", test_devcap_clearance_redeem_beyond_grant, false, NULL },
    { "devcap.clearance_one_shot",            test_devcap_clearance_one_shot,            false, NULL },
    { "devcap.clearance_cross_stripes",       test_devcap_clearance_cross_stripes,       false, NULL },
    { "devcap.clearance_valid_until",         test_devcap_clearance_valid_until,         false, NULL },
    { "devcap.clearance_kind_isolation",      test_devcap_clearance_kind_isolation,      false, NULL },
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
    { "devsrv.walk_service",           test_devsrv_walk_service,           false, NULL },
    { "devsrv.open_connect_byte",      test_devsrv_open_connect_byte,      false, NULL },
    { "devsrv.kernel_attached_io_refused",
                                       test_devsrv_kernel_attached_io_refused, false, NULL },
    { "devsrv.kernel_attached_server_close_eofs",
                                       test_devsrv_kernel_attached_server_close_eofs, false, NULL },
    { "devsrv.accept_immediate",       test_devsrv_accept_immediate,       false, NULL },
    { "devsrv.accept_blocks_then_wakes",
                                       test_devsrv_accept_blocks_then_wakes,
                                                                           false, NULL },
    { "devsrv.conn_io",                test_devsrv_conn_io,                false, NULL },
    { "devsrv.conn_release",           test_devsrv_conn_release,           false, NULL },
    { "devsrv.poster_exit_drains_backlog",
                                       test_devsrv_poster_exit_drains_backlog,
                                                                           false, NULL },
    { "devsrv.srv_peer_identity",      test_devsrv_srv_peer_identity,      false, NULL },
    { "devsrv.srv_peer_dead_peer",     test_devsrv_srv_peer_dead_peer,     false, NULL },
    { "devsrv.srv_peer_gate",          test_devsrv_srv_peer_gate,          false, NULL },
    { "devsrv.srv_peer_bad_args",      test_devsrv_srv_peer_bad_args,      false, NULL },
    { "srv_client.no_per_proc_cap",
                                       test_srv_client_no_per_proc_cap,
                                                                           false, NULL },
    { "srv_client.byte_mode_propagates_to_conn",
                                       test_srv_client_byte_mode_propagates_to_conn,
                                                                           false, NULL },
    { "srv_client.byte_mode_conn_dispatch",
                                       test_srv_client_byte_mode_conn_dispatch,
                                                                           false, NULL },
    { "srv_client.byte_mode_mode_change_rebind_refused",
                                       test_srv_client_byte_mode_mode_change_rebind_refused,
                                                                           false, NULL },
    { "srv_client.byte_mode_server_recv_blocking_eof",
                                       test_srv_client_byte_mode_server_recv_blocking_eof,
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
    { "virtio.reset_in_range_no_match", test_virtio_reset_in_range_no_match, false, NULL },
    { "virtio.vq_size_for",            test_virtio_vq_size_for,            false, NULL },
    { "virtio.proc_death_quiesces_device",
                                       test_virtio_proc_death_quiesces_device, false, NULL },
    { "virtio.proc_death_quiesces_vma_only_device",
                                       test_virtio_proc_death_quiesces_vma_only_device, false, NULL },
    { "irqfwd.create_destroy",         test_irqfwd_create_destroy,         false, NULL },
    { "irqfwd.refcount_lifecycle",     test_irqfwd_refcount_lifecycle,     false, NULL },
    { "irqfwd.wait_wakes_on_sgi",      test_irqfwd_wait_wakes_on_sgi,      false, NULL },
    { "irqfwd.collapses_concurrent_fires",
                                       test_irqfwd_collapses_concurrent_fires, false, NULL },
    { "irqfwd.second_waiter_refused",  test_irqfwd_second_waiter_refused,  false, NULL },
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
    { "9p_wire.tflush_round_trip",     test_9p_wire_tflush_round_trip,     false, NULL },
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
    { "9p_session.walk_fid_full_no_latch", test_9p_session_walk_fid_full_no_latch, false, NULL },
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
    { "9p_session.flush_reclaims_both",
                                       test_9p_session_flush_reclaims_both, false, NULL },
    { "9p_session.late_reply_does_not_free_awaiting_flush",
                                       test_9p_session_late_reply_does_not_free_awaiting_flush,
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
    { "9p_transport.deadline_idle_vs_eof",
                                       test_9p_transport_deadline_idle_vs_eof,
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
    { "9p_client.rlerror_hostile_ecode_bounded",
                                       test_9p_client_rlerror_hostile_ecode_bounded,
                                                                           false, NULL },
    { "9p_client.op_before_handshake_returns_ebusy",
                                       test_9p_client_op_before_handshake_returns_ebusy,
                                                                           false, NULL },
    { "9p_client.lock_released_between_ops",
                                       test_9p_client_lock_released_between_ops,
                                                                           false, NULL },
    { "9p_client.async_op_posts_cqe",  test_9p_client_async_op_posts_cqe,  false, NULL },
    { "9p_client.async_session_death_posts_error_cqe",
                                       test_9p_client_async_session_death_posts_error_cqe,
                                                                           false, NULL },
    { "9p_client.async_handoff_skips_async",
                                       test_9p_client_async_handoff_skips_async,
                                                                           false, NULL },
    { "9p_client.pump_deadline_idle",  test_9p_client_pump_deadline_idle,  false, NULL },
    { "9p_client.pump_deadline_data_ready_progresses",
                                       test_9p_client_pump_deadline_data_ready_progresses,
                                                                           false, NULL },
    { "9p_client.pump_deadline_chunked_frame_completes",
                                       test_9p_client_pump_deadline_chunked_frame_completes,
                                                                           false, NULL },
    { "9p_client.pump_deadline_busy_when_reader_active",
                                       test_9p_client_pump_deadline_busy_when_reader_active,
                                                                           false, NULL },
    { "9p_client.loom_fsync_e2e",      test_9p_client_loom_fsync_e2e,      false, NULL },
    { "9p_client.loom_rights_deny",    test_9p_client_loom_rights_deny,    false, NULL },
    { "9p_client.loom_quiesce_abandons_inflight",
                                       test_9p_client_loom_quiesce_abandons_inflight,
                                                                           false, NULL },
    { "9p_client.loom_multishot_stream",
                                       test_9p_client_loom_multishot_stream, false, NULL },
    { "9p_client.loom_multishot_backpressure",
                                       test_9p_client_loom_multishot_backpressure, false, NULL },
    { "9p_client.loom_link_cancel_cascade",
                                       test_9p_client_loom_link_cancel_cascade, false, NULL },
    { "9p_client.loom_link_success_ordering",
                                       test_9p_client_loom_link_success_ordering, false, NULL },
    { "9p_client.loom_drain_barrier",  test_9p_client_loom_drain_barrier,  false, NULL },
    { "9p_client.loom_independent_past_held",
                                       test_9p_client_loom_independent_past_held, false, NULL },
    { "9p_client.loom_drain_waits_for_rearm_pending",
                                       test_9p_client_loom_drain_waits_for_rearm_pending, false, NULL },
    { "9p_client.loom_read_e2e",       test_9p_client_loom_read_e2e,       false, NULL },
    { "9p_client.loom_write_e2e",      test_9p_client_loom_write_e2e,      false, NULL },
    { "9p_client.loom_rw_rejects",     test_9p_client_loom_rw_rejects,     false, NULL },
    { "9p_client.loom_readdir_e2e",    test_9p_client_loom_readdir_e2e,    false, NULL },
    { "9p_client.loom_readlink_e2e",   test_9p_client_loom_readlink_e2e,   false, NULL },
    { "9p_client.loom_getattr_e2e",    test_9p_client_loom_getattr_e2e,    false, NULL },
    { "9p_client.loom_statfs_e2e",     test_9p_client_loom_statfs_e2e,     false, NULL },
    { "9p_client.loom_metaread_rejects",
                                       test_9p_client_loom_metaread_rejects, false, NULL },
    { "9p_client.loom_mkdir_e2e",      test_9p_client_loom_mkdir_e2e,      false, NULL },
    { "9p_client.loom_setattr_e2e",    test_9p_client_loom_setattr_e2e,    false, NULL },
    { "9p_client.loom_renameat_e2e",   test_9p_client_loom_renameat_e2e,   false, NULL },
    { "9p_client.loom_mutation_rejects",
                                       test_9p_client_loom_mutation_rejects, false, NULL },
    { "9p_client.loom_multi_inflight_e2e",
                                       test_9p_client_loom_multi_inflight_e2e, false, NULL },
    { "9p_client.loom_multi_inflight_read_e2e",
                                       test_9p_client_loom_multi_inflight_read_e2e, false, NULL },
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
    { "dev9p.create_file",             test_dev9p_create_file,             false, NULL },
    { "dev9p.create_dir",              test_dev9p_create_dir,              false, NULL },
    { "dev9p.fsync",                   test_dev9p_fsync,                   false, NULL },
    { "dev9p.readdir",                 test_dev9p_readdir,                 false, NULL },
    { "dev9p.readdir_cookie_high_bit", test_dev9p_readdir_cookie_high_bit, false, NULL },
    { "dev9p.rename",                  test_dev9p_rename,                  false, NULL },
    { "dev9p.unlink",                  test_dev9p_unlink,                  false, NULL },
    { "dev9p.stat_native_maps_getattr",
                                       test_dev9p_stat_native_maps_getattr, false, NULL },
    { "dev9p.wstat_native_drives_setattr",
                                       test_dev9p_wstat_native_drives_setattr, false, NULL },
    { "dev9p.perm_enforced_deny_allow",
                                       test_dev9p_perm_enforced_deny_allow, false, NULL },
    { "p9_attached.create_destroy",    test_p9_attached_create_destroy,    false, NULL },
    { "p9_attached.handshake_failure_returns_null",
                                       test_p9_attached_handshake_failure_returns_null,
                                                                           false, NULL },
    { "p9_attached.handshake_rlerror_ecode_overflow_clamped",
                                       test_p9_attached_handshake_rlerror_ecode_overflow_clamped,
                                                                           false, NULL },
    { "p9_attached.root_spoor_walk_read",
                                       test_p9_attached_root_spoor_walk_read,
                                                                           false, NULL },
    { "p9_attached.query_helpers",     test_p9_attached_query_helpers,     false, NULL },
    { "p9_attached.walked_outlives_root_no_uaf",
                                       test_p9_attached_walked_outlives_root_no_uaf,
                                                                           false, NULL },
    { "sys_walk_open.max_length_name_nul_terminated",
                                       test_sys_walk_open_max_length_name_nul_terminated,
                                                                           false, NULL },
    { "spoor_transport.init_destroy",                       test_spoor_transport_init_destroy,                       false, NULL },
    { "spoor_transport.init_null_rejected",                 test_spoor_transport_init_null_rejected,                 false, NULL },
    { "spoor_transport.send_routes_to_tx_dev_write",        test_spoor_transport_send_routes_to_tx_dev_write,        false, NULL },
    { "spoor_transport.recv_routes_to_rx_dev_read",         test_spoor_transport_recv_routes_to_rx_dev_read,         false, NULL },
    { "spoor_transport.recv_empty_returns_zero",            test_spoor_transport_recv_empty_returns_zero,            false, NULL },
    { "spoor_transport.close_clunks_when_owned",            test_spoor_transport_close_clunks_when_owned,            false, NULL },
    { "spoor_transport.close_preserves_when_unowned",       test_spoor_transport_close_preserves_when_unowned,       false, NULL },
    { "spoor_transport.transport_core_round_trip",          test_spoor_transport_transport_core_round_trip,          false, NULL },
    { "spoor_transport.end_to_end_handshake",               test_spoor_transport_end_to_end_handshake,               false, NULL },
    { "9p_srvconn_transport.init_destroy",                  test_9p_srvconn_transport_init_destroy,                  false, NULL },
    { "9p_srvconn_transport.init_null_rejected",            test_9p_srvconn_transport_init_null_rejected,            false, NULL },
    { "9p_srvconn_transport.send_routes_to_c2s_ring",       test_9p_srvconn_transport_send_routes_to_c2s_ring,       false, NULL },
    { "9p_srvconn_transport.recv_routes_from_s2c_ring",     test_9p_srvconn_transport_recv_routes_from_s2c_ring,     false, NULL },
    { "9p_srvconn_transport.large_frame_roundtrip",         test_9p_srvconn_transport_large_frame_roundtrip,         false, NULL },
    { "9p_srvconn_transport.close_drops_srvconn_ref",       test_9p_srvconn_transport_close_drops_srvconn_ref,       false, NULL },
    { "9p_srvconn_transport.kernel_attached_skips_teardown_on_handle_close", test_9p_srvconn_transport_kernel_attached_skips_teardown_on_handle_close, false, NULL },
    { "9p_srvconn_transport.send_preserves_caller_deadline", test_9p_srvconn_transport_send_preserves_caller_deadline, false, NULL },
    { "9p_srvconn_transport.deadline_vtable_routes",        test_9p_srvconn_transport_deadline_vtable_routes,        false, NULL },
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
    { "poll.ready_immediately_pollin",          test_poll_ready_immediately_pollin,          false, NULL },
    { "poll.ready_immediately_pollout",         test_poll_ready_immediately_pollout,         false, NULL },
    { "poll.timeout_zero_not_ready",            test_poll_timeout_zero_not_ready,            false, NULL },
    { "poll.timeout_positive_fires",            test_poll_timeout_positive_fires,            false, NULL },
    { "poll.block_then_wake_pollin",            test_poll_block_then_wake_pollin,            false, NULL },
    { "poll.pollhup_on_close_write_end",        test_poll_pollhup_on_close_write_end,        false, NULL },
    { "poll.multi_fd_one_ready",                test_poll_multi_fd_one_ready,                false, NULL },
    { "poll.bad_fd_revents_pollnval",           test_poll_bad_fd_revents_pollnval,           false, NULL },
    { "poll.bad_args_rejected",                 test_poll_bad_args_rejected,                 false, NULL },
    { "poll.always_ready_null_dev_poll",        test_poll_always_ready_null_dev_poll,        false, NULL },
    { "poll.pollerr_on_write_after_read_close", test_poll_pollerr_on_write_after_read_close, false, NULL },
    { "poll.unregister_after_fast_path",        test_poll_unregister_after_fast_path,        false, NULL },
    { "poll.devsrv_listener_immediate_pollin",  test_poll_devsrv_listener_immediate_pollin,  false, NULL },
    { "poll.devsrv_listener_empty_not_ready",   test_poll_devsrv_listener_empty_not_ready,   false, NULL },
    { "poll.devsrv_listener_block_then_wake",   test_poll_devsrv_listener_block_then_wake,   false, NULL },
    { "poll.devsrv_listener_pollhup_on_tombstone", test_poll_devsrv_listener_pollhup_on_tombstone, false, NULL },
    { "poll.devsrv_conn_pollin_on_send",        test_poll_devsrv_conn_pollin_on_send,        false, NULL },
    { "poll.devsrv_conn_pollout_immediate",     test_poll_devsrv_conn_pollout_immediate,     false, NULL },
    { "poll.devsrv_conn_pollhup_on_teardown",   test_poll_devsrv_conn_pollhup_on_teardown,   false, NULL },
    { "poll.devsrv_conn_block_then_wake_pollin", test_poll_devsrv_conn_block_then_wake_pollin, false, NULL },
    { "poll.null_obj_spoor_pollnval",           test_poll_null_obj_spoor_pollnval,           false, NULL },
    { "poll.mixed_spoor_and_srv",               test_poll_mixed_spoor_and_srv,               false, NULL },
    { "poll.max_nfds",                          test_poll_max_nfds,                          false, NULL },
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
    { "chacha20.block_vector",                         test_chacha20_block_vector,                         false, NULL },
    { "chacha20.keystream_continuity",                 test_chacha20_keystream_continuity,                 false, NULL },
    { "kern_random.two_reads_differ",                  test_kern_random_two_reads_differ,                  false, NULL },
    { "kern_random.large_read_nonzero",                test_kern_random_large_read_nonzero,                false, NULL },
    { "kern_random.virtio_reseed",                     test_kern_random_virtio_reseed,                     false, NULL },
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
    { "sys_spawn_with_perms.zero_perm_is_spawn_full",  test_sys_spawn_with_perms_zero_perm_is_spawn_full,  false, NULL },
    { "sys_spawn_with_perms.console_attached_grants_may_post", test_sys_spawn_with_perms_console_attached_grants_may_post, false, NULL },
    { "sys_spawn_with_perms.rejects_non_console_attached_parent", test_sys_spawn_with_perms_rejects_non_console_attached_parent, false, NULL },
    { "sys_spawn_with_perms.rejects_unknown_perm_bits", test_sys_spawn_with_perms_rejects_unknown_perm_bits, false, NULL },
    { "sys_spawn_with_perms.holder_delegates_may_post", test_sys_spawn_with_perms_holder_delegates_may_post, false, NULL },
    { "sys_spawn_with_perms.console_trusted_not_delegable", test_sys_spawn_with_perms_console_trusted_not_delegable, false, NULL },
    { "sys_spawn_with_perms.console_owner_grant_gate",  test_sys_spawn_with_perms_console_owner_grant_gate,  false, NULL },
    { "sys_spawn_with_perms.console_owner_set_wiring",  test_sys_spawn_with_perms_console_owner_set_wiring,  false, NULL },
    { "sys_spawn_full_argv.no_argv_acts_as_spawn_with_perms", test_sys_spawn_full_argv_no_argv_acts_as_spawn_with_perms, false, NULL },
    { "sys_spawn_full_argv.golden_argc4",              test_sys_spawn_full_argv_golden_argc4,              false, NULL },
    { "sys_spawn_full_argv.rejects_argc_over_max",     test_sys_spawn_full_argv_rejects_argc_over_max,     false, NULL },
    { "sys_spawn_full_argv.rejects_data_len_over_max", test_sys_spawn_full_argv_rejects_data_len_over_max, false, NULL },
    { "sys_spawn_full_argv.rejects_missing_trailing_nul", test_sys_spawn_full_argv_rejects_missing_trailing_nul, false, NULL },
    { "sys_spawn_full_argv.rejects_nul_count_mismatch", test_sys_spawn_full_argv_rejects_nul_count_mismatch, false, NULL },
    { "sys_spawn_full_argv.rejects_argc_with_zero_data_len", test_sys_spawn_full_argv_rejects_argc_with_zero_data_len, false, NULL },
    { "sys_spawn_full_argv.rejects_zero_argc_with_nonzero_data", test_sys_spawn_full_argv_rejects_zero_argc_with_nonzero_data, false, NULL },
    // R1 F11 fix: console-attached gate test (uses proc_alloc to make
    // a fresh non-attached Proc, independent of kproc's flag state).
    { "sys_spawn_full_argv.rejects_non_console_attached_perm_flags", test_sys_spawn_full_argv_rejects_non_console_attached_perm_flags, false, NULL },
    { "sys_spawn_full_argv.validate_req_golden",       test_sys_spawn_full_argv_validate_req_golden,       false, NULL },
    { "sys_spawn_full_argv.validate_req_rejects_pad_envp", test_sys_spawn_full_argv_validate_req_rejects_pad_envp, false, NULL },
    { "sys_spawn_full_argv.validate_req_rejects_unknown_perm_bits", test_sys_spawn_full_argv_validate_req_rejects_unknown_perm_bits, false, NULL },
    { "sys_spawn_full_argv.validate_req_rejects_oversize_fields", test_sys_spawn_full_argv_validate_req_rejects_oversize_fields, false, NULL },
    { "userspace.stratumd_stub_round_trip",            test_stratumd_stub_round_trip,                      false, NULL },
    { "userspace.stratumd_stub_fs_round_trip",         test_stratumd_stub_fs_round_trip,                   false, NULL },
    { "userspace.stratumd_stub_walk_round_trip",       test_stratumd_stub_walk_round_trip,                 false, NULL },
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
    { "caps.rfork_strips_elevation_only",
                                       test_caps_rfork_strips_elevation_only,
                                                                           false, NULL },
    { "caps.rfork_inherits_legate_scope",
                                       test_caps_rfork_inherits_legate_scope,
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
    { "perm.check_owner_group_other",  test_perm_check_owner_group_other,  false, NULL },
    { "perm.check_owner_first",        test_perm_check_owner_first_authoritative, false, NULL },
    { "perm.check_hostowner_override", test_perm_check_hostowner_override, false, NULL },
    { "perm.in_group",                 test_perm_in_group,                 false, NULL },
    { "perm.want_for_omode",           test_perm_want_for_omode,           false, NULL },
    { "perm.rights_for_omode",         test_perm_rights_for_omode,         false, NULL },
    { "perm.oexec_no_read_leak",       test_perm_oexec_no_read_leak,       false, NULL },
    { "perm.wstat_policy",             test_perm_wstat_policy,             false, NULL },
    { "perm.devramfs_enforced_real_metadata",
                                       test_perm_devramfs_enforced_real_metadata, false, NULL },
    { "perm.dev_flags",                test_perm_dev_flags,                false, NULL },
    { "perm.check_dac_override_cap",   test_perm_check_dac_override_cap,   false, NULL },
    { "perm.wstat_chown_cap",          test_perm_wstat_chown_cap,          false, NULL },
    { "perm.check_want_zero_denied",   test_perm_check_want_zero_denied,   false, NULL },
    { "perm.wstat_rejects_unknown_valid_bit",
                                       test_perm_wstat_rejects_unknown_valid_bit, false, NULL },
    { "path.make_root",                test_path_make_root,                false, NULL },
    { "path.addelem_forms",            test_path_addelem_forms,            false, NULL },
    { "path.parent_forms",             test_path_parent_forms,             false, NULL },
    { "path.addelem_overflow_null",    test_path_addelem_overflow_null,    false, NULL },
    { "path.ref_balance",              test_path_ref_balance,              false, NULL },
    { "path.spoor_clone_shares",       test_path_spoor_clone_shares,       false, NULL },
    { "path.spoor_extend",             test_path_spoor_extend,             false, NULL },
    { "path.spoor_transplant",         test_path_spoor_transplant,         false, NULL },
    { "stalk.resolve_multi",           test_stalk_resolve_multi,           false, NULL },
    { "stalk.resolve_deep",            test_stalk_resolve_deep,            false, NULL },
    { "stalk.leading_and_double_slash", test_stalk_leading_and_double_slash, false, NULL },
    { "stalk.dot_noop",                test_stalk_dot_noop,                false, NULL },
    { "stalk.dotdot_pop",              test_stalk_dotdot_pop,              false, NULL },
    { "stalk.dotdot_containment",      test_stalk_dotdot_containment,      false, NULL },
    { "stalk.xsearch_deny",            test_stalk_xsearch_deny,            false, NULL },
    { "stalk.missing_component",       test_stalk_missing_component,       false, NULL },
    { "stalk.opath_no_open",           test_stalk_opath_no_open,           false, NULL },
    { "stalk.open_root",               test_stalk_open_root,               false, NULL },
    { "stalk.open_replace",            test_stalk_open_replace,            false, NULL },
    { "stalk.depth_cap",               test_stalk_depth_cap,               false, NULL },
    { "stalk.lifetime_no_leak",        test_stalk_lifetime_no_leak,        false, NULL },
    { "stalk.cross_mount",             test_stalk_cross_mount,             false, NULL },
    { "stalk.cross_mount_final_quarry", test_stalk_cross_mount_final_quarry, false, NULL },
    { "stalk.cross_mount_xsearch_deny", test_stalk_cross_mount_xsearch_deny, false, NULL },
    { "stalk.mount_amode_no_cross",    test_stalk_mount_amode_no_cross,    false, NULL },
    { "stalk.cross_mount_chain",       test_stalk_cross_mount_chain,       false, NULL },
    { "stalk.cross_mount_no_leak",     test_stalk_cross_mount_no_leak,     false, NULL },
    { "stalk.path_accumulate",         test_stalk_path_accumulate,         false, NULL },
    { "stalk.path_dotdot",             test_stalk_path_dotdot,             false, NULL },
    { "stalk.path_cross_transplant",   test_stalk_path_cross_transplant,   false, NULL },
    { "stalk.path_adopt_transplant",   test_stalk_path_adopt_transplant,   false, NULL },
    { NULL, NULL, false, NULL },          // sentinel
};

// ---------------------------------------------------------------------------
// Runner.
// ---------------------------------------------------------------------------

static struct test_case *current_test;
static unsigned passed_count, failed_count, total_count, soft_warn_count;

void test_fail(const char *msg) {
    if (current_test) {
        current_test->failed = true;
        current_test->fail_msg = msg;
    }
    // Dump the all-CPU runnable set on ANY test failure, so a scheduler-
    // quiescence assertion (sched_runnable_count()==0) self-documents which
    // thread is on which CPU. Runs only on the (rare) failure path -- no
    // passing-path cost, no timing detune. (This instrument cracked #857.)
    sched_dump_runnable(msg);
}

void test_soft_warn(const char *msg) {
    soft_warn_count++;
    uart_puts("[SOFT-WARN] ");
    uart_puts(msg ? msg : "(no message)");
    uart_puts("\n");
}

void test_run_all(void) {
    passed_count = 0;
    failed_count = 0;
    total_count  = 0;
    soft_warn_count = 0;

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
unsigned test_soft_warns(void) { return soft_warn_count; }
