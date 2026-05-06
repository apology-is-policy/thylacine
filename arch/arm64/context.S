// arch/arm64/context.S — context-switch primitive (P2-A).
//
// cpu_switch_context(struct Context *prev /* x0 */, struct Context *next /* x1 */)
//
// Saves the AAPCS64 callee-saved registers (x19-x28, x29 FP, x30 LR)
// plus SP and TPIDR_EL0 into `prev`; loads the same fields from `next`
// into the live CPU; rets to the loaded LR.
//
// This routine is a function-call leaf: no nested call, no stack frame
// of its own. x30 at entry is the bl return address (unsigned per
// AAPCS64); we save it directly into prev->lr without paciasp / autiasp.
// The caller's pac-ret discipline pushes its OWN signed lr to its stack
// at its prologue; what we hold in x30 across the call is just the fresh
// bl return address. See context.h commentary for the discipline.
//
// Field offsets MUST match struct Context layout in <thylacine/context.h>:
//   x19 @ 0   x20 @ 8    x21 @ 16   x22 @ 24
//   x23 @ 32  x24 @ 40   x25 @ 48   x26 @ 56
//   x27 @ 64  x28 @ 72   fp  @ 80   lr  @ 88
//   sp  @ 96  tpidr_el0 @ 104       ttbr0 @ 112  (size 120)
//
// _Static_asserts in context.h pin both sides; a layout drift triggers a
// build error there before the asm can corrupt context switches.
//
// P3-Bdb: ttbr0 swap. Each Thread's Context carries the TTBR0_EL1 value
// (ASID<<48 | pgtable_root_PA) appropriate for the Thread's Proc.
// cpu_switch_context saves the live TTBR0_EL1 into prev (atomic with the
// other register saves) and loads next's TTBR0 into the live CPU. An ISB
// after MSR TTBR0_EL1 makes the change visible to subsequent fetches.
// No TLB invalidate is needed: per-Proc ASIDs (1..255) tag user-mappings
// in the TLB; cross-Proc TLB entries don't collide. Kernel mappings
// (TTBR1, global, ASID-agnostic) are unaffected.

.section ".text", "ax"

.globl cpu_switch_context
.type cpu_switch_context, %function
cpu_switch_context:
    // Per ARM ARM D24.2.1: BL is a direct branch and sets PSTATE.BTYPE
    // = 0b00 (no constraint). BLR (which a future indirect-call site
    // might use to reach us) sets BTYPE = 0b10 (call). bti c matches
    // both: it passes under BTYPE=00 (any landing allowed) and under
    // BTYPE=10 (call landing). Direct BL from C → BTYPE=00 is the
    // common case at P2-A.
    bti     c

    // Save callee-saved + FP + LR into prev (x0).
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]

    // SP can't be a stp source — read via mov to a scratch GPR first.
    mov     x9, sp
    str     x9, [x0, #96]

    mrs     x9, tpidr_el0
    str     x9, [x0, #104]

    // P3-Bdb: save current TTBR0_EL1 into prev->ctx.ttbr0.
    mrs     x9, ttbr0_el1
    str     x9, [x0, #112]

    // Load next (x1) into the live CPU.
    ldp     x19, x20, [x1, #0]
    ldp     x21, x22, [x1, #16]
    ldp     x23, x24, [x1, #32]
    ldp     x25, x26, [x1, #48]
    ldp     x27, x28, [x1, #64]
    ldp     x29, x30, [x1, #80]

    ldr     x9, [x1, #96]
    mov     sp, x9

    ldr     x9, [x1, #104]
    msr     tpidr_el0, x9

    // P3-Bdb: load next's TTBR0_EL1. Encoded as (ASID<<48) | pgtable_root.
    // ISB serializes the TTBR0 change so subsequent translations / TLB
    // walks see the new value. No TLB invalidate needed: ASID tags user
    // mappings in the TLB; cross-Proc TLB entries don't collide. Kernel
    // mappings via TTBR1 are global and unaffected.
    ldr     x9, [x1, #112]
    msr     ttbr0_el1, x9
    isb

    ret
.size cpu_switch_context, . - cpu_switch_context


// thread_trampoline — initial ctx.lr for fresh threads created by
// thread_create / thread_create_with_arg. Reached via cpu_switch_context's
// ret. Per ARM ARM D24.2.1, RET sets PSTATE.BTYPE = 0b00 (no constraint)
// — the first instruction's bti c here passes regardless. bti c is
// defensive: it also passes if some future caller reaches the trampoline
// via blr (BTYPE=10).
//
// x21 carries the entry function pointer, parked by thread_create via
// ctx.x21 and loaded by cpu_switch_context's `ldp x21,x22`. x20 carries
// the entry's arg (parked by thread_create_with_arg via ctx.x20; zero
// for thread_create no-arg entries via KP_ZERO). The mov x0, x20 below
// places the arg in x0 per AAPCS64; entries that take no arg ignore
// x0 (it's caller-saved). The blr sets BTYPE=10 (call); the entry
// function's compiler-emitted `bti c` prologue (under
// -mbranch-protection=pac-ret+bti) matches.
//
// IRQ-mask discipline (P2-Bc): the schedule that switched into this
// fresh thread had IRQs masked (sched() entry's irqsave; cpu_switch_
// context preserves PSTATE.DAIF). For a fresh thread with no prior
// sched() frame to call irqrestore, we unmask IRQs explicitly here
// so timer-IRQ-driven preemption can fire while the thread runs its
// entry function. Subsequent sched() calls from the entry restore
// IRQ state via the normal irqsave/irqrestore pattern; no further
// asymmetry. `msr daifclr, #2` clears PSTATE.I (IRQ mask bit) only —
// the other DAIF bits stay at whatever the prior state was.
//
// If entry returns, halt on WFE — P2-D introduces exits() as the
// structured termination path; entry returning without exits() is a
// kernel-thread bug.
.globl thread_trampoline
.type thread_trampoline, %function
thread_trampoline:
    bti     c

    /* P2-Ce: release the run-tree lock prev held when switching to us.
     * Mirrors the post-cpu_switch_context lock-release in sched()'s
     * resume path. Without this, the per-CPU lock stays held forever
     * (until the new thread's own first sched() call, which would
     * then double-acquire and deadlock).
     *
     * sched_finish_task_switch is a leaf C function — calling it
     * before unmask is fine: PAC was applied per-CPU at start.S
     * before the long-branch, and bl preserves x21 (entry fn ptr)
     * AND x20 (entry arg) since both are callee-saved per AAPCS64. */
    bl      sched_finish_task_switch

    msr     daifclr, #2          /* P2-Bc: unmask IRQs for new thread */

    mov     x0, x20              /* P2-D: pass arg to entry. zero for
                                  *      no-arg entries via KP_ZERO. */
    blr     x21

    // Entry returned without calling exits() (P2-D). Halt.
1:  wfe
    b       1b
.size thread_trampoline, . - thread_trampoline
