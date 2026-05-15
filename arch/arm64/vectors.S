// ARM64 exception vector table.
//
// 16 entries × 0x80 bytes each = 0x800 (2 KiB). Base must be 0x800-
// aligned (VBAR_EL1 holds bits [63:11]; bits [10:0] read as zero).
//
// Vector layout (per ARM ARM D1.10):
//
//   Offset  EL source              Exception type
//   ------  ---------------------  ---------------
//   0x000   Current EL with SP_EL0  Synchronous   (unexpected — see below)
//   0x080   Current EL with SP_EL0  IRQ            (unexpected — see below)
//   0x100   Current EL with SP_EL0  FIQ            (unexpected)
//   0x180   Current EL with SP_EL0  SError         (unexpected)
//   0x200   Current EL with SP_ELx  Synchronous   ← LIVE: kernel sync faults
//   0x280   Current EL with SP_ELx  IRQ            ← LIVE: kernel IRQs
//   0x300   Current EL with SP_ELx  FIQ            (unexpected)
//   0x380   Current EL with SP_ELx  SError         (unexpected)
//   0x400   Lower EL using AArch64  Synchronous   ← LIVE: EL0 sync / SVC
//   0x480   Lower EL using AArch64  IRQ            ← LIVE: EL0 IRQs
//   0x500   Lower EL using AArch64  FIQ            (unexpected)
//   0x580   Lower EL using AArch64  SError         (unexpected)
//   0x600   Lower EL using AArch32  Synchronous   (we don't run AArch32)
//   0x680   Lower EL using AArch32  IRQ            (we don't run AArch32)
//   0x700   Lower EL using AArch32  FIQ            (we don't run AArch32)
//   0x780   Lower EL using AArch32  SError         (we don't run AArch32)
//
// SPSel discipline (P5-el1h-kernel; ARCHITECTURE.md §12.1, invariant
// I-21). The kernel runs UNIFORMLY at EL1h — PSTATE.SPSel=1 at all
// times, sp = SP_EL1 = the running thread's own kernel stack. There is
// no EL1t kernel mode and no separate per-CPU exception stack.
//
// Exception entry from the kernel (EL1) keeps SPSel=1 and dispatches
// to the SP_ELx group (0x200 sync / 0x280 IRQ); KERNEL_ENTRY's
// `sub sp, ...` builds the register frame on the same per-thread
// kernel stack the interrupted code was using. Exception entry from
// EL0 hardware-switches to SP_EL1 (= the thread's kernel stack) and
// dispatches to the lower-EL group (0x400 / 0x480). The "Current EL
// with SP_EL0" group (0x000 / 0x080) is UNREACHABLE: the kernel never
// runs at EL1t, so an exception arriving there means SPSel was somehow
// cleared — a soundness violation, routed to the unexpected-vector
// diagnostic.
//
// Because exception frames live on the per-thread kernel stack, they
// travel with the thread across a context switch or a cross-CPU
// migration — there is no per-CPU exception-stack locality to break.
// This is what makes SMP work-stealing of a mid-exception thread
// sound; the former P2-Cc EL1t model could not (a thread's exception
// frame was on a per-CPU stack it could not carry).
//
// The IRQ slots call exception_irq_curr_el then (P2-Bc) preempt_check_irq,
// then resume the interrupted code via the shared .Lexception_return
// trampoline (KERNEL_EXIT lives there to keep each slot ≤ 0x80 bytes;
// `.L` prefix keeps it out of the symbol table). preempt_check_irq is a
// C function that consults g_need_resched (set by sched_tick when the
// current thread's slice expired) and may call sched() to context-
// switch — sched()'s save captures the C-stack state of
// preempt_check_irq's invocation, so when the formerly-current thread
// is eventually resumed, control flows back through the
// preempt_check_irq frame, into vectors.S, and through KERNEL_EXIT to
// eret the IRQed instruction. KERNEL_EXIT runs at EL1h (the only kernel
// mode), so its `msr sp_el0` always writes the non-current bank — the
// architecturally well-defined form.
//
// Trip hazard (deferred): a kernel-stack overflow into the guard page
// now faults recursively (KERNEL_ENTRY builds the frame on the same
// overflowing stack) rather than landing on a separate stack. A
// dedicated overflow / SError stack — entered via an SP_EL0-based
// check in the 0x200 vector — is a future hardening item; g_exception_
// stacks is retained as the reserved buffer for it.

#include "exception.h"

.section ".text", "ax"

// ---------------------------------------------------------------------------
// KERNEL_ENTRY — save GP regs + special regs onto the current stack.
//
// Layout matches struct exception_context:
//   sp+0x000:  x0, x1
//   sp+0x010:  x2, x3
//   ...
//   sp+0x0E0:  x28, x29
//   sp+0x0F0:  x30, sp_el0
//   sp+0x100:  elr_el1, spsr_el1
//   sp+0x110:  esr_el1, far_el1
// Total: 0x120 = 288 bytes (matches EXCEPTION_CTX_SIZE).
// ---------------------------------------------------------------------------

