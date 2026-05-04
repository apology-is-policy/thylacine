// Thylacine kernel entry point — ARM64 / QEMU virt.
//
// Image format: Linux ARM64 Image (Documentation/arch/arm64/booting.rst).
// The 64-byte header at offset 0 lets QEMU's load_aarch64_image() detect
// us as a Linux-shaped kernel and pass DTB pointer in x0 per the standard
// ARM64 Linux boot protocol. Without this header, QEMU's ELF-load path
// runs (is_linux=0) and the DTB is never loaded into RAM — see commit
// log for P1-B for the investigation.
//
// Bootloader (QEMU's load_aarch64_image, EDK2 UEFI, U-Boot's bootm) hands
// control at the start of the image (which is _start, where code0 lives)
// with:
//   - Exception level: EL1h (kernel mode, SP_EL1).
//     QEMU virt boots at EL1; if we ever come in at EL2 (Pi 5 bare metal
//     post-v1.0), the EL2->EL1 drop sequence lands at P1-C / Pi 5.
//   - x0 = DTB physical address (ARM64 Linux boot protocol).
//   - x1..x3 = 0 (reserved).
//   - MMU off, caches off, interrupts masked.
//   - Single CPU executing this code; secondaries held by PSCI.
//
// Header layout per Linux ARM64 booting protocol:
//   0x00  u32  code0       executable: branch to _real_start
//   0x04  u32  code1       executable: reserved-as-nop
//   0x08  u64  text_offset image load offset (informational w/ flag bit 3)
//   0x10  u64  image_size  total image size (text + rodata + data + bss)
//   0x18  u64  flags       endian/page-size/placement flags
//   0x20  u64  res2        0
//   0x28  u64  res3        0
//   0x30  u64  res4        0
//   0x38  u32  magic       0x644d5241 ("ARM\x64", little-endian)
//   0x3C  u32  res5        0
//   0x40  ...              _real_start

.section ".text._start", "ax"
.globl _start
.type _start, %function
_start:
    // 0x00: code0 — branch to _real_start (at offset 0x40, past the header).
    b       _real_start
    // 0x04: code1 — reserved (must be valid code; nop is fine).
    nop
    // 0x08: text_offset. With flag bit 3 set (placement-anywhere), this
    // is informational only. Use 0x80000 to match Linux convention.
    .quad   0x0000000000080000
    // 0x10: image_size. Linker resolves this absolute symbol to
    // (_kernel_end - _kernel_start); see kernel.ld.
    .quad   _image_size
    // 0x18: flags.
    //   bit 0 = 0  little-endian
    //   bits 1-2 = 1  4 KiB granule
    //   bit 3 = 1  placement-anywhere (kernel can be loaded at any 2 MiB-
    //              aligned address; we trust the bootloader)
    //   bits 4-63 = 0
    // = 0x000000000000000a
    .quad   0x000000000000000a
    // 0x20-0x37: reserved.
    .quad   0
    .quad   0
    .quad   0
    // 0x38: magic "ARM\x64" little-endian = 0x644d5241.
    .long   0x644d5241
    // 0x3C: reserved.
    .long   0

.globl _real_start
.type _real_start, %function
_real_start:
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

    // 6. Enable the MMU. After this returns, the kernel runs with caches
    //    enabled and per-section page protections in force (W^X invariant
    //    I-12 enforced at PTE bit level). Identity-mapped low 4 GiB so
    //    PCs and SP remain valid through the transition.
    bl      mmu_enable

    // 7. Branch to boot_main().
    bl      boot_main

    // 7. boot_main() must not return. If it does, fall through to halt.
    b       _hang

.size _start, . - _start
.size _real_start, . - _real_start

// ---------------------------------------------------------------------------
// _hang — clean halt loop. WFI until an external event.
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
// would need different setup). Halt silently; the developer notices the
// silent boot in QEMU.
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
// BSS-allocated (zeroed at boot), populated by _real_start AFTER the BSS
// clear. Read by boot_main() to print in the boot banner; consumed by the
// DTB parser (P1-B's lib/dtb.c).
// ---------------------------------------------------------------------------
.section ".bss", "aw", %nobits
.globl _saved_dtb_ptr
.type _saved_dtb_ptr, %object
.align 3
_saved_dtb_ptr:
    .skip 8
.size _saved_dtb_ptr, 8
