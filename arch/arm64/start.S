// Thylacine kernel entry point — ARM64 / QEMU virt.
//
// Bootloader (QEMU's -kernel direct loader, or EDK2 UEFI, or U-Boot) hands us
// control at _start with:
//   - Exception level: EL1h (kernel mode, SP_EL1).
//     QEMU's -kernel boots directly at EL1; if we ever come in at EL2, the
//     EL2->EL1 drop sequence lands at P1-C / Pi 5 (ARCH §22.4). For P1-A we
//     assume EL1 and trap if we're wrong.
//   - x0 = DTB physical address (Linux ARM64 boot protocol convention).
//   - x1..x3 = 0 (reserved by Linux boot protocol).
//   - MMU off, caches off (per Linux protocol), interrupts masked.
//   - Single CPU executing this code (others are held by PSCI; brought up
//     in P1-F / Phase 2 with SMP).
//
// Our job at P1-A:
//   1. Verify EL1 (otherwise halt — P1-A doesn't handle EL2 entry).
//   2. Stash x0 (DTB pointer) in callee-saved x19 across BSS clear.
//   3. Set up SP to _boot_stack_top (defined by kernel.ld).
//   4. Clear BSS.
//   5. Save DTB pointer to _saved_dtb_ptr (now safe — BSS is cleared).
//   6. Branch to boot_main().
//   7. If boot_main() returns (it shouldn't), WFI loop forever.
//
// Per ARCHITECTURE.md §5 boot sequence and §22 hardware platform model.

.section ".text._start", "ax"
.globl _start
.type _start, %function
_start:
    // 1. Verify we're at EL1. CurrentEL[3:2] holds the EL number.
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    and     x1, x1, #3
    cmp     x1, #1
    b.ne    .Lnot_el1                  // EL2 / EL3 entry not supported at P1-A

    // 2. Stash DTB pointer in x19 (callee-saved; survives the BSS clear).
    mov     x19, x0

    // 3. Set up boot stack. SP grows down from _boot_stack_top.
    adrp    x0, _boot_stack_top
    add     x0, x0, :lo12:_boot_stack_top
    mov     sp, x0

    // 4. Clear BSS. [_bss_start, _bss_end), 8-byte stride.
    //    Linker script guarantees both bounds are 8-byte-aligned.
    adrp    x0, _bss_start
    add     x0, x0, :lo12:_bss_start
    adrp    x1, _bss_end
    add     x1, x1, :lo12:_bss_end
.Lbss_clear:
    cmp     x0, x1
    b.hs    .Lbss_done
    str     xzr, [x0], #8
    b       .Lbss_clear
.Lbss_done:

    // 5. Save DTB pointer. BSS is now cleared; this write sticks.
    adrp    x0, _saved_dtb_ptr
    add     x0, x0, :lo12:_saved_dtb_ptr
    str     x19, [x0]

    // 6. Branch to boot_main(). No args at P1-A; the DTB pointer is
    //    available via the saved_dtb_ptr global. P1-B may switch to
    //    passing it in x0 if that becomes natural.
    bl      boot_main

    // 7. boot_main() must not return. If it does, fall through to halt.
    b       _hang

.size _start, . - _start

// ---------------------------------------------------------------------------
// _hang — clean halt loop. WFI until an external event (which won't come
// at P1-A; interrupts are masked and there's no IRQ source).
//
// Used by:
//   - Post-boot_main() fallthrough (boot_main shouldn't return).
//   - .Lnot_el1 (wrong-EL entry).
//   - Future panic handler (P1-C+).
// ---------------------------------------------------------------------------
.section ".text", "ax"
.globl _hang
.type _hang, %function
_hang:
1:  wfi
    b       1b
.size _hang, . - _hang

// ---------------------------------------------------------------------------
// .Lnot_el1 — wrong-EL entry. Can't print yet (UART access from EL2/EL3
// would need different setup, and we haven't initialized state). Halt
// silently; the developer notices the silent boot in QEMU.
//
// P1-C will replace this with a proper EL2->EL1 drop sequence + a
// visible diagnostic via PL011 (memory-mapped flat at 0x09000000 on
// QEMU virt; accessible from any EL since MMU is off).
// ---------------------------------------------------------------------------
.section ".text", "ax"
.Lnot_el1:
    b       _hang

// ---------------------------------------------------------------------------
// _saved_dtb_ptr — DTB physical address handed to us by the bootloader.
// BSS-allocated (zeroed at boot), populated by _start AFTER the BSS clear.
// Read by boot_main() to print in the boot banner; consumed by the DTB
// parser at P1-B.
// ---------------------------------------------------------------------------
.section ".bss", "aw", %nobits
.globl _saved_dtb_ptr
.type _saved_dtb_ptr, %object
.align 3
_saved_dtb_ptr:
    .skip 8
.size _saved_dtb_ptr, 8
