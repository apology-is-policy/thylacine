# Thylacine kernel build.
#
# Produces build/kernel/thylacine.elf — a static ARM64 ELF that QEMU's -kernel
# flag loads directly.
#
# Per ROADMAP.md §4 (Phase 1): kernel skeleton. P1-A produces the minimal boot
# stub; subsequent sub-chunks add MMU, KASLR, allocator, etc.

# KERNEL_TESTS (#61, RW-11 R4-F1): the production boot shape. ON (default) keeps
# the in-kernel test suite -- test/*.c compiled + g_tests[] run at boot by
# test_run_all() (kernel/main.c) -- which is the dev/CI shape every gate depends
# on. OFF excludes test/*.c entirely and (via the KERNEL_TESTS compile define)
# skips the boot-time test_run_all() + the boot-stack-report probe, yielding the
# lean production kernel the V1.0 boot-time budget targets. Set OFF by
# tools/build.sh --production (which flips the joey THYLA_BOOT_PROBES gate in
# lockstep). Collides with nothing in ARCHITECTURE.md §28: it gates only test
# scaffolding, never a real boot-ordering step or an invariant-bearing path.
option(KERNEL_TESTS "Compile + run the in-kernel test suite at boot" ON)

set(KERNEL_SRCS
    main.c
    extinction.c
    canary.c
    fault_test.c
    proc.c
    perm.c
    thread.c
    sched.c
    smp.c
    ../arch/arm64/start.S
    ../arch/arm64/uart.c
    ../arch/arm64/mmu.c
    ../arch/arm64/kaslr.c
    ../arch/arm64/exception.c
    ../arch/arm64/fault.c
    ../arch/arm64/halls.c
    ../arch/arm64/uaccess.c
    ../arch/arm64/uaccess.S
    ../arch/arm64/vectors.S
    ../arch/arm64/gic.c
    ../arch/arm64/timer.c
    ../arch/arm64/rtc.c
    ../arch/arm64/hwfeat.c
    ../arch/arm64/alternatives.c
    ../arch/arm64/context.S
    ../arch/arm64/userland.S
    ../arch/arm64/psci.c
    ../arch/arm64/asid.c
    ../lib/dtb.c
    ../mm/buddy.c
    ../mm/magazines.c
    ../mm/phys.c
    ../mm/slub.c
    territory.c
    handle.c
    burrow.c
    loom.c
    vma.c
    elf.c
    exec.c
    syscall.c
    joey.c
    spoor.c
    path.c
    stalk.c
    dev.c
    devnone.c
    cons.c
    null.c
    zero.c
    full.c
    random.c
    chacha20.c
    devproc.c
    devctl.c
    devramfs.c
    devsrv.c
    devcap.c
    devdev.c
    cpio.c
    virtio.c
    virtio_pci.c
    irqfwd.c
    mmio_handle.c
    dma_handle.c
    9p_wire.c
    9p_session.c
    9p_transport.c
    9p_client.c
    dev9p.c
    9p_attach.c
    9p_spoor_transport.c
    9p_srvconn_transport.c
    pipe.c
    poll.c
    srvconn.c
    torpor.c
    notes.c
    devnotes.c
)

# The in-kernel test suite (gated by KERNEL_TESTS). Every test/*.c registers
# into g_tests[]; test_run_all() (kernel/main.c) executes them at boot. Kept in
# its own list so the production shape (KERNEL_TESTS=OFF) drops the whole suite
# from the link without touching the production source list above.
set(KERNEL_TEST_SRCS
    test/test.c
    test/test_kaslr.c
    test/test_dtb.c
    test/test_phys.c
    test/test_slub.c
    test/test_gic.c
    test/test_timer.c
    test/test_clock.c
    test/test_hardening.c
    test/test_alternatives.c
    test/test_context.c
    test/test_fp.c
    test/test_sched.c
    test/test_rendez.c
    test/test_tsleep.c
    test/test_smp.c
    test/test_proc.c
    test/test_proc_identity.c
    test/test_resource.c
    test/test_exec_namespace.c
    test/test_namespace_layout.c
    test/test_territory.c
    test/test_territory_mount.c
    test/test_territory_chroot.c
    test/test_handle.c
    test/test_burrow.c
    test/test_elf.c
    test/test_asid.c
    test/test_directmap.c
    test/test_proc_pgtable.c
    test/test_fault_decode.c
    test/test_halls.c
    test/test_vma.c
    test/test_burrow_map_proc.c
    test/test_demand_page.c
    test/test_exec.c
    test/test_syscall.c
    test/test_dev.c
    test/test_trivial_devs.c
    test/test_devproc.c
    test/test_cons.c
    test/test_devctl.c
    test/test_devdev.c
    test/test_cpio.c
    test/test_devramfs.c
    test/test_devsrv.c
    test/test_devcap.c
    test/test_virtio.c
    test/test_random.c
    test/test_virtio_pci.c
    test/test_irqfwd.c
    test/test_userspace2.c
    test/test_userspace_ramfs.c
    test/test_caps.c
    test/test_mmio_handle.c
    test/test_dma_handle.c
    test/test_handle_hw.c
    test/test_burrow_mmio.c
    test/test_mmio_map.c
    test/test_mmio_probe.c
    test/test_irq_probe.c
    test/test_virtio_blk_probe.c
    test/test_virtio_blk_rw.c
    test/test_virtio_net_probe.c
    test/test_virtio_net_arp.c
    test/test_virtio_net_loop.c
    test/test_virtio_input_probe.c
    test/test_virtio_gpu_probe.c
    test/test_uaccess.c
    test/test_irq_latency_bench.c
    test/test_driver_crash_recovery.c
    test/test_9p_wire.c
    test/test_9p_session.c
    test/test_9p_transport.c
    test/test_9p_client.c
    test/test_dev9p.c
    test/test_perm.c
    test/test_path.c
    test/test_stalk.c
    test/test_9p_attach.c
    test/test_9p_spoor_transport.c
    test/test_9p_srvconn_transport.c
    test/test_territory_pivot_root.c
    test/test_pipe.c
    test/test_pipe_blocking.c
    test/test_poll.c
    test/test_srvconn.c
    test/test_devsrv_conn.c
    test/test_srv_client.c
    test/test_sys_pipe.c
    test/test_sys_mount.c
    test/test_sys_corvus.c
    test/test_sys_spawn.c
    test/test_sys_spawn_with_fds.c
    test/test_sys_spawn_with_caps.c
    test/test_sys_spawn_full.c
    test/test_sys_spawn_with_perms.c
    test/test_sys_spawn_full_argv.c
    test/test_sys_burrow.c
    test/test_torpor.c
    test/test_loom.c
    test/test_notes.c
    test/test_thread_spawn.c
    test/test_pipe_probe.c
    test/test_attach_probe.c
    test/test_stratumd_stub.c
    test/test_stub_driver.c
    # Test-only transport infrastructure (not under test/, but referenced only by
    # the 9P/Loom test cases -- #61 F2). 9p_transport_mq is the Loom-6c
    # multi-in-flight test transport; 9p_transport_loopback is the single-slot
    # test transport it builds on. No production code references either.
    9p_transport_loopback.c
    9p_transport_mq.c
)

