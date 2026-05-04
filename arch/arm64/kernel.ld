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
 *   commitment). Aligned to a page; a guard page below it is added at P1-C
 *   when the MMU lands and we can mark a page non-present.
 *
 * - At P1-A, the kernel runs at the load address with no relocation. KASLR
 *   relocation lands at P1-C; PIE flags are off in the toolchain file.
 *
 * Symbols exported for C code (see arch/arm64/start.S references):
 *   _kernel_start    — first byte of the kernel image (== load address)
 *   _kernel_end      — first byte past the kernel image
 *   _bss_start, _bss_end — BSS bounds (start.S clears this range)
 *   _boot_stack_top  — top of the boot stack (start.S sets SP to this)
 */

ENTRY(_start)

KERNEL_LOAD_ADDR = 0x40080000;
PAGE_SIZE        = 0x1000;       /* 4 KiB */
BOOT_STACK_SIZE  = 0x4000;       /* 16 KiB; matches ARCH §7.3 thread stack */

SECTIONS
{
    . = KERNEL_LOAD_ADDR;
    _kernel_start = .;

    /* .text — kernel code. Future MMU mapping: RX. */
    .text : ALIGN(PAGE_SIZE) {
        KEEP(*(.text._start))      /* _start MUST be the first instruction */
        *(.text .text.*)
    }

    /* .rodata — read-only data. Future MMU mapping: R. */
    .rodata : ALIGN(PAGE_SIZE) {
        *(.rodata .rodata.*)
    }

    /* .data — initialized writable data. Future MMU mapping: RW. */
    .data : ALIGN(PAGE_SIZE) {
        *(.data .data.*)
    }

    /* .bss — zero-initialized data. start.S clears [_bss_start, _bss_end). */
    .bss : ALIGN(PAGE_SIZE) {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)

        /* Boot stack lives in BSS so it's zeroed at startup. SP grows down,
         * so the "top" pointer in start.S is _boot_stack_top, which is the
         * highest address — i.e. the END of the buffer. */
        . = ALIGN(PAGE_SIZE);
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

/* Sanity assertions: the linker errors out if these are violated. */
ASSERT(_kernel_start == KERNEL_LOAD_ADDR,
       "Kernel must load at KERNEL_LOAD_ADDR (0x40080000)")
ASSERT((_boot_stack_top - _boot_stack_bottom) == BOOT_STACK_SIZE,
       "Boot stack size mismatch")
ASSERT((_kernel_end - _kernel_start) < 0x100000,
       "Kernel image > 1 MiB at P1-A; investigate before continuing")
