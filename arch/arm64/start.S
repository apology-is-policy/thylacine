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
//   - Exception level: EL1h on QEMU virt; EL2h on Pi 5 bare metal.
//     The kernel handles both: at EL1 we proceed directly; at EL2 we
//     drop to EL1 via the canonical SPSR_EL2 / ELR_EL2 / eret sequence
//     (P1-C-extras). EL3 / EL0 entry is not supported and halts silently
//     (no UART available without a stack + DTB-discovered base).
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
    // 1. Determine current Exception Level.
    //
    // CurrentEL[3:2] holds the EL number. We accept EL1 (proceed) and
    // EL2 (drop to EL1). EL0 and EL3 are unexpected; halt silently.
    //
    // After this branch:
    //   x20 = 0 if entered at EL1 directly
    //   x20 = 1 if dropped from EL2
    //   x0  = DTB pointer (preserved across the EL2 drop via eret's
    //         general-register passthrough)
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    and     x1, x1, #3
    cmp     x1, #2
    b.eq    .Lat_el2
    cmp     x1, #1
    b.ne    _hang                       // EL3 or EL0 — silent halt
    mov     x20, #0
    b       .Lel1_main

// ---------------------------------------------------------------------------
// EL2 → EL1 drop sequence.
//
// Canonical ordering per ARM ARM D1.6 (Process state on exception return):
//
//   1. HCR_EL2.RW = 1  — EL1 is AArch64 (not AArch32).
//   2. CNTHCTL_EL2.{EL1PCEN, EL1PCTEN} = 1  — let EL1 read physical
//      timer + counter without trapping. (Used at P1-G.)
//   3. CNTVOFF_EL2 = 0  — no virtual time offset.
//   4. VTTBR_EL2 = 0  — no Stage 2 translation (we're not a hypervisor).
//   5. SCTLR_EL1 = INIT_SCTLR_EL1_MMU_OFF  — RES1 bits set, MMU off,
//      caches off, little-endian. Bits 11, 20, 22, 23, 28, 29 are
//      RES1 in ARMv8.0+ (per ARM ARM D13.2.118). Composite = 0x30D00800.
//   6. SPSR_EL2 = 0x3C5  — EL1h (M[3:0]=0b0101), DAIF masked
//      (D|A|I|F = bits 9..6 = 1).
//   7. ELR_EL2 = .Lpost_el2_drop  — return target.
//   8. eret — drop to EL1, GP regs (incl. x0 = DTB) preserved.
//
// On Pi 5 (bare metal, EL2 entry, post-v1.0) this gets us into EL1 with
// the DTB still in x0. On QEMU virt this branch is dead code (we always
// enter at EL1) but exercising the sequence in code review keeps it
// honest until Pi 5 work picks it up. The diagnostic — that the kernel
// dropped from EL2 — is published in the boot banner via _entered_at_el2.
// ---------------------------------------------------------------------------
.Lat_el2:
    // 1. HCR_EL2.RW = 1.
    mov     x1, #(1 << 31)
    msr     hcr_el2, x1

    // 2. CNTHCTL_EL2.{EL1PCEN, EL1PCTEN} = 1.
    mrs     x1, cnthctl_el2
    orr     x1, x1, #3
    msr     cnthctl_el2, x1

    // 3. CNTVOFF_EL2 = 0.
    msr     cntvoff_el2, xzr

    // 4. VTTBR_EL2 = 0.
    msr     vttbr_el2, xzr

    // 5. SCTLR_EL1 = INIT_SCTLR_EL1_MMU_OFF (0x30D00800).
    //    bits 11, 20, 22, 23, 28, 29 = 1 (RES1); everything else 0.
    movz    x1, #0x0800, lsl #0
    movk    x1, #0x30D0, lsl #16
    msr     sctlr_el1, x1

    // 6. SPSR_EL2 = 0x3C5 (EL1h, DAIF masked).
    mov     x1, #0x3C5
    msr     spsr_el2, x1

    // 7. ELR_EL2 = .Lpost_el2_drop.
    adrp    x1, .Lpost_el2_drop
    add     x1, x1, :lo12:.Lpost_el2_drop
    msr     elr_el2, x1

    // 8. Drop. eret reads SPSR_EL2.M to set target EL, jumps to ELR_EL2.
    //    GP registers (incl. x0 = DTB pointer) are preserved.
    eret

