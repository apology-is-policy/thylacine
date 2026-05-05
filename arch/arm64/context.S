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
//   sp  @ 96  tpidr_el0 @ 104       (size 112)
//
// _Static_asserts in context.h pin both sides; a layout drift triggers a
// build error there before the asm can corrupt context switches.

.section ".text", "ax"

.globl cpu_switch_context
.type cpu_switch_context, %function
cpu_switch_context:
    // Reached via bl from C — BTYPE = 10. bti c is the matching landing.
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

    ret
.size cpu_switch_context, . - cpu_switch_context


// thread_trampoline — initial ctx.lr for fresh threads created by
// thread_create. Reached via cpu_switch_context's ret (BTYPE = 00).
// The first instruction is `bti c` for defense in depth (passes under
// BTYPE=00; also passes if some future caller reaches this via blr).
//
// x21 carries the entry function pointer, parked there by thread_create
// via ctx.x21 and loaded by cpu_switch_context's `ldp x21,x22`. Calling
// via blr matches the entry function's `bti c` prologue (BTYPE=10).
//
// If entry returns, halt on WFE — Phase 2 close adds thread_exit + reap.
.globl thread_trampoline
.type thread_trampoline, %function
thread_trampoline:
    bti     c

    blr     x21

    // Entry returned (no thread_exit at P2-A). Halt.
1:  wfe
    b       1b
.size thread_trampoline, . - thread_trampoline
