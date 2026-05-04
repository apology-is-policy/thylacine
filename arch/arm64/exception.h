// ARM64 exception handling — synchronous fault dispatch + IRQ stub.
//
// At P1-F: the vector table is armed; sync faults route to ESR/FAR
// decode and the first deliberate extinction callers (stack-overflow
// detection on the boot-stack guard page, W^X-violation detection on
// kernel image writes/execs). IRQ / FIQ / SError / lower-EL entries
// all extinction with an index — they're unexpected at this phase
// (no GIC, no userspace yet). GIC + IRQ-driven UART land at P1-G or a
// P1-F-extras chunk; userspace exception entry at Phase 2.
//
// Per ARCHITECTURE.md §12 (interrupt handling).

#ifndef THYLACINE_ARCH_ARM64_EXCEPTION_H
#define THYLACINE_ARCH_ARM64_EXCEPTION_H

// Total size: 31 + 5 = 36 u64s = 288 bytes. Exposed as a constant so
// vectors.S can #include this header (via .S preprocessing) and use
// the same value in `sub sp, sp, #EXCEPTION_CTX_SIZE`.
#define EXCEPTION_CTX_SIZE   288

#ifndef __ASSEMBLER__

#include <thylacine/types.h>

// State saved on exception entry, to be restored on eret.
//
// LAYOUT IS LOAD-BEARING — vectors.S's KERNEL_ENTRY macro writes
// fields by byte offset. _Static_asserts in exception.c verify the
// offsets match. If you reorder fields here, fix vectors.S in the
// same commit.
struct exception_context {
    u64 regs[31];   // x0..x30 (lr is x30)
    u64 sp;         // SP_EL0 at fault (irrelevant for current-EL faults but saved for symmetry)
    u64 elr;        // ELR_EL1 (faulting / interrupted PC)
    u64 spsr;       // SPSR_EL1 (process state at fault)
    u64 esr;        // ESR_EL1 (exception syndrome)
    u64 far;        // FAR_EL1 (faulting address; valid for translation / alignment faults)
};

// Set VBAR_EL1 to the kernel exception vector table. Call from
// boot_main once MMU is on and the kernel is running at high VA.
//
// Until exception_init runs, exceptions are undefined behavior
// (boot path takes care not to trigger any).
void exception_init(void);

// C handlers called by vectors.S. The vector table is laid out as
// 4 "EL source" groups × 4 "exception type" entries each = 16 total.
//
// At P1-F we route:
//   - Current-EL-SPx Sync     → exception_sync_curr_el
//   - All other 15 entries    → exception_unexpected
//
// (Lower-EL and Current-EL-SP0 entries are unexpected at this phase
// — no userspace yet, kernel always uses SP_EL1.)
__attribute__((noreturn))
void exception_unexpected(struct exception_context *ctx, u64 vector_idx);

void exception_sync_curr_el(struct exception_context *ctx);

#endif // __ASSEMBLER__

#endif // THYLACINE_ARCH_ARM64_EXCEPTION_H
