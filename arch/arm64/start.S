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
    b.ne    _torpor                       // EL3 or EL0 — silent halt
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
    // 1.5. Capture boot-start counter for the boot-time banner (P1-I).
    //     CNTPCT_EL0 ticks at the architectural counter frequency
    //     (CNTFRQ_EL0; ~62.5 MHz on QEMU virt, varies elsewhere).
    //     Reading it here gives the earliest-recorded reference point;
    //     boot_main subtracts the post-banner read to compute boot time.
    //     CNTHCTL_EL2.EL1PCTEN is 1 by this point: either set by EL2
    //     drop or by firmware on direct-EL1 entry. No DTB needed.
    //     We can't write to BSS yet (BSS clear hasn't run); stash in
    //     a callee-saved reg and write after BSS clear, alongside DTB.
    mrs     x22, cntpct_el0

    // 2. Stash DTB pointer in x19 (callee-saved; survives the BSS clear).
    mov     x19, x0

    // 3. Set up the kernel stack. P5-el1h-kernel: the kernel runs
    //    uniformly at EL1h (SPSel=1) — assert SPSel=1 BEFORE the `mov sp`
    //    so the write lands SP_EL1 (the kernel stack) regardless of the
    //    firmware/EL2-drop entry SPSel. `msr SPSel,#1` is a no-op if
    //    already 1; isb drains it so the `mov sp` observes the new bank.
    //    SP grows down from _boot_stack_top.
    msr     SPSel, #1
    isb
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

    // 4.5. Initialize OS-reserved per-CPU registers to known-zero.
    //
    // Per ARM ARM, TPIDR_EL0 and TPIDR_EL1 reset values are
    // architecturally UNKNOWN. P2-A's `current_thread()` accessor
    // (kernel/include/thylacine/thread.h) reads TPIDR_EL1 — a pre-
    // thread_init read used to depend on "BSS-default zero" (which
    // doesn't apply to system registers). The msr to xzr below makes
    // the pre-thread_init read deterministic-NULL.
    //
    // TPIDR_EL0 is the EL0 TLS pointer, saved/restored across context
    // switches by cpu_switch_context. Without this init, kthread
    // captures whatever firmware leftover lives there on its first
    // switch-out — a leak hazard once userspace lands at Phase 5.
    //
    // Both writes are 1 instruction each; cost is negligible. Closes
    // P2-A audit R4 F44 + F45.
    msr     tpidr_el0, xzr
    msr     tpidr_el1, xzr

    // 4.6. P5-el1h-kernel: assert the uniform-EL1h kernel mode.
    //
    // The kernel runs uniformly at EL1h — PSTATE.SPSel = 1 at all times,
    // the active stack pointer is always SP_EL1, and SP_EL1 is the
    // running thread's own kernel stack (the boot stack here; a per-Proc
    // kstack once threads exist). Exception entry keeps SPSel=1 and
    // builds its register frame on that same stack. There is NO separate
    // EL1t kernel mode and NO separate per-CPU exception stack.
    //
    // Rationale (ARCHITECTURE.md §12.1, invariant I-21): the earlier
    // P2-Cc design ran normal kernel code at EL1t (SPSel=0, sp = SP_EL0)
    // and entered EL1h only transiently in handlers, with SP_EL1 a
    // per-CPU exception stack. That made a thread's execution mode a
    // function of scheduling history — cpu_switch_context carries SP but
    // not SPSel — and under SMP work-stealing a thread could resume the
    // KERNEL_EXIT path in the wrong mode, where `msr SP_EL0` against the
    // selected SP is CONSTRAINED UNPREDICTABLE (EC=0 trap, dead CPU). A
    // per-CPU exception stack is also incompatible with migrating a
    // mid-exception thread. The uniform-EL1h model removes the mode.
    //
    // step 3 already asserted SPSel=1 and set sp = SP_EL1 = the kernel
    // stack. Here we only zero SP_EL0 — it is the userspace stack and
    // must not carry a kernel-VA leftover into the first eret-to-EL0.
    // At SPSel=1, SP_EL0 is the non-current bank, so `msr sp_el0` is
    // the architecturally well-defined write.
    msr     sp_el0, xzr

    // 5. Save DTB pointer. BSS is now cleared; this write sticks.
    adrp    x0, _saved_dtb_ptr
    add     x0, x0, :lo12:_saved_dtb_ptr
    str     x19, [x0]

    // 6. Save EL2-entry flag (0 or 1).
    adrp    x0, _entered_at_el2
    add     x0, x0, :lo12:_entered_at_el2
    str     x20, [x0]

    // 6.25. Save boot-start CNTPCT for the boot-time banner.
    adrp    x0, _boot_start_cntpct
    add     x0, x0, :lo12:_boot_start_cntpct
    str     x22, [x0]

    // 6.5. P1-H: PAC + BTI runtime enable.
    //
    // PAC: We program the APIA key from cntpct_el0. The compiler's
    //  -mbranch-protection=pac-ret emits paciasp / autiasp around
    //  every non-leaf function; on ARMv8.0 hardware these are NOPs in
    //  the HINT space (harmless), on ARMv8.3+ they sign / authenticate
    //  the link register against APIAKey. Both halves of the key are
    //  programmed before the first paciasp runs (which happens when
    //  the next BL transfers control into a C function emitting the
    //  prologue) so the auth and sign are consistent. We also set
    //  APIB / APDA / APDB keys for completeness — pac-ret only uses
    //  APIA, but if Phase 2 turns on data-pointer auth or PAC-B
    //  variants, the keys are pre-seeded.
    //
    // BTI: SCTLR_EL1.BT0 (bit 35) enables Branch Target Identification
    //  enforcement at EL1 on ARMv8.5+. Pages with PTE.GP=1 (set by
    //  mmu.c via PTE_KERN_TEXT) require indirect-branch targets to
    //  match a `bti j/c/jc` marker matching PSTATE.BTYPE. The compiler
    //  emits `bti c` at function-call landing pads under
    //  -mbranch-protection=bti.
    //
    // Both EnIA (bit 31) and BT0 (bit 35) are RES0 on hardware that
    // doesn't implement FEAT_PAuth / FEAT_BTI — writing 1 is a NOP
    // there. So this sequence is zero-cost on ARMv8.0 and unlocks the
    // hardening on ARMv8.3+ / 8.5+.
    // P2-Cb: refactored — keys derived once on primary, stored in
    // g_pac_keys[8] (BSS), then applied by pac_apply_this_cpu (also
    // called by secondary CPU bring-up so all CPUs use IDENTICAL
    // PAC keys). Cross-CPU PAC consistency is required for thread
    // migration: a thread's signed return address on its kstack
    // must auth-validate against APIA on whichever CPU resumes it.
    bl      pac_derive_keys
    bl      pac_apply_this_cpu

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

    // 8.5. P5-el1h-kernel: re-anchor the kernel stack (SP_EL1) at the
    //      post-KASLR HIGH VA of _boot_stack_top.
    //
    // step 3 set SP_EL1 to a LOW PA (the pre-MMU PC-relative resolution
    // of _boot_stack_top). After mmu_enable the kernel runs at high VA
    // via TTBR1; SP_EL1 must point at the high VA so exception frames
    // and stack walks match the kernel's view — and so a stack access
    // survives P3-Bda's mmu_retire_ttbr0_identity, after which the
    // load-PA mapping is gone and any load-PA access faults.
    //
    // The boot stack at load PA and at high VA are the SAME physical
    // memory (l3_kernel covers the kernel image's 2 MiB block under
    // both TTBR0 identity and TTBR1 high-VA — same L3 table), so moving
    // sp loses no data. _real_start has no live stack frame here (all
    // bl calls have returned + popped their saved lr), so resetting sp
    // to the stack top is safe. The kernel runs at SPSel=1 (§4.6), so
    // `mov sp` writes SP_EL1 directly — no SPSel dance, no separate
    // per-CPU exception stack. This single re-anchor replaces the
    // former P2-Cc SP_EL1-exception-stack + P3-Bda SP_EL0-boot-stack
    // pair.
    adrp    x0, _boot_stack_top
    add     x0, x0, :lo12:_boot_stack_top
    bl      kaslr_high_va_addr              // x0 = HIGH VA of _boot_stack_top
    mov     sp, x0                          // SP_EL1 := HIGH VA boot-stack top
    isb

    // 9. Long-branch to high VA. Compute the runtime high VA of
    //    boot_main via kaslr_high_va_addr(pa); the PA we pass is the
    //    PC-relative-resolved address of boot_main (current PC = PA,
    //    PIC mode → adrp+add gives PA).
    //
    // Per ARM ARM D24.2.1: BLR sets PSTATE.BTYPE = 0b10 (call), BR
    // sets BTYPE = 0b01 (jump). bti c matches BTYPE=10 (call); bti j
    // matches BTYPE=01. The compiler emits `bti c` at boot_main's
    // prologue under -mbranch-protection=pac-ret+bti. Use BLR so the
    // resulting BTYPE=10 matches the bti c. With BR, BTYPE=01 would
    // not match bti c and BTI on FEAT_BTI hardware would fault. The
    // discarded LR is harmless because boot_main is `noreturn` and
    // never reads it. (P2-A audit R4 F41 fixed the BTYPE-value
    // mislabel that was here previously.)
    adrp    x0, boot_main
    add     x0, x0, :lo12:boot_main
    bl      kaslr_high_va_addr          // x0 = high VA of boot_main
    blr     x0                          // call high-VA boot_main

    // 10. boot_main() must not return. If it does, fall through to halt.
    b       _torpor

