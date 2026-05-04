// Stack canary infrastructure (P1-H).
//
// `-fstack-protector-strong` instructs clang to emit a prologue that
// loads __stack_chk_guard and stores it just below the saved frame
// pointer for any function that has an address-taken local or a stack
// array. The matching epilogue re-reads __stack_chk_guard, compares it
// against the stack-stored value, and calls __stack_chk_fail on
// mismatch. The kernel provides both symbols.
//
// Initialization sequence (load-bearing — see kernel/canary.c comment):
//   1. __stack_chk_guard is initialized at link time to a non-zero
//      compile-time pattern. This is the value used by every function
//      whose canary check fires before canary_init runs.
//   2. canary_init() is called from kaslr_init's prologue, mixing the
//      KASLR seed (DTB / cntpct entropy) into a runtime cookie.
//      kaslr_init is marked __attribute__((no_stack_protector)) so it
//      doesn't check a half-initialized cookie against itself.
//   3. After canary_init returns, __stack_chk_guard holds the runtime
//      cookie. All subsequent function returns check against it.
//
// Reads / writes to __stack_chk_guard outside this protocol can only
// be made by code that is itself not stack-canary-protected.
//
// Per ARCHITECTURE.md §24.3.

#ifndef THYLACINE_KERNEL_CANARY_H
#define THYLACINE_KERNEL_CANARY_H

#include <thylacine/types.h>

// Initialize the global stack-canary cookie from a seed value. Called
// from kaslr_init (which is marked no_stack_protector) before any
// other canary-protected function returns. Idempotent only in the
// trivial sense — calling twice with the same seed is fine; calling
// with different seeds across the lifetime of a stack frame would
// trip a canary mismatch on that frame's epilogue.
//
// The seed comes from kaslr.c's entropy chain (DTB kaslr-seed →
// rng-seed → cntpct fallback). If the seed is 0, canary_init mixes
// in cntpct_el0 directly so the cookie is never zero — a zero cookie
// would silently disable the protection for any function whose
// stack-stored canary happened to be zeroed.
void canary_init(u64 seed);

// Diagnostic accessor for the test harness. Returns the current
// __stack_chk_guard value. Used by hardening.detect_smoke to verify
// canary_init produced a non-zero cookie.
u64 canary_get_cookie(void);

#endif // THYLACINE_KERNEL_CANARY_H
