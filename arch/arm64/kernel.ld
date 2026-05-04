/*
 * Thylacine kernel linker script — ARM64 / QEMU virt.
 *
 * Layout decisions (ARCHITECTURE.md §5, §6):
 *
 * - Load address: 0x40080000. QEMU virt RAM starts at 0x40000000; the first
 *   512 KiB is conventionally reserved for early firmware (UEFI / U-Boot /
 *   shadow ROM). Linux ARM64 uses the same offset.
 *
 * - Section order: .text, .rodata, .data, .bss. Each starts on a 4 KiB page
 *   boundary so future MMU page-protection (P1-C) can give .text RX,
 *   .rodata R, .data RW, .bss RW with no inter-section spillover. W^X
 *   invariant (I-12) starts here even though MMU isn't enabled yet.
 *
 * - BSS-allocated boot stack: 16 KiB (THREAD_STACK_SIZE per ARCH §7.3 future
 *   commitment). Aligned to a page; a 4 KiB guard page sits BELOW the stack
 *   at _boot_stack_guard. mmu.c zeroes its L3 PTE so a stack overflow into
 *   the guard region faults synchronously instead of silently corrupting
 *   BSS. The guard page is in BSS and zero-cleared by the BSS clear loop;
 *   it becomes unreachable only after mmu_enable() runs.
 *
 * - At P1-A, the kernel runs at the load address with no relocation. KASLR
 *   relocation lands at P1-C-extras; PIE flags are off in the toolchain
 *   file at this sub-chunk and get flipped on when KASLR lands.
 *
 * Symbols exported for C code (see arch/arm64/start.S references):
 *   _kernel_start    — first byte of the kernel image (== load address)
 *   _kernel_end      — first byte past the kernel image
 *   _bss_start, _bss_end — BSS bounds (start.S clears this range)
 *   _boot_stack_guard    — start of the 4 KiB stack guard page
 *   _boot_stack_bottom   — first usable stack byte (== _boot_stack_guard + 4 KiB)
 *   _boot_stack_top      — top of the boot stack (start.S sets SP to this)
 */

ENTRY(_start)

KERNEL_LOAD_ADDR = 0x40080000;
PAGE_SIZE        = 0x1000;       /* 4 KiB */
BOOT_STACK_SIZE  = 0x4000;       /* 16 KiB; matches ARCH §7.3 thread stack */

SECTIONS
{
    . = KERNEL_LOAD_ADDR;
    _kernel_start = .;

    /* .text — kernel code. MMU mapping: RX (P1-C and onward). */
    .text : ALIGN(PAGE_SIZE) {
        KEEP(*(.text._start))      /* _start MUST be the first instruction */
        *(.text .text.*)
    }
    . = ALIGN(PAGE_SIZE);
    _text_end = .;

    /* .rodata — read-only data. MMU mapping: R (P1-C and onward). */
    .rodata : ALIGN(PAGE_SIZE) {
        *(.rodata .rodata.*)
    }
    . = ALIGN(PAGE_SIZE);
    _rodata_end = .;

    /* .data — initialized writable data. MMU mapping: RW. */
    .data : ALIGN(PAGE_SIZE) {
        *(.data .data.*)
    }
    . = ALIGN(PAGE_SIZE);
    _data_end = .;

    /* .bss — zero-initialized data. start.S clears [_bss_start, _bss_end). */
    .bss : ALIGN(PAGE_SIZE) {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)

        /* Stack guard page. mmu.c zeroes the L3 PTE for [_boot_stack_guard,
         * _boot_stack_bottom) so a stack overflow faults synchronously
         * (translation fault, FAR_EL1 inside the guard region) instead of
         * silently corrupting prior BSS. The page is BSS-allocated and
         * zero-cleared at boot; once mmu_enable() runs, it's unreachable
         * via TTBR0/TTBR1. P1-F's fault handler will recognize FAR_EL1
         * inside this region as a stack-overflow extinction. */
        . = ALIGN(PAGE_SIZE);
        _boot_stack_guard = .;
        . += PAGE_SIZE;

        /* Boot stack lives in BSS so it's zeroed at startup. SP grows down,
         * so the "top" pointer in start.S is _boot_stack_top, which is the
         * highest address — i.e. the END of the buffer. */
        _boot_stack_bottom = .;
        . += BOOT_STACK_SIZE;
        _boot_stack_top = .;

        _bss_end = .;
    }

    . = ALIGN(PAGE_SIZE);
    _kernel_end = .;

    /* Discard sections we don't want in the final image. */
    /DISCARD/ : {
        *(.note.*)
        *(.comment)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.ARM.attributes)
    }
}

/*
 * Linker-computed size symbol. The Linux ARM64 image header in start.S
 * needs `image_size = _kernel_end - _kernel_start` at offset 0x10 of the
 * binary, but the assembler cannot evaluate subtraction of external
 * symbols. Defining the difference here as a linker-resolved absolute
 * symbol lets start.S do `.quad _image_size` directly.
 *
 * Per Linux ARM64 booting protocol §1: image_size includes BSS so that
 * a bootloader allocating image_size bytes from the load address has
 * space for the kernel's full memory footprint.
 */
_image_size = _kernel_end - _kernel_start;


/* Sanity assertions: the linker errors out if these are violated. */
ASSERT(_kernel_start == KERNEL_LOAD_ADDR,
       "Kernel must load at KERNEL_LOAD_ADDR (0x40080000)")
ASSERT((_boot_stack_top - _boot_stack_bottom) == BOOT_STACK_SIZE,
       "Boot stack size mismatch")
ASSERT((_boot_stack_bottom - _boot_stack_guard) == PAGE_SIZE,
       "Boot stack guard page must be exactly 4 KiB")
ASSERT((_kernel_end - _kernel_start) < 0x200000,
       "Kernel image > 2 MiB; the L3 kernel mapping covers exactly 2 MiB")