.size _start, . - _start
.size _real_start, . - _real_start

// ---------------------------------------------------------------------------
// _torpor — clean halt loop. WFI until an external event.
//
// Used by:
//   - Post-boot_main() fallthrough (boot_main shouldn't return).
//   - Wrong-EL entry (EL3 / EL0).
//   - extinction() in kernel/extinction.c.
// ---------------------------------------------------------------------------
.section ".text", "ax"
.globl _torpor
.type _torpor, %function
_torpor:
    // BTI landing pad: accept call (BTYPE=01), jump (BTYPE=10), or
    // jump-call (BTYPE=11). All current reaches are direct branches
    // (b _torpor / bl _torpor) which don't trigger BTI checks, but
    // _torpor lives in a GP=1 page (PTE_KERN_TEXT) and SCTLR_EL1.BT0=1,
    // so a future indirect dispatch (function-pointer table, panic
    // handler array) into _torpor would fault on the bare wfi. `bti jc`
    // is a HINT-space NOP on ARMv8.0..8.4 — zero cost, prevents the
    // class of bug at the cost of one instruction (P1-H audit F16).
    bti     jc
1:  wfi
    b       1b
.size _torpor, . - _torpor

// ---------------------------------------------------------------------------
// PAC key management (P2-Cb refactor).
//
// At v1.0 P2-Cb the PAC keys are derived ONCE by the primary CPU from
// CNTPCT_EL0 + a ROR chain, stored in `g_pac_keys[8]` (BSS), and then
// applied to AP*KEY*_EL1 on every CPU (primary first, then each
// secondary as it comes up via PSCI). Cross-CPU PAC key consistency
// is REQUIRED for thread migration (P2-Ce work-stealing): a thread's
// signed return address on its kstack must auth-validate against
// APIA on whichever CPU resumes it. Per-CPU random keys would break
// migration.
//
// Both functions are LEAF (no bl, no callee-saved register usage,
// no stack frame). They are safe to call before/without paciasp/
// autiasp emitted by the C compiler — pac_apply_this_cpu in
// particular is what initializes the keys, so no caller can rely on
// PAC working before its return.
//
// Memory layout — `g_pac_keys` is u64[8]:
//   [0]  apia_hi   [1]  apia_lo
//   [2]  apib_hi   [3]  apib_lo
//   [4]  apda_hi   [5]  apda_lo
//   [6]  apdb_hi   [7]  apdb_lo
//
// (declared in kernel/smp.c so the BSS allocation is in one place
// with the other SMP globals.)
// ---------------------------------------------------------------------------