.macro KERNEL_ENTRY
    sub     sp, sp, #EXCEPTION_CTX_SIZE
    stp     x0,  x1,  [sp, #0x000]
    stp     x2,  x3,  [sp, #0x010]
    stp     x4,  x5,  [sp, #0x020]
    stp     x6,  x7,  [sp, #0x030]
    stp     x8,  x9,  [sp, #0x040]
    stp     x10, x11, [sp, #0x050]
    stp     x12, x13, [sp, #0x060]
    stp     x14, x15, [sp, #0x070]
    stp     x16, x17, [sp, #0x080]
    stp     x18, x19, [sp, #0x090]
    stp     x20, x21, [sp, #0x0A0]
    stp     x22, x23, [sp, #0x0B0]
    stp     x24, x25, [sp, #0x0C0]
    stp     x26, x27, [sp, #0x0D0]
    stp     x28, x29, [sp, #0x0E0]
    mrs     x10, sp_el0
    stp     x30, x10, [sp, #0x0F0]
    mrs     x11, elr_el1
    mrs     x12, spsr_el1
    stp     x11, x12, [sp, #0x100]
    mrs     x13, esr_el1
    mrs     x14, far_el1
    stp     x13, x14, [sp, #0x110]
.endm

// KERNEL_EXIT — mirror of KERNEL_ENTRY for recoverable exceptions.
// Currently unused at P1-F (every exception terminates in extinction)
// but defined so Phase 2's recoverable page-fault handler has the
// matching restore path.
.macro KERNEL_EXIT
    ldp     x11, x12, [sp, #0x100]
    msr     elr_el1, x11
    msr     spsr_el1, x12
    ldp     x30, x10, [sp, #0x0F0]
    msr     sp_el0, x10
    ldp     x28, x29, [sp, #0x0E0]
    ldp     x26, x27, [sp, #0x0D0]
    ldp     x24, x25, [sp, #0x0C0]
    ldp     x22, x23, [sp, #0x0B0]
    ldp     x20, x21, [sp, #0x0A0]
    ldp     x18, x19, [sp, #0x090]
    ldp     x16, x17, [sp, #0x080]
    ldp     x14, x15, [sp, #0x070]
    ldp     x12, x13, [sp, #0x060]
    ldp     x10, x11, [sp, #0x050]
    ldp     x8,  x9,  [sp, #0x040]
    ldp     x6,  x7,  [sp, #0x030]
    ldp     x4,  x5,  [sp, #0x020]
    ldp     x2,  x3,  [sp, #0x010]
    ldp     x0,  x1,  [sp, #0x000]
    add     sp, sp, #EXCEPTION_CTX_SIZE
    eret
.endm

// VEC_UNEXPECTED — boilerplate for entries that aren't expected to
// fire at P1-F. Calls exception_unexpected(ctx, idx); never returns.
.macro VEC_UNEXPECTED idx
    KERNEL_ENTRY
    mov     x0, sp
    mov     x1, #\idx
    bl      exception_unexpected
    b       _torpor                   // unreached; silences the assembler
.endm

// ---------------------------------------------------------------------------
// The vector table itself. Each entry must fit in 0x80 bytes (32
// instructions). KERNEL_ENTRY is ~24 instructions; +4 for dispatch +
// safety branch fits in 32.
// ---------------------------------------------------------------------------

.align 11    /* 0x800 alignment per ARM ARM */
.globl _exception_vectors
.type _exception_vectors, %function
_exception_vectors:

    // ------------------------------------------------------------------
    // Current EL with SP_EL0 — UNREACHABLE under the uniform-EL1h kernel
    // model (P5-el1h-kernel; ARCHITECTURE.md §12.1, invariant I-21).
    //
    // The kernel runs uniformly at EL1h (PSTATE.SPSel=1, sp = SP_EL1 =
    // the running thread's kernel stack). An exception taken at the
    // current EL while SPSel=0 therefore means PSTATE.SPSel was somehow
    // cleared — a soundness violation, not a normal control path. Both
    // slots route to the unexpected-vector diagnostic (KERNEL_ENTRY runs
    // on SP_EL1, which hardware selects on entry regardless), which
    // extincts with the vector index so the failure is loud + greppable.
    //
    // The former P2-Cc design ran normal kernel code at EL1t and made
    // these the live kernel-exception entries; P5-el1h-kernel moved the
    // kernel to uniform EL1h, so kernel exceptions now always enter via
    // the SP_ELx group (0x200 / 0x280) below.
    // ------------------------------------------------------------------
    .org _exception_vectors + 0x000      /* Sync EL1t — must not happen */
    VEC_UNEXPECTED 0

    .org _exception_vectors + 0x080      /* IRQ EL1t — must not happen */
    VEC_UNEXPECTED 1

    .org _exception_vectors + 0x100      /* FIQ */
    VEC_UNEXPECTED 2

    .org _exception_vectors + 0x180      /* SError */
    VEC_UNEXPECTED 3

    // ------------------------------------------------------------------
    // Current EL with SP_ELx — the kernel-exception group. Under the
    // uniform-EL1h kernel model (P5-el1h-kernel) the kernel always runs
    // at EL1h, so EVERY exception taken while in the kernel dispatches
    // here: 0x200 for a synchronous fault (page fault, alignment, BTI,
    // BRK, kernel-mode uaccess fault), 0x280 for an IRQ. KERNEL_ENTRY
    // builds the register frame on the interrupted thread's own kernel
    // stack (SP_EL1). KERNEL_EXIT / .Lexception_return restores it and
    // erets — staying at EL1h for a kernel→kernel return.
    // ------------------------------------------------------------------
    .org _exception_vectors + 0x200      /* Sync */
    KERNEL_ENTRY
    mov     x0, sp
    bl      exception_sync_curr_el
    b       .Lexception_return           /* R12-uaccess: see 0x000 slot. */

    .org _exception_vectors + 0x280      /* IRQ — recoverable */
    KERNEL_ENTRY
    mov     x0, sp
    bl      exception_irq_curr_el
    bl      preempt_check_irq            /* P2-Bc: maybe context-switch */
    b       .Lexception_return           /* shared KERNEL_EXIT trampoline (local) */

    .org _exception_vectors + 0x300      /* FIQ */
    VEC_UNEXPECTED 6

    .org _exception_vectors + 0x380      /* SError */
    VEC_UNEXPECTED 7

    // ------------------------------------------------------------------
    // Lower EL using AArch64 — userspace.
    //
    // P3-Ea: sync (0x400) routes EL0 page faults through
    // exception_sync_lower_el → arch_fault_handle → userland_demand_page
    // (P3-Dc demand paging). On FAULT_HANDLED the trampoline ERETs to
    // resume the faulting EL0 instruction. SVC stubs extinct at v1.0;
    // P3-Ec adds the syscall dispatcher.
    //
    // IRQ (0x480) reuses exception_irq_curr_el — same GIC ack→dispatch→
    // EOI flow regardless of EL source. KERNEL_ENTRY captures SP_EL0
    // (the user's stack at fault) for the saved context. preempt_check_irq
    // observes the slice/need_resched and may context-switch.
    //
    // FIQ + SError still extinct (no FIQ source online; SError indicates
    // hardware-level corruption that v1.0 can't recover from cleanly).
    // ------------------------------------------------------------------
    .org _exception_vectors + 0x400      /* Sync from EL0 */
    KERNEL_ENTRY
    mov     x0, sp
    bl      exception_sync_lower_el
    b       .Lexception_return           /* ERET to EL0 on FAULT_HANDLED */

    .org _exception_vectors + 0x480      /* IRQ from EL0 */
    KERNEL_ENTRY
    mov     x0, sp
    bl      exception_irq_curr_el
    bl      preempt_check_irq            /* P2-Bc: maybe context-switch */
    b       .Lexception_return

    .org _exception_vectors + 0x500
    VEC_UNEXPECTED 10

    .org _exception_vectors + 0x580
    VEC_UNEXPECTED 11

    // ------------------------------------------------------------------
    // Lower EL using AArch32 — we don't run AArch32 code anywhere.
    // ------------------------------------------------------------------
    .org _exception_vectors + 0x600
    VEC_UNEXPECTED 12

    .org _exception_vectors + 0x680
    VEC_UNEXPECTED 13

    .org _exception_vectors + 0x700
    VEC_UNEXPECTED 14

    .org _exception_vectors + 0x780
    VEC_UNEXPECTED 15

    .org _exception_vectors + 0x800
.size _exception_vectors, . - _exception_vectors

// ---------------------------------------------------------------------------
// .Lexception_return — shared trampoline for recoverable exception paths.
//
// Each vector slot is capped at 0x80 bytes (32 instructions). KERNEL_ENTRY
// is ~25 instructions; KERNEL_EXIT is ~23. The combined sequence
// (entry + handler call + exit) is 50+ instructions, well over the budget.
// We factor KERNEL_EXIT into this trampoline so any recoverable slot fits
// as: KERNEL_ENTRY + mov x0,sp + bl handler + b .Lexception_return = 28
// instructions.
//
// **Local label** (`.L` prefix; not exposed in the symbol table). This
// prevents accidental control transfer from outside the vector path —
// branching to `_exception_return` as a global would let any kernel
// caller invoke KERNEL_EXIT on whatever happens to sit on the current
// SP, ERETing to attacker-controllable ELR_EL1 + SPSR_EL1. Phase 2's
// recoverable sync-fault paths (page-fault COW, etc.) will branch here
// from within vectors.S the same way the IRQ slot does — no need for
// a global symbol.
//
// The trampoline doesn't peek at SP_EL0 / x0 — it restores everything
// KERNEL_ENTRY saved, then ERETs to ELR_EL1 with SPSR_EL1.
// ---------------------------------------------------------------------------
.section ".text", "ax"
.Lexception_return:
    KERNEL_EXIT
