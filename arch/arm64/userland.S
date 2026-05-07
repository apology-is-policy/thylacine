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

.section ".text", "ax"

.globl userland_enter
.type userland_enter, %function
userland_enter:
    bti     c

    // SUBTLE: P2-Cc kernel runs at EL1 with SPSel=0, so the live SP
    // register IS SP_EL0. `msr sp_el0, x` on QEMU virt with SPSel=0
    // raises a synchronous EC=0 (Unknown reason) — empirically.
    // `mov sp, x17` (encoded as `add sp, x17, #0`) works correctly.
    //
    // Park entry + user_sp in x16/x17 (caller-saved scratch; this
    // routine is __noreturn), zero every other GPR, then write SP
    // LAST — after that point we execute only register-to-register
    // ops (no stack touches) until eret.
    mov     x16, x0
    mov     x17, x1

    msr     elr_el1,  x16
    msr     spsr_el1, xzr        // PSTATE: EL0t, all DAIF clear

    // Zero every GPR except x16/x17 (still parked).
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

    // LAST step before eret: set SP. Live SP changes from kernel
    // kstack to user_sp here. Subsequent instructions (mov + eret)
    // do NOT touch the stack — pure register ops.
    mov     sp, x17

    // Zero the parked scratch.
    mov     x17, xzr
    mov     x16, xzr

    eret

    // Defensive infinite loop if eret somehow falls through.
1:  wfe
    b       1b
.size userland_enter, . - userland_enter