.section ".text", "ax"

// Lazarus W1: the kernel compiles to the ARMv8.0-A floor (-march=armv8-a),
// under which the assembler would reject the FEAT_PAuth key-register MSRs
// below. This directive re-permits them for ENCODING only; their EXECUTION
// is runtime-gated on FEAT_PAuth in pac_apply_this_cpu (UNDEF on a v8.0 core
// like the A72, so a guard is mandatory, not cosmetic).
.arch_extension pauth

.globl pac_derive_keys
.type pac_derive_keys, %function
pac_derive_keys:
    bti     c                       // safe landing for blr/bl

    // Derive 8 key halves from CNTPCT_EL0 via ROR chain.
    mrs     x0, cntpct_el0
    ror     x1, x0, #13
    ror     x2, x1, #17
    ror     x3, x2, #19
    ror     x4, x3, #23

    // Store to g_pac_keys[8] via PIC.
    adrp    x9, g_pac_keys
    add     x9, x9, :lo12:g_pac_keys
    stp     x0, x1, [x9, #0]        // apia hi/lo
    stp     x1, x2, [x9, #16]       // apib hi/lo
    stp     x2, x3, [x9, #32]       // apda hi/lo
    stp     x3, x4, [x9, #48]       // apdb hi/lo

    ret
.size pac_derive_keys, . - pac_derive_keys

.globl pac_apply_this_cpu
.type pac_apply_this_cpu, %function
pac_apply_this_cpu:
    bti     c                       // safe landing

    // Lazarus W1: PAC + BTI are runtime-conditional. Gate the PAuth work on
    // the running CPU's ID registers. The AP*KEY*_EL1 MSRs below are UNDEF on
    // a core without FEAT_PAuth (the A72 / v8.0 floor) -- a write there traps,
    // so this guard is load-bearing, not cosmetic. The SCTLR PAC-enable bits
    // are RES0 there (harmless), but we gate them too so the enabled set
    // always matches the silicon. Leaf function (no bl / no paciasp): the
    // SCTLR write here is what TURNS PAC ON, so its own prologue must not sign
    // a return address with a key that is not yet loaded.
    mrs     x10, id_aa64isar1_el1
    ubfx    x11, x10, #4, #8        // [11:4] = API:APA -> FEAT_PAuth presence
    cbz     x11, .Lpac_no_pauth

    adrp    x9, g_pac_keys
    add     x9, x9, :lo12:g_pac_keys

    ldp     x0, x1, [x9, #0]
    msr     apiakeyhi_el1, x0
    msr     apiakeylo_el1, x1

    ldp     x2, x3, [x9, #16]
    msr     apibkeyhi_el1, x2
    msr     apibkeylo_el1, x3

    ldp     x4, x5, [x9, #32]
    msr     apdakeyhi_el1, x4
    msr     apdakeylo_el1, x5

    ldp     x6, x7, [x9, #48]
    msr     apdbkeyhi_el1, x6
    msr     apdbkeylo_el1, x7

    // SCTLR_EL1 PAC-enable bits: EnIA (31), EnIB (30), EnDA (27), EnDB (13).
    //   bits = 0x0000_0000_c800_2000  (BT0 is set separately, below)
    mrs     x0, sctlr_el1
    movz    x1, #0x2000, lsl #0
    movk    x1, #0xc800, lsl #16
    orr     x0, x0, x1
    msr     sctlr_el1, x0
    isb

.Lpac_no_pauth:
    // SCTLR_EL1.BT0 (bit 35): gate on FEAT_BTI (ID_AA64PFR1_EL1.BT [3:0]).
    // RES0-safe without BTI, but gated for symmetry with the capability.
    mrs     x12, id_aa64pfr1_el1
    ubfx    x13, x12, #0, #4
    cbz     x13, .Lpac_done
    mrs     x0, sctlr_el1
    movz    x1, #0x0008, lsl #32    // BT0 = bit 35
    orr     x0, x0, x1
    msr     sctlr_el1, x0
    isb

.Lpac_done:
    ret
.size pac_apply_this_cpu, . - pac_apply_this_cpu

// ---------------------------------------------------------------------------
// secondary_entry — PSCI bring-up trampoline (P2-Ca; extended at P2-Cb).
//
// Reached when the primary issues PSCI_CPU_ON for a secondary CPU,
// pointing the entry-point arg at this label's PA. The secondary
// arrives with:
//   - x0 = context_id passed by primary (= secondary CPU index, 1..N-1).
//   - MMU off.
//   - PSTATE.DAIF.* = 1 (all maskable exceptions masked).
//   - SP undefined.
//   - EL1.
//
// P2-Cb extension: the trampoline now does full per-CPU init —
//   1. Validate idx in [1, DTB_MAX_CPUS).
//   2. Set SP_EL1 = this CPU's kernel stack from g_secondary_boot_stacks
//      [idx-1] (20 KiB slot: 4 KiB guard page + 16 KiB usable); keep
//      SPSel=1 (P5-el1h-kernel — the kernel
//      runs uniformly at EL1h, exception frames on the per-thread
//      kernel stack, no separate exception stack).
//   3. Flip g_cpu_online[idx] (early "trampoline reached" signal).
//   4. bl pac_apply_this_cpu — load shared keys + enable PAC.
//   5. bl mmu_program_this_cpu — enable MMU using primary's tables.
//   6. Long-branch via kaslr_high_va_addr to per_cpu_main(idx) at
//      the kernel's high VA. per_cpu_main sets VBAR_EL1, TPIDR_EL1,
//      flips g_cpu_alive[idx] (the "fully initialized" signal), and
//      enters an idle WFI loop.
//
// PSCI bring-up is serialized by smp_init (one secondary at a time),
// so the per-CPU stack assignment is race-free. PAC keys must be
// initialized BEFORE bl mmu_program_this_cpu because the C function
// emits paciasp/autiasp; without keys, autiasp would fail.
//
// PIC discipline: all symbols (g_cpu_online, g_secondary_boot_stacks,
// per_cpu_main, etc.) reachable via adrp + lo12 because PC = PA at
// trampoline entry and all symbols are in the same kernel image
// (the offset between PC and any kernel symbol is link-time-constant).
//
// kaslr_high_va_addr: a leaf C function that takes a PA, returns the
// runtime high VA after the KASLR slide. Safe to call at low PA via
// PIC; primary already populated the slide globals before secondaries
// come up.
// ---------------------------------------------------------------------------
.section ".text", "ax"
.globl secondary_entry
.type secondary_entry, %function
secondary_entry:
    // BTI landing pad. PSCI dispatch sets BTYPE = 00 (no constraint
    // — return) on the implementation, but `bti c` is defensive.
    bti     c

    // x0 = secondary CPU index (passed as PSCI context_id).
    // Validate: 0 < idx < DTB_MAX_CPUS. If out of range, hang silently
    // (no UART access — we have no MMU and no DTB-discovered base).
    mov     x9, #1
    cmp     x0, x9
    b.lt    secondary_bad
    mov     x9, #8                          // DTB_MAX_CPUS at v1.0
    cmp     x0, x9
    b.ge    secondary_bad

    // Save idx in a callee-saved register — survives the bl
    // pac_apply_this_cpu / bl mmu_program_this_cpu calls.
    mov     x19, x0

    // P5-el1h-kernel: set up this secondary's single kernel stack.
    // Mirror primary's _real_start: the kernel runs uniformly at EL1h
    // (SPSel=1), sp = SP_EL1 = this CPU's kernel stack. PSCI hands the
    // secondary off at SPSel=1 with SP undefined; assert SPSel=1 BEFORE
    // the `mov sp` so the write lands SP_EL1 regardless of the entry
    // SPSel (`msr sp_el1` from EL1 is UNDEFINED — ARM ARM B6.2 — so SP_EL1
    // must be written via `mov sp` while SPSel=1). isb drains the SPSel
    // change so the `mov sp` observes the new bank.
    msr     SPSel, #1
    isb

    // Set SP_EL1 to the top of this secondary's kernel stack. Each slot
    // in g_secondary_boot_stacks is SECONDARY_STACK_SLOT_SIZE bytes —
    // a 4 KiB guard page followed by 16 KiB of usable stack. The guard
    // leads the slot, so SP starts at the slot top and grows DOWN
    // through the 16 KiB usable region; an overflow past the usable
    // bottom crosses into the (no-access) guard page and faults.
    //   slot_base = &g_secondary_boot_stacks[idx-1]   (= the guard page)
    //   sp        = slot_base + SECONDARY_STACK_SLOT_SIZE
    // The #20480 literal mirrors SECONDARY_STACK_SLOT_SIZE in
    // thylacine/smp.h (asm cannot include that header); the header's
    // _Static_assert pins the C side. Keep the two in sync.
    sub     x20, x19, #1                    // idx0 = idx - 1
    adrp    x9, g_secondary_boot_stacks
    add     x9, x9, :lo12:g_secondary_boot_stacks
    mov     x10, #20480                     // SECONDARY_STACK_SLOT_SIZE
    madd    x9, x10, x20, x9                // slot_base = stacks + idx0 * 20480
    add     x9, x9, x10                     // sp = slot_base + 20480 (slot top)
    mov     sp, x9                          // sp = SP_EL1 := kernel-stack top

    // Zero SP_EL0 (the userspace stack; must not carry a kernel-VA
    // leftover into the first eret-to-EL0). SP_EL0 is the non-current
    // bank at SPSel=1, so `msr sp_el0` is the well-defined write.
    msr     sp_el0, xzr

    // Flip g_cpu_online[idx] — early "trampoline reached" signal
    // (primary's smp_init also waits for g_cpu_alive set in per_cpu_main).
    adrp    x9, g_cpu_online
    add     x9, x9, :lo12:g_cpu_online
    add     x9, x9, x19
    mov     w10, #1
    strb    w10, [x9]
    dsb     sy

    // Apply PAC keys (primary already derived + stored to g_pac_keys).
    // Leaf asm function — no paciasp emission, safe to call before
    // PAC is enabled on this CPU.
    bl      pac_apply_this_cpu

    // Enable MMU using primary's already-built page tables.
    // mmu_program_this_cpu is a C function; PAC is now enabled so
    // its paciasp/autiasp work correctly.
    bl      mmu_program_this_cpu

    // P5-el1h-kernel: re-anchor this CPU's kernel stack (SP_EL1) at the
    // post-KASLR HIGH VA of its g_secondary_boot_stacks slot top. See
    // primary's _real_start step 8.5 for rationale. Must run AFTER
    // mmu_program_this_cpu (we need TTBR1 to resolve high VAs) and
    // BEFORE the long-branch to per_cpu_main (so any subsequent fault
    // has SP_EL1 in high VA). The kernel runs at SPSel=1, so `mov sp`
    // writes SP_EL1 directly — one re-anchor, no SPSel dance, no
    // separate per-CPU exception stack (the former P2-Cc SP_EL1-exc /
    // P3-Bda SP_EL0-boot pair collapses to this).
    sub     x20, x19, #1                    // idx0 = idx - 1
    adrp    x0, g_secondary_boot_stacks
    add     x0, x0, :lo12:g_secondary_boot_stacks
    mov     x9, #20480                      // SECONDARY_STACK_SLOT_SIZE
    madd    x0, x9, x20, x0                 // slot_base = stacks + idx0 * 20480
    add     x0, x0, x9                      // slot top = slot_base + 20480
    bl      kaslr_high_va_addr              // x0 = HIGH VA of slot top
    mov     sp, x0                          // SP_EL1 := HIGH VA kernel-stack top
    isb

    // Long-branch to per_cpu_main(idx) at the kernel's high VA.
    //   1. Get PA of per_cpu_main via PC-relative adrp+add.
    //   2. Convert to high VA via kaslr_high_va_addr(pa).
    //   3. Move idx to x0 (per_cpu_main first arg).
    //   4. blr to high VA.
    adrp    x0, per_cpu_main
    add     x0, x0, :lo12:per_cpu_main
    bl      kaslr_high_va_addr              // x0 = high VA
    mov     x21, x0                         // x21 = high VA (callee-saved)

    mov     x0, x19                         // x0 = idx (per_cpu_main arg)
    blr     x21                             // call high-VA per_cpu_main(idx)

    // per_cpu_main is noreturn. Defensive halt at low PA (TTBR0
    // identity-mapped, so accessible even with MMU on).
1:  wfi
    b       1b

secondary_bad:
    // Invalid idx — park at WFI without setting any flag.
    dsb     sy
1:  wfi
    b       1b
.size secondary_entry, . - secondary_entry

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

// _boot_start_cntpct — CNTPCT_EL0 captured at the very start of
// _real_start (before BSS clear, before DTB save, before MMU). Read by
// boot_main to compute the elapsed boot time. Counter ticks at
// CNTFRQ_EL0 Hz (architectural; readable post-EL2-drop or directly at
// EL1 if firmware enabled CNTHCTL_EL2.EL1PCTEN).
.globl _boot_start_cntpct
.type _boot_start_cntpct, %object
.align 3
_boot_start_cntpct:
    .skip 8
.size _boot_start_cntpct, 8
