// ARM64 exception handlers.
//
// vectors.S saves all GP regs + special regs into a struct
// exception_context, then calls into the C handlers below. Each
// handler decides whether to:
//   - Recover (P1-G: IRQ handler returns normally; vectors.S issues
//     KERNEL_EXIT to ERET. Phase 2's page-fault handler will COW etc.).
//   - extinction with a specific diagnostic.
//
// At P1-G the live diagnostics + handlers are:
//   - "kernel stack overflow" — translation/permission fault with
//     FAR inside the boot-stack guard region.
//   - "PTE violates W^X" — permission fault in kernel image area
//     (write to .text/.rodata or exec from .data/.bss).
//   - IRQ — gic_acknowledge → gic_dispatch → gic_eoi.
// All other faults extinction with the raw ESR/FAR/ELR for forensic
// analysis.
//
// Per ARCHITECTURE.md §12.

#include "exception.h"
#include "fault.h"                    // P3-C: arch_fault_handle / fault_info_decode
#include "gic.h"

#include <thylacine/extinction.h>
#include <thylacine/types.h>

// Vector table from vectors.S.
extern char _exception_vectors[];

// ---------------------------------------------------------------------------
// Sanity-check the exception_context layout. vectors.S writes by
// fixed byte offsets; if the struct ever drifts these asserts fail
// the build before we miscompare register-saved state at runtime.
// ---------------------------------------------------------------------------

_Static_assert(sizeof(struct exception_context) == EXCEPTION_CTX_SIZE,
               "exception_context size mismatch with EXCEPTION_CTX_SIZE");
_Static_assert(__builtin_offsetof(struct exception_context, regs) == 0,
               "regs[0] must be at offset 0");
_Static_assert(__builtin_offsetof(struct exception_context, sp) == 0xF8,
               "sp must be at offset 0xF8");
_Static_assert(__builtin_offsetof(struct exception_context, elr) == 0x100,
               "elr must be at offset 0x100");
_Static_assert(__builtin_offsetof(struct exception_context, spsr) == 0x108,
               "spsr must be at offset 0x108");
_Static_assert(__builtin_offsetof(struct exception_context, esr) == 0x110,
               "esr must be at offset 0x110");
_Static_assert(__builtin_offsetof(struct exception_context, far) == 0x118,
               "far must be at offset 0x118");

// ---------------------------------------------------------------------------
// ESR_EL1 EC values used directly by the sync-handler dispatch.
// Page-fault classification + handling moved to arch/arm64/fault.{h,c}
// at P3-C; this file dispatches data/instruction aborts there and
// handles the remaining sync exceptions inline.
//
// Per ARM ARM D17.2.40.
// ---------------------------------------------------------------------------

#define ESR_EC_SHIFT       26
#define ESR_EC(esr)        (((esr) >> ESR_EC_SHIFT) & 0x3F)

#define EC_INST_ABORT_LOWER 0x20    /* instruction abort from lower EL */
#define EC_INST_ABORT_SAME 0x21     /* instruction abort from current EL */
#define EC_DATA_ABORT_LOWER 0x24
#define EC_DATA_ABORT_SAME 0x25
#define EC_SP_ALIGN        0x26
#define EC_PC_ALIGN        0x22
#define EC_BTI             0x0D     /* Branch Target Exception (FEAT_BTI) */
#define EC_BRK             0x3C     /* deliberate brk #imm */
#define EC_SVC_AARCH64     0x15     /* svc #imm at EL0 (AArch64) */

// ---------------------------------------------------------------------------
// exception_init.
// ---------------------------------------------------------------------------

void exception_init(void) {
    u64 vbar = (u64)(uintptr_t)_exception_vectors;
    __asm__ __volatile__("msr vbar_el1, %0\n" "isb\n"
                         :: "r" (vbar) : "memory");
}

// ---------------------------------------------------------------------------
// Sync handler at current EL with SPx (the kernel's normal mode).
//
// Page-fault classification (data/instruction aborts) is handed to
// arch_fault_handle in fault.c. Other sync exceptions (alignment, BTI,
// BRK) are handled inline below.
// ---------------------------------------------------------------------------

