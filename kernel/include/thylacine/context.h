// ARM64 thread saved-register context for cpu_switch_context.
//
// Per ARCHITECTURE.md §7.3 (Thread struct) + §8 (Scheduler). Only the
// AAPCS64 callee-saved registers plus SP, LR (resume PC), and TPIDR_EL0
// (TLS pointer) need preservation across a context switch — caller-saved
// registers (x0-x18) are clobbered by any function call from the C
// compiler's perspective, including the cpu_switch_context call.
//
// Layout is the contract between C (struct Thread embedding) and the
// asm switcher in arch/arm64/context.S. Field order MUST match the
// offset table in context.S; _Static_asserts below pin both.

#ifndef THYLACINE_CONTEXT_H
#define THYLACINE_CONTEXT_H

#include <stddef.h>
#include <thylacine/types.h>

// Context layout — 15 u64 GP region (120 bytes, 8-byte aligned) + 8 B
// pad + 512 B fp_v + 4 B fpsr + 4 B fpcr. Total 648 bytes, struct itself
// _Alignas(16) so fp_v at offset 128 satisfies STP/LDP Q-reg alignment
// per ARM ARM C7.2.348.
//
// P3-Bdb: ttbr0 — the value to write to TTBR0_EL1 on context switch
// into this thread. Encodes (ASID << 48) | pgtable_root_PA. kproc
// threads carry the "kernel-only" TTBR0 (l0_ttbr0 PA | ASID 0).
//
// P4-Ic5-FP: fp_v[512] + fpsr + fpcr — Q0..Q31 (32 × 16 B) + FPSR + FPCR
// for eager save/restore at every context switch. arch/arm64/context.S
// extends with 16 STP-Q pairs after the GP saves + matching LDP pairs
// after the GP loads. Trade-off: every thread adds 528 B even if it
// never touches FP. Phase 5+ may switch to lazy (CPACR_EL1.FPEN trap-
// and-allocate on first FP use) if thread count or per-thread RSS
// becomes a concern.
struct Context {
    u64 x19;
    u64 x20;
    u64 x21;
    u64 x22;
    u64 x23;
    u64 x24;
    u64 x25;
    u64 x26;
    u64 x27;
    u64 x28;
    u64 fp;          // x29 (frame pointer; callee-saved by AAPCS64)
    u64 lr;          // x30 (link / resume PC)
    u64 sp;          // stack pointer
    u64 tpidr_el0;   // EL0 TLS pointer; 0 for kernel threads, set for userspace
    u64 ttbr0;       // P3-Bdb: TTBR0_EL1 value (ASID<<48 | pgtable_root_PA)
    u64 _pad_fp;     // P4-Ic5-FP: pad to 16-byte align fp_v at offset 128
    _Alignas(16) u8 fp_v[512];  // P4-Ic5-FP: V0..V31 (32 × 16 B Q-reg payloads). _Alignas forces 16-byte alignment for STP/LDP Q-reg + forces the containing struct alignment to >= 16.
    u32 fpsr;        // P4-Ic5-FP: FPSR — cumulative FP exception flags
    u32 fpcr;        // P4-Ic5-FP: FPCR — rounding mode + trap enables
};

// Pin context size + field offsets at compile time. arch/arm64/context.S
// hardcodes these; a field reorder without an asm update would silently
// corrupt context switches. Static asserts catch the drift at build.
_Static_assert(sizeof(struct Context) == 656,
               "struct Context must be 656 bytes (15 u64 GP + 8 _pad_fp + "
               "512 fp_v + 4 fpsr + 4 fpcr + 8 trailing pad to keep struct "
               "16-aligned for arrays-of-Context); arch/arm64/context.S "
               "depends on field offsets 0..647");
_Static_assert(_Alignof(struct Context) >= 16,
               "struct Context alignment must be >= 16 (STP/LDP Q-reg "
               "alignment requirement at offset 128)");
