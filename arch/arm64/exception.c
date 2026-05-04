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
#include "gic.h"
#include "kaslr.h"

#include <thylacine/extinction.h>
#include <thylacine/types.h>

// Linker symbols — boot stack guard region.
extern char _boot_stack_guard[];
extern char _boot_stack_bottom[];
extern char _kernel_start[];
extern char _kernel_end[];

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
// ESR_EL1 decode helpers.
//
// ESR_EL1[31:26] = EC (exception class).
// ESR_EL1[25]    = IL (instruction length: 0=16-bit, 1=32-bit).
// ESR_EL1[24:0]  = ISS (instruction-specific syndrome; semantics vary by EC).
//
// Per ARM ARM D17.2.40.
// ---------------------------------------------------------------------------

#define ESR_EC_SHIFT       26
#define ESR_EC_MASK        (0x3Full << ESR_EC_SHIFT)
#define ESR_EC(esr)        (((esr) >> ESR_EC_SHIFT) & 0x3F)

#define EC_UNKNOWN         0x00
#define EC_WFI_WFE         0x01
#define EC_SVC_AARCH64     0x15
#define EC_HVC_AARCH64     0x16
#define EC_SMC_AARCH64     0x17
#define EC_TRAPPED_MSR_MRS 0x18
#define EC_INST_ABORT_LOWER 0x20    /* instruction abort from lower EL */
#define EC_INST_ABORT_SAME 0x21     /* instruction abort from current EL */
#define EC_PC_ALIGN        0x22
#define EC_DATA_ABORT_LOWER 0x24
#define EC_DATA_ABORT_SAME 0x25
#define EC_SP_ALIGN        0x26
#define EC_FP_AARCH64      0x2C
#define EC_BRK             0x3C     /* deliberate brk #imm */

// For data / instruction aborts, ISS[5:0] = DFSC / IFSC (fault status code).
#define FSC_TRANS_FAULT_L0 0x04
#define FSC_TRANS_FAULT_L1 0x05
#define FSC_TRANS_FAULT_L2 0x06
#define FSC_TRANS_FAULT_L3 0x07
#define FSC_PERM_FAULT_L1  0x0D
#define FSC_PERM_FAULT_L2  0x0E
#define FSC_PERM_FAULT_L3  0x0F
#define FSC_MASK           0x3F

static inline u32 esr_fsc(u64 esr) {
    return (u32)(esr & FSC_MASK);
}

static inline bool fsc_is_translation(u32 fsc) {
    return fsc >= FSC_TRANS_FAULT_L0 && fsc <= FSC_TRANS_FAULT_L3;
}

static inline bool fsc_is_permission(u32 fsc) {
    return fsc >= FSC_PERM_FAULT_L1 && fsc <= FSC_PERM_FAULT_L3;
}

// ---------------------------------------------------------------------------
// PA / VA helpers.
//
// FAR_EL1 holds the faulting VA. For accesses that went through TTBR0
// (low VA == PA via identity), FAR is the PA. For accesses that went
// through TTBR1 (high VA), FAR is a high VA in the kernel image's
// runtime region.
//
// To check whether FAR is in the boot-stack guard region we need its
// PA bounds. _boot_stack_guard's compile-time symbol is a high VA at
// runtime; we convert via the kaslr.c PA accessors.
// ---------------------------------------------------------------------------

static u64 high_va_to_pa(u64 high_va) {
    u64 high_va_kernel_start = kaslr_kernel_high_base();
    u64 pa_kernel_start = kaslr_kernel_pa_start();
    return high_va - high_va_kernel_start + pa_kernel_start;
}

static u64 sym_to_pa(const void *sym) {
    return high_va_to_pa((u64)(uintptr_t)sym);
}

// True iff `addr` falls inside the boot-stack guard page. Accepts both
// PAs (the typical case for stack accesses, since SP is PA at P1-F)
// and high VAs (in case some future code uses high-VA stacks).
static bool addr_is_stack_guard(u64 addr) {
    u64 guard_pa = sym_to_pa(_boot_stack_guard);
    u64 bottom_pa = sym_to_pa(_boot_stack_bottom);
    if (addr >= guard_pa && addr < bottom_pa) return true;

    // High-VA fallback (no high-VA stack today, but check anyway).
    u64 guard_va = (u64)(uintptr_t)_boot_stack_guard;
    u64 bottom_va = (u64)(uintptr_t)_boot_stack_bottom;
    return addr >= guard_va && addr < bottom_va;
}

// True iff `addr` falls inside the kernel image (TEXT / RODATA /
// DATA / BSS / dynamic-section family). Used to recognize W^X
// violations on the kernel image specifically.
static bool addr_is_kernel_image(u64 addr) {
    u64 ks_pa = kaslr_kernel_pa_start();
    u64 ke_pa = kaslr_kernel_pa_end();
    if (addr >= ks_pa && addr < ke_pa) return true;

    u64 ks_va = (u64)(uintptr_t)_kernel_start;
    u64 ke_va = (u64)(uintptr_t)_kernel_end;
    return addr >= ks_va && addr < ke_va;
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
// Decisions in priority order:
//   1. Permission fault on stack-guard region → kernel stack overflow.
//   2. Permission fault on kernel image       → W^X violation.
//   3. Translation fault                       → unhandled page fault.
//   4. BRK exception (deliberate brk #imm)     → assertion failure.
//   5. Anything else                            → generic extinction.
// ---------------------------------------------------------------------------

void exception_sync_curr_el(struct exception_context *ctx) {
    u64 esr = ctx->esr;
    u64 far = ctx->far;
    u32 ec  = (u32)ESR_EC(esr);
    u32 fsc = esr_fsc(esr);

    switch (ec) {
    case EC_DATA_ABORT_SAME:
    case EC_INST_ABORT_SAME:
        if (addr_is_stack_guard(far)) {
            extinction_with_addr("kernel stack overflow", (uintptr_t)far);
        }
        if (fsc_is_permission(fsc) && addr_is_kernel_image(far)) {
            extinction_with_addr("PTE violates W^X (kernel image)",
                                 (uintptr_t)far);
        }
        if (fsc_is_translation(fsc)) {
            extinction_with_addr("unhandled translation fault",
                                 (uintptr_t)far);
        }
        if (fsc_is_permission(fsc)) {
            extinction_with_addr("unhandled permission fault",
                                 (uintptr_t)far);
        }
        extinction_with_addr("data/instruction abort", (uintptr_t)far);

    case EC_SP_ALIGN:
        extinction_with_addr("SP alignment fault", (uintptr_t)far);

    case EC_PC_ALIGN:
        extinction_with_addr("PC alignment fault", (uintptr_t)ctx->elr);

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