void exception_sync_curr_el(struct exception_context *ctx) {
    u64 esr = ctx->esr;
    u64 far = ctx->far;
    u32 ec  = (u32)ESR_EC(esr);

    switch (ec) {
    case EC_DATA_ABORT_SAME:
    case EC_INST_ABORT_SAME: {
        // P3-C: structured fault dispatch. Decode + classify, then hand
        // to arch_fault_handle. The handler either resolves the fault
        // (returns FAULT_HANDLED — ERET resumes the interrupted
        // instruction) or extincts internally with a specific
        // diagnostic. FAULT_UNHANDLED_USER is unreachable here (this
        // handler is wired to the Current EL/SPx vector, never sees
        // lower-EL aborts); P3-E will add `exception_sync_lower_el`
        // with the same dispatcher and the user-mode handling.
        struct fault_info fi;
        fault_info_decode(esr, far, ctx->elr, &fi);
        enum fault_result r = arch_fault_handle(&fi);

        switch (r) {
        case FAULT_HANDLED:
            return;          // ERET resumes the faulting instruction.
        case FAULT_UNHANDLED_USER:
            extinction_with_addr("unhandled user-mode fault (no VMA / SIGSEGV pending)",
                                 (uintptr_t)fi.vaddr);
        case FAULT_FATAL:
            // Reserved — current arch_fault_handle paths extinct
            // internally. Defense-in-depth.
            extinction_with_addr("arch_fault_handle returned FAULT_FATAL",
                                 (uintptr_t)fi.vaddr);
        }
        // Unreachable (extinction is noreturn); fallthrough silences any
        // future compiler warning if the enum gains new values.
        extinction_with_addr("arch_fault_handle returned unknown result",
                             (uintptr_t)fi.vaddr);
    }

    case EC_SP_ALIGN:
        extinction_with_addr("SP alignment fault", (uintptr_t)far);

    case EC_PC_ALIGN:
        extinction_with_addr("PC alignment fault", (uintptr_t)ctx->elr);

    case EC_BTI:
        // ARMv8.5+ Branch Target Exception. Raised when an indirect
        // branch (br/blr) lands on an instruction that is NOT a `bti
        // j/c/jc` matching PSTATE.BTYPE, IF the target page has
        // PTE_GP=1 AND SCTLR_EL1.BT0=1. Both conditions are set up in
        // start.S + mmu.h at boot. Forging an indirect-call target to
        // bypass the canary would land here on FEAT_BTI hardware.
        extinction_with_addr("BTI fault (indirect branch to non-guarded target)",
                             (uintptr_t)ctx->elr);

    case EC_BRK:
        // Deliberate brk #imm — assertion failure or debug trap.
        // ESR.ISS[15:0] = brk immediate.
        extinction_with_addr("brk instruction (assertion?)",
                             (uintptr_t)ctx->elr);

    default:
        extinction_with_addr("unhandled sync exception (EC in ESR_EL1)",
                             (uintptr_t)esr);
    }
}

// ---------------------------------------------------------------------------
// Catch-all for unexpected vector entries (every entry except sync at
// current EL with SPx, at P1-F). vector_idx is the index 0..15 from
// vectors.S so the diagnostic narrows down which one fired.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// IRQ handler at current EL with SPx.
//
// Acknowledge → dispatch → EOI is the GICv3 single-step flow with
// ICC_CTLR_EL1.EOImode=0. A spurious INTID (1023) skips the dispatch
// AND the EOI per ARM IHI 0069 §3.7.
//
// Returns normally; vectors.S issues KERNEL_EXIT (eret) on return,
// resuming the interrupted code with all GP regs restored.
// ---------------------------------------------------------------------------

void exception_irq_curr_el(struct exception_context *ctx) {
    (void)ctx;       // available for scheduler use at Phase 2
    u32 intid = gic_acknowledge();
    // INTIDs 1020..1023 are reserved per ARM IHI 0069 §2.2.1 (1023 is
    // explicitly "spurious"; 1020..1022 are also reserved and must not
    // be dispatched or EOI'd). Treat the full range as spurious.
    if (intid >= GIC_NUM_INTIDS) {
        return;
    }
    gic_dispatch(intid);
    gic_eoi(intid);
}