# HX-2: in-kernel symbol table (kallsyms-style). Generated PER-BUILD-DIR from
# the linked ELF by tools/gen-halls-symtab.py (driven by tools/build.sh's
# two-pass build) -- it is build-specific (the default + sanitizer builds have
# different .text layouts), so it lives under the build dir, never the source
# tree. The committed stub (count=0 -> halls_symbolize() returns NULL -> a
# graceful HX-1 raw+link fallback) seeds the build-dir copy at first configure
# so a bare cmake / IDE build still links + runs; build.sh then regenerates the
# real table + re-links. The NOT-EXISTS guard keeps a re-configure from
# clobbering the regenerated table.
set(HALLS_SYMTAB_GEN ${CMAKE_BINARY_DIR}/generated/halls_symtab.c)
if(NOT EXISTS ${HALLS_SYMTAB_GEN})
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/../arch/arm64/halls_symtab.stub.c
        ${HALLS_SYMTAB_GEN}
        COPYONLY)
endif()

set(KERNEL_LD_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/../arch/arm64/kernel.ld)

add_executable(thylacine.elf ${KERNEL_SRCS} ${HALLS_SYMTAB_GEN})

# The in-kernel test suite is opt-in (ON by default). When ON, the test sources
# join the link and KERNEL_TESTS is defined so kernel/main.c runs test_run_all()
# + boot_stack_report() and sched.c compiles its test-only accessors. When OFF
# (tools/build.sh --production), none of that scaffolding is compiled.
if(KERNEL_TESTS)
    target_sources(thylacine.elf PRIVATE ${KERNEL_TEST_SRCS})
    target_compile_definitions(thylacine.elf PRIVATE KERNEL_TESTS)
    message(STATUS "Kernel: in-kernel test suite ENABLED (KERNEL_TESTS=ON)")
else()
    message(STATUS "Kernel: production shape -- in-kernel test suite DISABLED (KERNEL_TESTS=OFF)")
endif()

target_include_directories(thylacine.elf
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../arch/arm64
)

# Custom linker script. -T tells the linker to use it instead of the default.
target_link_options(thylacine.elf
    PRIVATE
        -T${KERNEL_LD_SCRIPT}
        -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/thylacine.map
)

# Re-link if the linker script changes.
set_property(TARGET thylacine.elf
    APPEND PROPERTY LINK_DEPENDS ${KERNEL_LD_SCRIPT}
)

# Conventionally place the ELF at build/kernel/thylacine.elf so tools/run-vm.sh
# can find it without args.
set_target_properties(thylacine.elf PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    SUFFIX ""
)

# Post-build:
# 1. Convert the ELF to a flat binary so QEMU's load_aarch64_image() picks it
#    up as a Linux ARM64 Image kernel (which loads the DTB and passes it in
#    x0; ELF load skips the DTB — see docs/reference/01-boot.md). The flat
#    binary lives alongside the ELF; tools/run-vm.sh uses the .bin.
# 2. Print kernel size summary for the boot-time-budget conversation.
add_custom_command(TARGET thylacine.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary
        $<TARGET_FILE:thylacine.elf>
        $<TARGET_FILE_DIR:thylacine.elf>/thylacine.bin
    COMMAND ${CMAKE_COMMAND} -E echo
        "Flat binary: $<TARGET_FILE_DIR:thylacine.elf>/thylacine.bin"
    COMMAND ls -la
        $<TARGET_FILE_DIR:thylacine.elf>/thylacine.elf
        $<TARGET_FILE_DIR:thylacine.elf>/thylacine.bin
    COMMAND ${CMAKE_NM} --print-size --size-sort --radix=d $<TARGET_FILE:thylacine.elf> | tail -10
    COMMENT "Kernel post-link: flat binary + size summary"
    VERBATIM
)
