# Thylacine kernel build.
#
# Produces build/kernel/thylacine.elf — a static ARM64 ELF that QEMU's -kernel
# flag loads directly.
#
# Per ROADMAP.md §4 (Phase 1): kernel skeleton. P1-A produces the minimal boot
# stub; subsequent sub-chunks add MMU, KASLR, allocator, etc.

set(KERNEL_SRCS
    main.c
    extinction.c
    canary.c
    fault_test.c
    proc.c
    thread.c
    sched.c
    smp.c
    ../arch/arm64/start.S
    ../arch/arm64/uart.c
    ../arch/arm64/mmu.c
    ../arch/arm64/kaslr.c
    ../arch/arm64/exception.c
    ../arch/arm64/fault.c
    ../arch/arm64/uaccess.c
    ../arch/arm64/uaccess.S
    ../arch/arm64/vectors.S
    ../arch/arm64/gic.c
    ../arch/arm64/timer.c
    ../arch/arm64/hwfeat.c
    ../arch/arm64/context.S
    ../arch/arm64/userland.S
    ../arch/arm64/psci.c
    ../arch/arm64/asid.c
    ../lib/dtb.c
    ../mm/buddy.c
    ../mm/magazines.c
    ../mm/phys.c
    ../mm/slub.c
    test/test.c
    test/test_kaslr.c
    test/test_dtb.c
    test/test_phys.c
    test/test_slub.c
    test/test_gic.c
    test/test_timer.c
    test/test_hardening.c
    test/test_context.c
    test/test_fp.c
    test/test_sched.c
    test/test_rendez.c
    test/test_smp.c
    test/test_proc.c
    test/test_territory.c
    test/test_territory_mount.c
    test/test_handle.c
    test/test_burrow.c
    test/test_elf.c
    test/test_asid.c
    test/test_directmap.c
    test/test_proc_pgtable.c
    test/test_fault_decode.c
    test/test_vma.c
    test/test_burrow_map_proc.c
    test/test_demand_page.c
    test/test_exec.c
    test/test_syscall.c
    territory.c
    handle.c
    burrow.c
    vma.c
    elf.c
    exec.c
    syscall.c
    joey.c
    spoor.c
    dev.c
    devnone.c
    cons.c
    null.c
    zero.c
    random.c
    devproc.c
    devctl.c
    devramfs.c
    cpio.c
    virtio.c
    virtio_pci.c
    irqfwd.c
    mmio_handle.c
    dma_handle.c
    9p_wire.c
    9p_session.c
    9p_transport.c
    9p_transport_loopback.c
    9p_client.c
    dev9p.c
    9p_attach.c
    9p_spoor_transport.c
    pipe.c
    test/test_dev.c
    test/test_trivial_devs.c
    test/test_devproc.c
    test/test_devctl.c
    test/test_cpio.c
    test/test_devramfs.c
    test/test_virtio.c
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
    test/test_9p_attach.c
    test/test_9p_spoor_transport.c
    test/test_pipe.c
    test/test_pipe_blocking.c
    test/test_sys_pipe.c
    test/test_sys_mount.c
    test/test_sys_corvus.c
    test/test_sys_spawn.c
    test/test_sys_spawn_with_fds.c
    test/test_pipe_probe.c
    test/test_attach_probe.c
    test/test_stratumd_stub.c
    test/test_stub_driver.c
)

set(KERNEL_LD_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/../arch/arm64/kernel.ld)

add_executable(thylacine.elf ${KERNEL_SRCS})

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
