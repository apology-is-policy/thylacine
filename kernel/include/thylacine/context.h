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

// 14 u64 = 112 bytes. 8-byte aligned (stp/ldp pair friendly).
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
};

// Pin context size + field offsets at compile time. arch/arm64/context.S
// hardcodes these; a field reorder without an asm update would silently
// corrupt context switches. Static asserts catch the drift at build.
_Static_assert(sizeof(struct Context) == 112,
               "struct Context must be 112 bytes (14 u64); arch/arm64/context.S "
               "depends on the layout");
_Static_assert(_Alignof(struct Context) >= 8,
               "struct Context alignment must allow stp/ldp pair access");
_Static_assert(offsetof(struct Context, x19) == 0,    "ctx.x19 offset");
_Static_assert(offsetof(struct Context, x20) == 8,    "ctx.x20 offset");
_Static_assert(offsetof(struct Context, x21) == 16,   "ctx.x21 offset");
_Static_assert(offsetof(struct Context, fp)  == 80,   "ctx.fp offset");
_Static_assert(offsetof(struct Context, lr)  == 88,   "ctx.lr offset");
_Static_assert(offsetof(struct Context, sp)  == 96,   "ctx.sp offset");
_Static_assert(offsetof(struct Context, tpidr_el0) == 104, "ctx.tpidr_el0 offset");

// Save callee-saved + SP + LR + TPIDR_EL0 from the live CPU into `prev`;
// load same fields from `next` into the CPU; ret to next->lr.
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

#endif // THYLACINE_CONTEXT_H
