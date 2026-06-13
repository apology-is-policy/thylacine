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
#include "halls.h"                    // HX-1: per-CPU live-frame tracking for the crash dump
#include "uaccess.h"                  // R12-uaccess: kernel-mode user-VA fault-fixup

#include <thylacine/extinction.h>
#include <thylacine/notes.h>          // P6 #3a: NOTE_NAME_SNARE_* constants for proc_fault_terminate
#include <thylacine/proc.h>           // R12-uaccess: struct Proc for demand-page synth; P6 #3a: proc_fault_terminate
#include <thylacine/syscall.h>        // P3-Ec: syscall_dispatch
#include <thylacine/thread.h>         // R12-uaccess: current_thread()
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
// HX-1: per-CPU live-frame tracking. Each public exception entry point is a
// thin wrapper that records its register frame in the per-CPU Halls slot for
// the duration of the handler, so halls_dump (reached from extinction) reads
// the dying frame without threading ctx through every signature. A normal
// return restores the previous slot; a noreturn extinction inside the handler
// skips the restore, leaving the slot at the dying frame; a BLOCKING handler
// that resumes on another CPU runs its restore there, stranding the slot.
// halls_dump never trusts the raw slot regardless -- HX-I4's plausibility gate
// rejects an implausible slot and falls back to capture-current (see halls.c).
// ---------------------------------------------------------------------------

static void exception_sync_curr_el_impl(struct exception_context *ctx);
static void exception_irq_curr_el_impl(struct exception_context *ctx);
static void exception_sync_lower_el_impl(struct exception_context *ctx);
__attribute__((noreturn))
static void exception_unexpected_impl(struct exception_context *ctx, u64 vector_idx);

void exception_sync_curr_el(struct exception_context *ctx) {
    const struct exception_context *prev = halls_enter_frame(ctx);
    exception_sync_curr_el_impl(ctx);
    halls_leave_frame(prev);
}

void exception_irq_curr_el(struct exception_context *ctx) {
    const struct exception_context *prev = halls_enter_frame(ctx);
    exception_irq_curr_el_impl(ctx);
    halls_leave_frame(prev);
}

void exception_sync_lower_el(struct exception_context *ctx) {
    const struct exception_context *prev = halls_enter_frame(ctx);
    exception_sync_lower_el_impl(ctx);
    halls_leave_frame(prev);
}

void exception_unexpected(struct exception_context *ctx, u64 vector_idx) {
    // F6: the discarded prev + missing leave are safe ONLY because
    // exception_unexpected_impl is noreturn (it always extincts) -- the
    // machine dies before the slot's staleness matters, and halls_dump reads
    // the frame we just set. If this vector group is ever made recoverable
    // (vectors.S flags a future SError-recovery stack), restore save/restore
    // symmetry with the other three wrappers (prev = halls_enter_frame(ctx)
    // ... halls_leave_frame(prev)).
    halls_enter_frame(ctx);
    exception_unexpected_impl(ctx, vector_idx);
}

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