_Static_assert(offsetof(struct Context, x19) == 0,    "ctx.x19 offset");
_Static_assert(offsetof(struct Context, x20) == 8,    "ctx.x20 offset");
_Static_assert(offsetof(struct Context, x21) == 16,   "ctx.x21 offset");
_Static_assert(offsetof(struct Context, fp)  == 80,   "ctx.fp offset");
_Static_assert(offsetof(struct Context, lr)  == 88,   "ctx.lr offset");
_Static_assert(offsetof(struct Context, sp)  == 96,   "ctx.sp offset");
_Static_assert(offsetof(struct Context, tpidr_el0) == 104, "ctx.tpidr_el0 offset");
_Static_assert(offsetof(struct Context, ttbr0) == 112, "ctx.ttbr0 offset");
_Static_assert(offsetof(struct Context, fp_v) == 128,
               "ctx.fp_v offset MUST be 16-aligned for STP/LDP Q-reg");
_Static_assert(offsetof(struct Context, fpsr) == 640, "ctx.fpsr offset");
_Static_assert(offsetof(struct Context, fpcr) == 644, "ctx.fpcr offset");

// Save callee-saved + SP + LR + TPIDR_EL0 + TTBR0 + FP state from the
// live CPU into `prev`; load same fields from `next` into the CPU;
// ret to next->lr.
//
// Pure asm in arch/arm64/context.S. From the compiler's view this is a
// normal function call (caller-saved x0-x18 clobbered, void return).
//
// The caller is responsible for thread bookkeeping (current_thread
// pointer + state field updates) — see thread_switch() in kernel/thread.c.
//
// Both pointers must be valid; prev != next is the caller's contract.
// For a fresh thread, ctx.lr MUST point at thread_trampoline (below);
// directly ret-ing into a C entry function lands on its `bti c` with
// BTYPE=00 — passes — but the function's first instruction may also be
// `paciasp` which, while not a BTI check, still expects to be reached
// via call so the corresponding `autiasp` epilogue authenticates against
// a known SP. The trampoline uses `blr` to land in entry, matching the
// PAC-ret discipline.
void cpu_switch_context(struct Context *prev, struct Context *next);

// Trampoline used as the initial ctx.lr for fresh threads created by
// thread_create. cpu_switch_context's `ret` lands here (BTYPE=00); the
// trampoline's first instruction is `bti c` which passes under BTYPE=00.
// It then `blr`s the entry function pointer parked in x21 (set by
// thread_create via ctx.x21). If entry returns, the trampoline halts on
// WFE — Phase 2 close adds the real thread_exit + reap path.
extern void thread_trampoline(void);

// P4-Ic5-FP: enable CPACR_EL1.FPEN = 0b11 (no FP/SIMD trap at any EL)
// on the calling CPU. Called from boot_main (primary) + per_cpu_main
// (secondaries) so all CPUs uniformly admit FP/SIMD at both EL1 and
// EL0. Required before cpu_switch_context's STP/LDP Q-reg accesses can
// run without trapping; required at EL0 so userspace can use the
// AArch64 Advanced SIMD instructions the Rust compiler-builtins emit
// in memset/memcpy intrinsics.
//
// CPACR_EL1.FPEN encoding (ARM ARM D13.2.30):
//   0b00 — trap FP/SIMD at EL0 and EL1.
//   0b01 — trap FP/SIMD at EL0 only.
//   0b10 — reserved.
//   0b11 — no trapping.
//
// header-inline because it's a single MSR + ISB; placing in a TU would
// add a call+ret per CPU bring-up. ISB ordering required so a subsequent
// FP-touching instruction (the very first context switch's STP Qn)
// observes the new FPEN bit. Idempotent — re-calling is harmless.
static inline void fp_enable_this_cpu(void) {
    u64 cpacr;
    __asm__ __volatile__("mrs %0, CPACR_EL1" : "=r"(cpacr));
    // R12-FP audit close (F168 P3): defensive masking of trap fields we
    // don't enable. ZEN[17:16] (SVE), SMEN[25:24] (SME), TTA[28]
    // (trace-access trap) all forced clear so SVE / SME / EL1 trace
    // accesses TRAP at EL0+EL1 — the kernel emits neither (toolchain
    // config), and a permissive firmware/PSCI inheriting a non-zero
    // ZEN would let a stray SVE instruction succeed silently with
    // uninitialized SVE state. Forward-proof at trivial cost.
    cpacr &= ~((3ull << 16) | (3ull << 24) | (1ull << 28));
    cpacr |= (3ull << 20);  // FPEN = 0b11
    __asm__ __volatile__("msr CPACR_EL1, %0\nisb\n" :: "r"(cpacr));
}

#endif // THYLACINE_CONTEXT_H
