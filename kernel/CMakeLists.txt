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
    ../arch/arm64/start.S
    ../arch/arm64/uart.c
    ../arch/arm64/mmu.c
    ../arch/arm64/kaslr.c
    ../arch/arm64/exception.c
    ../arch/arm64/vectors.S
    ../arch/arm64/gic.c
    ../arch/arm64/timer.c
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
