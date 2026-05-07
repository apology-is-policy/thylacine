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

    // SP-write strategy: `mov sp, x17` (encoded `add sp, x17, #0`) updates
    // whichever SP register `sp` aliases under the current PSTATE.SPSel
    // (SP_EL0 at SPSel=0; SP_EL1 at SPSel=1). The fix block below at line
    // 102 explicitly forces SPSel=0 immediately before this write, so
    // `mov sp, x17` always lands SP_EL0 := user_sp regardless of the
    // entry-side SPSel — closing trip-hazard #157 (P4-Fix157).
    //
    // The `mov sp` form is preserved over the alternative `msr sp_el0,
    // x17` for backward compatibility with older QEMU virt versions: the
    // `msr sp_el0, x17` form was empirically observed to raise a
    // synchronous EC=0 ("Unknown reason") on the QEMU virt version we
    // shipped against during P3-Ed bring-up (R7 F135 reclassified that as
    // a QEMU quirk, not an architectural issue — newer QEMU versions
    // accept the `msr` form correctly). Switching to `msr sp_el0, x17`
    // would let us drop the SPSel dance entirely; tracked as a future
    // simplification once the test-infrastructure QEMU baseline is past
    // the affected version.
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

    // [#157 fix] Force SPSel=0 before writing sp.
    //
    // The prior code path assumed `sp` always aliases SP_EL0, but that
    // only holds when PSTATE.SPSel=0. When userland_enter is called from
    // a kthread that was dispatched in SPSel=1 state (e.g., the parent
    // got preempted by a timer IRQ → preempt_check_irq → sched →
    // cpu_switch_context, all running at SPSel=1, and the new thread's
    // trampoline never lowered SPSel back to 0), `mov sp, x17` writes
    // SP_EL1 — not SP_EL0. After eret to EL0 the user stack is unset
    // (SP_EL0 retains stale state) AND every subsequent EL1 exception
    // entry uses the corrupt SP_EL1 (= 0x80000000 user VA), which lives
    // in TTBR0 territory; KERNEL_ENTRY's first stp faults, fault
    // handler re-enters at the same vector, decrements SP_EL1 by
    // EXCEPTION_CTX_SIZE (0x120), faults again, ad infinitum. This is
    // the trip-hazard #157 root cause (deferred from R7).
    //
    // Why iter 1 worked: iter 1's child happened to be dispatched at
    // SPSel=0 (parent's wait_pid sleep path). Iter 2's dispatch went
    // through an IRQ-driven sched (timer preempt of the parent's busy
    // wait), arriving at SPSel=1.
    //
    // The fix: explicitly switch to SPSel=0 before writing sp.
    // ISB serializes the SPSel update so subsequent `mov sp, ...`
    // observes the new SPSel value (and thus aliases SP_EL0).
    msr     SPSel, #0
    isb

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
