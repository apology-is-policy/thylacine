# Thylacine kernel build.
#
# Produces build/kernel/thylacine.elf — a static ARM64 ELF that QEMU's -kernel
# flag loads directly.
#
# Per ROADMAP.md §4 (Phase 1): kernel skeleton. P1-A produces the minimal boot
# stub; subsequent sub-chunks add MMU, KASLR, allocator, etc.

set(KERNEL_SRCS
    main.c
    ../arch/arm64/start.S
    ../arch/arm64/uart.c
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

# Post-build: print kernel size for the boot-time-budget conversation.
add_custom_command(TARGET thylacine.elf POST_BUILD
    COMMAND ${CMAKE_NM} --print-size --size-sort --radix=d $<TARGET_FILE:thylacine.elf> | tail -10
    COMMAND ${CMAKE_OBJDUMP} --section-headers $<TARGET_FILE:thylacine.elf>
    COMMENT "Kernel size summary"
    VERBATIM
)
