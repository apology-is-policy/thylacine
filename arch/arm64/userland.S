// arch/arm64/userland.S — kernel→EL0 entry trampoline (P3-Ed).
//
// userland_enter(entry_pc, user_sp) does the eret that transitions the
// calling thread from EL1 to EL0 at `entry_pc` running on `user_sp`.
//
// Used by exec_thunk-style helpers: a kthread runs exec_setup (loading
// the ELF into the Proc's address space + reporting entry + sp), then
// calls userland_enter to perform the eret. The function never returns.
//
// PSTATE setup before eret:
//   ELR_EL1  = entry_pc        (target instruction in EL0)
//   SP_EL0   = user_sp         (initial user stack pointer)
//   SPSR_EL1 = 0               (PSTATE: M=EL0t, all DAIF clear)
//
// Register cleanup before eret: every GPR is zeroed so the kernel
// doesn't leak state to userspace.
//
// P5-el1h-kernel: the kernel runs uniformly at EL1h (PSTATE.SPSel=1;
// ARCHITECTURE.md §12.1, invariant I-21). userland_enter therefore
// executes at EL1h with the live `sp` being SP_EL1 (the calling
// thread's kernel stack). The user stack pointer is installed by
// writing the *non-current* bank directly — `msr sp_el0, user_sp` —
// which at EL1h is the architecturally well-defined form (SP_EL0 is
// not the selected stack pointer, so this is not the CONSTRAINED
// UNPREDICTABLE write-the-current-SP case). The kernel `sp` (SP_EL1)
// is left untouched; the eret to EL0 makes SP_EL0 active.
//
// This replaces the former P4-Fix157 `msr SPSel,#0; mov sp` dance:
// that band-aid existed only because the pre-P5 EL1t/EL1h dual-mode
// kernel could arrive here at either SPSel value, so `mov sp` wrote an
// unpredictable bank. Under the uniform-EL1h model the mode is fixed
// and the dance is unnecessary — `msr sp_el0` is unambiguous.

.section ".text", "ax"

.globl userland_enter
.type userland_enter, %function
userland_enter:
    bti     c

    // Park entry_pc (x0) + user_sp (x1) in caller-saved scratch
    // (x16/x17) so the GPR-zeroing sweep below can clobber x0/x1.
    mov     x16, x0
    mov     x17, x1

    // PSTATE for the eret target: EL0t, all DAIF clear.
    msr     elr_el1,  x16            // ELR_EL1 := entry_pc
    msr     spsr_el1, xzr            // SPSR_EL1 := 0 (EL0t, DAIF clear)

    // Install the user stack pointer. At EL1h, SP_EL0 is the non-current
    // bank — `msr sp_el0` is the well-defined write and does NOT disturb
    // the live kernel `sp` (SP_EL1). After the eret drops to EL0, SP_EL0
    // becomes the active stack pointer.
    msr     sp_el0,   x17            // SP_EL0 := user_sp

    // Zero every GPR — including the x16/x17 scratch, now consumed — so
    // no kernel register state leaks across the EL1→EL0 boundary.
    mov     x0,  xzr
    mov     x1,  xzr
    mov     x2,  xzr
    mov     x3,  xzr
    mov     x4,  xzr
    mov     x5,  xzr
    mov     x6,  xzr
    mov     x7,  xzr
    mov     x8,  xzr
    mov     x9,  xzr
    mov     x10, xzr
    mov     x11, xzr
    mov     x12, xzr
    mov     x13, xzr
    mov     x14, xzr
    mov     x15, xzr
    mov     x16, xzr
    mov     x17, xzr
    mov     x18, xzr
    mov     x19, xzr
    mov     x20, xzr
    mov     x21, xzr
    mov     x22, xzr
    mov     x23, xzr
    mov     x24, xzr
    mov     x25, xzr
    mov     x26, xzr
    mov     x27, xzr
    mov     x28, xzr
    mov     x29, xzr             // FP
    mov     x30, xzr             // LR

    eret

    // Defensive infinite loop if eret somehow falls through.
1:  wfe
    b       1b
.size userland_enter, . - userland_enter
