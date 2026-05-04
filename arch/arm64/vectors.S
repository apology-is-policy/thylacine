// ARM64 exception vector table.
//
// 16 entries × 0x80 bytes each = 0x800 (2 KiB). Base must be 0x800-
// aligned (VBAR_EL1 holds bits [63:11]; bits [10:0] read as zero).
//
// Vector layout (per ARM ARM D1.10):
//
//   Offset  EL source              Exception type
//   ------  ---------------------  ---------------
//   0x000   Current EL with SP_EL0  Synchronous
//   0x080   Current EL with SP_EL0  IRQ
//   0x100   Current EL with SP_EL0  FIQ
//   0x180   Current EL with SP_EL0  SError
//   0x200   Current EL with SP_ELx  Synchronous   ← LIVE entry (P1-F)
//   0x280   Current EL with SP_ELx  IRQ            ← LIVE entry (P1-G)
//   0x300   Current EL with SP_ELx  FIQ            (unexpected)
//   0x380   Current EL with SP_ELx  SError         (unexpected)
//   0x400   Lower EL using AArch64  Synchronous   (no userspace yet)
//   0x480   Lower EL using AArch64  IRQ            (no userspace yet)
//   0x500   Lower EL using AArch64  FIQ            (no userspace yet)
//   0x580   Lower EL using AArch64  SError         (no userspace yet)
//   0x600   Lower EL using AArch32  Synchronous   (we don't run AArch32)
//   0x680   Lower EL using AArch32  IRQ            (we don't run AArch32)
//   0x700   Lower EL using AArch32  FIQ            (we don't run AArch32)
//   0x780   Lower EL using AArch32  SError         (we don't run AArch32)
//
// At P1-G the Sync slot terminates in extinction; the IRQ slot is the
// first recoverable path — vectors.S calls exception_irq_curr_el and
// then resumes the interrupted code via the shared .Lexception_return
// trampoline (KERNEL_EXIT lives there to keep the slot ≤ 0x80 bytes;
// `.L` prefix keeps it out of the symbol table).
// All other 14 entries call exception_unexpected with the vector index,
// which extinctions. Phase 2 fills lower-EL entries when userspace lands.
//
// KNOWN LIMITATION: the handler runs on the existing SP_EL1 (i.e., the
// boot stack at P1-F). A stack overflow that triggers a fault then
// recursively faults inside KERNEL_ENTRY because SP is in the guard
// region. The recursion wedges QEMU before extinction prints anything.
// Mitigation: per-CPU exception stack lands at Phase 2 with thread
// management. Until then, kernel-stack overflow is detected only when
// the stack is corrupted gradually (so SP still has headroom on the
// faulting access).

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
    b       _hang                   // unreached; silences the assembler
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
    // Current EL with SP_EL0 — kernel running on SP_EL0. We never use
    // SP_EL0 at EL1, so any of these entries firing means kernel state
    // got corrupted. extinction.
    // ------------------------------------------------------------------
    .org _exception_vectors + 0x000
    VEC_UNEXPECTED 0

    .org _exception_vectors + 0x080
    VEC_UNEXPECTED 1

    .org _exception_vectors + 0x100
    VEC_UNEXPECTED 2

    .org _exception_vectors + 0x180
    VEC_UNEXPECTED 3

    // ------------------------------------------------------------------
    // Current EL with SP_ELx — the live entries. Sync goes to the
    // real handler; IRQ / FIQ / SError extinction (P1-G adds GIC + IRQ).
    //
    // At P1-F every sync exception terminates in extinction (via
    // exception_sync_curr_el's noreturn paths), so we don't run
    // KERNEL_EXIT inline — it would push the slot over 0x80 bytes.
    // KERNEL_EXIT remains defined for Phase 2's recoverable
    // page-fault handler.
    // ------------------------------------------------------------------
    .org _exception_vectors + 0x200      /* Sync */
    KERNEL_ENTRY
    mov     x0, sp
    bl      exception_sync_curr_el
    b       _hang                        /* unreached if extinction took over */

    .org _exception_vectors + 0x280      /* IRQ — recoverable */
    KERNEL_ENTRY
    mov     x0, sp
    bl      exception_irq_curr_el
    b       .Lexception_return           /* shared KERNEL_EXIT trampoline (local) */

    .org _exception_vectors + 0x300      /* FIQ */
    VEC_UNEXPECTED 6

    .org _exception_vectors + 0x380      /* SError */
    VEC_UNEXPECTED 7

    // ------------------------------------------------------------------
    // Lower EL using AArch64 — userspace lands at Phase 2.
    // ------------------------------------------------------------------
    .org _exception_vectors + 0x400
    VEC_UNEXPECTED 8

    .org _exception_vectors + 0x480
    VEC_UNEXPECTED 9

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