.Lpost_el2_drop:
    // We're now at EL1h with DAIF masked. x0 still holds the DTB pointer.
    // Mark the fact that we dropped from EL2 — boot_main() prints this
    // in the banner once BSS is cleared and globals are usable.
    mov     x20, #1
    b       .Lel1_main

// ---------------------------------------------------------------------------
// EL1 main path. Entered either directly (x20=0) or after EL2 drop (x20=1).
// x0 holds the DTB physical address.
// ---------------------------------------------------------------------------
.Lel1_main:
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

    // 6. Save EL2-entry flag (0 or 1).
    adrp    x0, _entered_at_el2
    add     x0, x0, :lo12:_entered_at_el2
    str     x20, [x0]

    // 7. KASLR. Choose a randomized slide from DTB seed (or hardware
    //    counter fallback), apply any R_AARCH64_RELATIVE relocations
    //    in the embedded .rela.dyn section. Returns the slide in x0.
    bl      kaslr_init
    mov     x21, x0                     // x21 = slide (callee-saved)

    // 8. Enable the MMU. mmu_enable(slide) builds TTBR0 (low 4 GiB
    //    identity) and TTBR1 (kernel high-half at KASLR_LINK_VA + slide),
    //    programs MAIR / TCR / TTBR / SCTLR. Per-section permissions
    //    enforce W^X (I-12) under both translation roots. After return,
    //    PC is still load PA via TTBR0; we long-branch to high VA next.
    mov     x0, x21
    bl      mmu_enable

    // 9. Long-branch to high VA. Compute the runtime high VA of
    //    boot_main via kaslr_high_va_addr(pa); the PA we pass is the
    //    PC-relative-resolved address of boot_main (current PC = PA,
    //    PIC mode → adrp+add gives PA).
    adrp    x0, boot_main
    add     x0, x0, :lo12:boot_main
    bl      kaslr_high_va_addr          // x0 = high VA of boot_main
    br      x0                          // branch to high VA

    // 10. boot_main() must not return. If it does, fall through to halt.
    b       _hang

.size _start, . - _start
.size _real_start, . - _real_start

// ---------------------------------------------------------------------------
// _hang — clean halt loop. WFI until an external event.
//
// Used by:
//   - Post-boot_main() fallthrough (boot_main shouldn't return).
//   - Wrong-EL entry (EL3 / EL0).
//   - extinction() in kernel/extinction.c.
// ---------------------------------------------------------------------------
.section ".text", "ax"
.globl _hang
.type _hang, %function
_hang:
1:  wfi
    b       1b
.size _hang, . - _hang

// ---------------------------------------------------------------------------
// _saved_dtb_ptr — DTB physical address handed to us by the bootloader.
// BSS-allocated (zeroed at boot), populated by _real_start AFTER the BSS
// clear. Read by boot_main() to print in the boot banner; consumed by the
// DTB parser (lib/dtb.c).
//
// _entered_at_el2 — 1 if the kernel was entered at EL2 and dropped to EL1,
// 0 if entered directly at EL1. Read by boot_main() for the banner's
// el-entry diagnostic. Useful when porting to bare metal (Pi 5) where
// firmware boots us at EL2.
// ---------------------------------------------------------------------------
.section ".bss", "aw", %nobits
.globl _saved_dtb_ptr
.type _saved_dtb_ptr, %object
.align 3
_saved_dtb_ptr:
    .skip 8
.size _saved_dtb_ptr, 8

.globl _entered_at_el2
.type _entered_at_el2, %object
.align 3
_entered_at_el2:
    .skip 8
.size _entered_at_el2, 8