static void exception_sync_curr_el_impl(struct exception_context *ctx) {
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

        // R12-uaccess: kernel-mode user-VA fault recovery. SYS_PUTS
        // and similar kernel-mode user-VA dereferences may fault on
        // a translation if the user page is in a VMA but not yet
        // PTE-installed (lazy demand-paging hasn't fired). Pre-R12,
        // these reached arch_fault_handle's "unhandled kernel
        // translation fault" extinction. Now we:
        //   1. Recognize FAR in user half + faulting PC in fixup table.
        //   2. Call userland_demand_page on the current Proc.
        //      userland_demand_page reads vaddr + is_write +
        //      is_instruction only — `from_user` is not inspected,
        //      so the kernel-mode fi is passed through unchanged.
        //      On success the PTE is installed and ERET re-executes
        //      the load.
        //   3. On demand-page failure (no VMA / perm denied / OOM),
        //      transfer control to the fixup label by overwriting
        //      ctx->elr — the fixup label returns -1 to the caller
        //      (e.g. uaccess_load_u8 → -1 → SYS_PUTS returns -1).
        // FAR's user-half check is the only thing distinguishing
        // uaccess from a genuine kernel-pointer-corruption fault
        // (which still extincts via arch_fault_handle below).
        if (!fi.from_user && fi.vaddr < UACCESS_USER_VA_TOP &&
            (fi.is_translation || fi.is_permission || fi.is_access_flag)) {
            u64 fixup_pc = uaccess_fixup_lookup(fi.elr);
            if (fixup_pc != 0) {
                struct Thread *t = current_thread();
                if (t && t->magic == THREAD_MAGIC &&
                    t->proc && t->proc->magic == PROC_MAGIC &&
                    t->proc->pgtable_root != 0) {
                    // R12-uaccess F211 close: pass `fi` (kernel-mode
                    // fault) through unchanged rather than
                    // synthesizing from_user=true. userland_demand_page
                    // intentionally ignores from_user; a synthesis
                    // here would leave fi.ec mismatched against
                    // fi.from_user (EC_DATA_ABORT_SAME vs the
                    // EC_DATA_ABORT_LOWER that the rest of the
                    // fault_info contract implies for user-mode
                    // faults). Less synthesis = less to keep
                    // consistent.
                    if (userland_demand_page(t->proc, &fi) == FAULT_HANDLED) {
                        return;   // ERET re-executes the faulting load.
                    }
                }
                // Demand-page failed (no thread / no proc / no VMA /
                // perm denied / OOM). Hand off to the primitive's
                // fixup label so the syscall surface returns -1
                // instead of extincting.
                ctx->elr = fixup_pc;
                return;
            }
        }

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

static void exception_irq_curr_el_impl(struct exception_context *ctx) {
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
// terminates the faulting Proc via proc_fault_terminate(NOTE_NAME_SNARE_*,
// addr) -- the kernel does NOT extinct. Per docs/ERRORS.md (scripture
// e45a571) the snare:* note name family signals the fault kind to the
// parent's wait_pid + uart log; v1.x adds POSIX signal mapping +
// structured 64-bit exit_status.
//
// SVC (EC 0x15) routes through syscall_dispatch at v1.0 -- userspace
// syscalls land at P3-Ec; does NOT extinct.
//
// EC_PC_ALIGN / EC_SP_ALIGN / EC_BTI / EC_BRK from EL0 also terminate
// via proc_fault_terminate (snare:align / snare:bti / snare:brk
// respectively).
// P6-pouch-signals-impl (sub-chunk 13a): the EL0-return-tail note-delivery hook
// (defined in kernel/notes.c). As of #107 it is invoked from the vector tails
// (vectors.S .Lel0_sync_return for sync-from-EL0, 0x480 for IRQ-from-EL0), not
// from the C handlers here; the decl is retained for documentation.
void notes_deliver_at_el0_return(struct exception_context *ctx);

// SYS_EXIT_GROUP / kill cross-thread shootdown (ARCH §7.9.1, I-24): the
// EL0-return-tail die-check (defined in kernel/proc.c). If the calling
// Thread's Proc is group-terminating, the Thread self-exits here (noreturn);
// otherwise returns. As of #107 invoked from the vector tails (vectors.S
// .Lel0_sync_return + 0x480), AFTER preempt_check_irq -- so a group-terminate
// landing during the preempt is caught before any EL0 instruction runs.
void el0_return_die_check(void);

static void exception_sync_lower_el_impl(struct exception_context *ctx) {
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
            // #107: the EL0-return tail -- preempt_check_irq ->
            // el0_return_die_check (I-24) -> notes_deliver_at_el0_return -- now
            // runs at the VECTOR level (vectors.S .Lel0_sync_return), AFTER this
            // handler returns and its halls frame is closed. That clean-frame
            // preempt mirrors the 0x480 IRQ slot and avoids the mid-handler steal
            // hazard (#104). The fault was resolved; ERET (via KERNEL_EXIT)
            // resumes the faulting EL0 instruction, with the die-check + any
            // pending note applied on the way out.
            return;
        case FAULT_UNHANDLED_USER:
            // P6 hardening #3a (scripture e45a571): terminate the
            // faulting Proc with the snare:segv tag; the kernel does
            // NOT extinct. proc_fault_terminate emits a uart
            // diagnostic + calls exits(NOTE_NAME_SNARE_SEGV). Parent
            // reaps via wait_pid; exit_status = 1 at v1.0.
            // Diagnostic preserves the visibility the prior
            // extinction_with_addr line provided.
            proc_fault_terminate(NOTE_NAME_SNARE_SEGV, (uintptr_t)fi.vaddr);
        case FAULT_FATAL:
            // F3 audit close: FAULT_FATAL's documented contract (per
            // arch/arm64/fault.h) is "kernel-side invariant violation;
            // the dispatcher extincted internally; this return value
            // is reserved for paths that can't extinct from inside."
            // It is NOT "user-mode SIGBUS-class fault". Routing it to
            // snare:bus would silently terminate the offending user
            // Proc while leaving the kernel in the broken state that
            // FAULT_FATAL was meant to surface. Keep extinction so a
            // future arch_fault_handle path returning FAULT_FATAL per
            // the documented contract is escalated, not masked.
            // SIGBUS-class user faults (the v1.x lift) will land via
            // a new dedicated enum value or via FAULT_UNHANDLED_USER
            // with a sub-classification, not by overloading FAULT_FATAL.
            extinction_with_addr("arch_fault_handle returned FAULT_FATAL (EL0)",
                                 (uintptr_t)fi.vaddr);
        }
        // Unknown fault_result from arch_fault_handle is a kernel-side
        // bug (enum grew without this switch updating). Extinct -- the
        // dispatcher itself can't classify the user fault, so we can't
        // attribute a snare:* tag confidently.
        extinction_with_addr("arch_fault_handle returned unknown result (EL0)",
                             (uintptr_t)fi.vaddr);
    }

    case EC_SVC_AARCH64:
        // svc #imm at EL0. P3-Ec: route through syscall_dispatch which
        // reads nr from x8, args from x0..x5, writes return value to
        // x0. SYS_EXITS does not return (kernel exits() + sched()
        // pick another thread). All other syscalls return normally;
        // vectors.S ERETs to EL0 with the result in x0.
        syscall_dispatch(ctx);
        // EL0-return tail runs at the VECTOR level now (vectors.S
        // .Lel0_sync_return), AFTER this handler returns + its halls frame is
        // closed: (1) preempt_check_irq -- the #107 syscall-return preempt, the
        // RW-11 SA-1b wake-preemption consumer, re-added at a CLEAN saved frame
        // mirroring the 0x480 IRQ slot (the #104 deadlock was a per-CPU `cs`
        // TOCTOU in sched(), fixed at the root by masking IRQs before the
        // this_cpu_sched() read -- so this preempt no longer risks the
        // mid-handler steal/leak); (2) el0_return_die_check -- I-24, AFTER the
        // preempt so a Proc group-terminated during the preempt-switch is caught
        // before any EL0 instruction runs; (3) notes_deliver_at_el0_return -- the
        // P6-pouch-signals async note delivery. SYS_EXITS / a die path do not
        // return here (kernel exits() + sched()).
        return;

    case EC_PC_ALIGN:
        // EL0 PC alignment fault -- terminate Proc with snare:align.
        proc_fault_terminate(NOTE_NAME_SNARE_ALIGN, (uintptr_t)ctx->elr);

    case EC_SP_ALIGN:
        // EL0 SP alignment fault -- terminate Proc with snare:align.
        proc_fault_terminate(NOTE_NAME_SNARE_ALIGN, (uintptr_t)far);

    case EC_BTI:
        // EL0 indirect branch to non-`bti j/c/jc` target -- terminate
        // with snare:bti. On FEAT_BTI hardware this catches the
        // forge-indirect-call leg of a ROP/JOP chain; v1.0 boot is
        // green on QEMU virt which doesn't raise BTI (defense-in-
        // depth for hardware that does).
        proc_fault_terminate(NOTE_NAME_SNARE_BTI, (uintptr_t)ctx->elr);

    case EC_BRK:
        // EL0 brk #imm -- userspace assertion or debug trap.
        // Terminate with snare:brk. Debuggers attaching at EL0 are a
        // Phase 5+ concern; v1.0 treats all EL0 brk as fatal-to-Proc.
        proc_fault_terminate(NOTE_NAME_SNARE_BRK, (uintptr_t)ctx->elr);

    default:
        // Unknown EC from EL0 -- terminate with snare:ill (illegal
        // instruction / unknown sync EC). ESR encoded in the addr
        // slot for forensics.
        proc_fault_terminate(NOTE_NAME_SNARE_ILL, (uintptr_t)esr);
    }
}

static void exception_unexpected_impl(struct exception_context *ctx, u64 vector_idx) {
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