// =============================================================================
// P3-Ea: sync handler for EL0 exceptions.
// =============================================================================
//
// Counterpart to exception_sync_curr_el for EL0 → EL1 sync exceptions.
// Routes through the same arch_fault_handle dispatcher — fault_info_decode
// inspects EC and sets `from_user=true` for EC_DATA_ABORT_LOWER /
// EC_INST_ABORT_LOWER, which routes the user-mode case through
// userland_demand_page (P3-Dc).
//
// On FAULT_HANDLED the function returns normally; vectors.S slot 0x400
// branches to .Lexception_return which ERETs to ELR_EL1 (the faulting
// EL0 instruction).
//
// On FAULT_UNHANDLED_USER (no VMA / permission denied) the handler
// extincts. Phase 5+ note delivery upgrades this to a SIGSEGV-like
// note.
//
// SVC (EC 0x15) extincts at v1.0 — userspace syscalls land at P3-Ec.
//
// EC_PC_ALIGN / EC_SP_ALIGN / EC_BTI / EC_BRK from EL0 also extinct
// at v1.0; Phase 5+ note delivery handles these as user-faults.
void exception_sync_lower_el(struct exception_context *ctx) {
    u64 esr = ctx->esr;
    u64 far = ctx->far;
    u32 ec  = (u32)ESR_EC(esr);

    switch (ec) {
    case EC_DATA_ABORT_LOWER:
    case EC_INST_ABORT_LOWER: {
        struct fault_info fi;
        fault_info_decode(esr, far, ctx->elr, &fi);
        enum fault_result r = arch_fault_handle(&fi);

        switch (r) {
        case FAULT_HANDLED:
            return;          // ERET resumes the faulting EL0 instruction.
        case FAULT_UNHANDLED_USER:
            // Phase 5+: deliver SIGSEGV-like note to the offending
            // Proc; the thread terminates rather than the kernel
            // extincting. At v1.0 we extinct loudly so test failures
            // surface immediately rather than the user thread silently
            // dying.
            extinction_with_addr("EL0 fault: no VMA covers vaddr / permission denied",
                                 (uintptr_t)fi.vaddr);
        case FAULT_FATAL:
            extinction_with_addr("arch_fault_handle returned FAULT_FATAL (EL0)",
                                 (uintptr_t)fi.vaddr);
        }
        extinction_with_addr("arch_fault_handle returned unknown result (EL0)",
                             (uintptr_t)fi.vaddr);
    }

    case EC_SVC_AARCH64:
        // svc #imm at EL0. The syscall dispatcher lands at P3-Ec; v1.0
        // pre-Ec extincts so an accidental syscall instruction in early
        // userspace surfaces.
        extinction_with_addr("EL0 SVC (userspace syscall) — not implemented at v1.0",
                             (uintptr_t)ctx->elr);

    case EC_PC_ALIGN:
        extinction_with_addr("EL0 PC alignment fault", (uintptr_t)ctx->elr);

    case EC_SP_ALIGN:
        extinction_with_addr("EL0 SP alignment fault", (uintptr_t)far);

    case EC_BTI:
        extinction_with_addr("EL0 BTI fault (indirect branch to non-guarded target)",
                             (uintptr_t)ctx->elr);

    case EC_BRK:
        extinction_with_addr("EL0 brk instruction (assertion?)",
                             (uintptr_t)ctx->elr);

    default:
        extinction_with_addr("EL0 unhandled sync exception (EC in ESR_EL1)",
                             (uintptr_t)esr);
    }
}

void exception_unexpected(struct exception_context *ctx, u64 vector_idx) {
    // Stash ESR / FAR / ELR / vector_idx into recognizable diagnostic.
    // extinction_with_addr is the closest match in the existing API;
    // we encode the vector index in the message so a developer can
    // tell which vector fired.
    static const char *names[16] = {
        "[Curr EL/SP0] Sync",      "[Curr EL/SP0] IRQ",
        "[Curr EL/SP0] FIQ",       "[Curr EL/SP0] SError",
        "[Curr EL/SPx] Sync",      "[Curr EL/SPx] IRQ",
        "[Curr EL/SPx] FIQ",       "[Curr EL/SPx] SError",
        "[Lower EL a64] Sync",     "[Lower EL a64] IRQ",
        "[Lower EL a64] FIQ",      "[Lower EL a64] SError",
        "[Lower EL a32] Sync",     "[Lower EL a32] IRQ",
        "[Lower EL a32] FIQ",      "[Lower EL a32] SError",
    };
    const char *name = vector_idx < 16 ? names[vector_idx] : "[? vector]";
    (void)ctx;          // ESR/FAR/ELR available if a future handler wants them
    extinction(name);   // halts; never returns
}
