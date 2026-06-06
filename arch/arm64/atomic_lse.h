// Patchable read-modify-write atomics (Lazarus W1.5).
//
// Each primitive is authored as an LL/SC sequence (the ARMv8.0 baseline,
// correct on every core) carrying a single-instruction LSE replacement in
// an .altinstructions entry. apply_alternatives() rewrites the site to the
// LSE form at boot on FEAT_LSE cores; on an A72 the LL/SC stays. The hot
// RMW sites the design routes through these -- the spinlock (xchg), the
// Spoor/SrvConn refcounts (fetch_add/sub), and the scheduler steal-rotate
// counter (fetch_add) -- get native LSE on capable silicon with zero
// steady-state branch, exactly as if compiled for that CPU.
//
// Cold or non-hot-category RMW sites (the legate cap fetch_or, the
// group-exit compare_exchange, identity counters) deliberately stay on the
// compiler's __atomic_* builtins (which also inline LL/SC under the v8.0
// floor): a single-instruction LSE win there is immaterial and the
// dual-form asm is disproportionate. The framework is general -- a hot
// fetch_or/cas site can be added later.
//
// PORTABILITY.md section 4.5. memory-order suffixes map LL/SC exclusive
// load/store mnemonics + the matching LSE op order suffix:
//   relaxed: ldxr  / stxr   + ldadd  / swp
//   acqrel : ldaxr / stlxr  + ldaddal/ swpal     (acquire: ldaxr/stxr + *a)
// The LL/SC and LSE forms are per-op equivalent (same operand register,
// same width, same acquire/release semantics) -- so an unpatched site is
// correct and a patcher bug fails safe (slower, never wrong).

#ifndef THYLACINE_ARM64_ATOMIC_LSE_H
#define THYLACINE_ARM64_ATOMIC_LSE_H

#include <thylacine/types.h>
#include "alternatives.h"

// fetch_<op>: atomically *ptr = *ptr <op> val; return the PRE value.
//   fn  : function name             ty  : operand C type
//   rw  : "w" (32-bit) | "x" (64-bit) register-width specifier
//   ld/st: order-carrying exclusive load/store (ldxr/stxr | ldaxr/stlxr)
//   alu : the LL/SC ALU mnemonic (add, ...)
//   lse : the matching single-instruction LSE op (ldadd, ldaddal, ...)
// `.arch_extension lse` lets the LSE op assemble under -march=armv8-a; its
// EXECUTION is gated by apply_alternatives only patching FEAT_LSE cores
// (the same assemble-gate / execute-gate split W1 used for `xpaci`).
// Early-clobber (=&r) on the outputs keeps them off the ptr/val input
// registers, which must survive the LL/SC retry loop.
#define T_ATOMIC_FETCH_OP(fn, ty, rw, ld, st, alu, lse)                    \
    static inline ty fn(ty *ptr, ty val) {                                 \
        ty oldv, newv;                                                     \
        u32 st_fail;                                                       \
        __asm__ __volatile__(                                              \
            ALTERNATIVE(                                                    \
                "1: " ld  " %" rw "0, [%3]\n"                              \
                "   " alu  " %" rw "1, %" rw "0, %" rw "4\n"               \
                "   " st  " %w2, %" rw "1, [%3]\n"                         \
                "   cbnz    %w2, 1b",                                       \
                lse " %" rw "4, %" rw "0, [%3]",                           \
                ALT_FEAT_LSE)                                               \
            : "=&r"(oldv), "=&r"(newv), "=&r"(st_fail)                     \
            : "r"(ptr), "r"(val)                                           \
            : "memory");                                                   \
        (void)newv;                                                        \
        return oldv;                                                       \
    }

// exchange: atomically *ptr = val; return the PRE value. (swp has no ALU
// operand and no intermediate value, so it is its own macro.)
#define T_ATOMIC_XCHG(fn, ty, rw, ld, st, lse)                             \
    static inline ty fn(ty *ptr, ty val) {                                 \
        ty oldv;                                                           \
        u32 st_fail;                                                       \
        __asm__ __volatile__(                                              \
            ALTERNATIVE(                                                    \
                "1: " ld " %" rw "0, [%2]\n"                               \
                "   " st " %w1, %" rw "3, [%2]\n"                          \
                "   cbnz   %w1, 1b",                                        \
                lse " %" rw "3, %" rw "0, [%2]",                           \
                ALT_FEAT_LSE)                                               \
            : "=&r"(oldv), "=&r"(st_fail)                                  \
            : "r"(ptr), "r"(val)                                           \
            : "memory");                                                   \
        return oldv;                                                       \
    }

// ---- Instances: exactly the (op, type, order) tuples the routed sites use.
// Unused static inlines emit no code (and no .altinstructions entry), so
// the set can be widened freely.

// exchange-acquire u32 -- the spinlock test-and-set (spin_lock / spin_trylock).
T_ATOMIC_XCHG(t_atomic_xchg_acq_u32, u32, "w", "ldaxr", "stxr", "swpa")

// fetch_add -- scheduler steal-rotate (u32 relaxed) + the int refcounts.
T_ATOMIC_FETCH_OP(t_atomic_fetch_add_relaxed_u32, u32, "w", "ldxr",  "stxr",  "add", "ldadd")
T_ATOMIC_FETCH_OP(t_atomic_fetch_add_relaxed_int, int, "w", "ldxr",  "stxr",  "add", "ldadd")
T_ATOMIC_FETCH_OP(t_atomic_fetch_add_acqrel_int,  int, "w", "ldaxr", "stlxr", "add", "ldaddal")

// fetch_sub == fetch_add of the two's-complement negation: identical PRE
// value and identical stored result under wraparound, so no separate LSE
// op (there is no LDSUB; Linux likewise negates). The negation is computed
// in C; the atomic itself is the add primitive above. Precondition:
// val != INT_MIN (the routed refcount sites pass 1) -- `-INT_MIN` is C UB,
// unreachable here.
static inline int t_atomic_fetch_sub_relaxed_int(int *ptr, int val) {
    return t_atomic_fetch_add_relaxed_int(ptr, -val);
}
static inline int t_atomic_fetch_sub_acqrel_int(int *ptr, int val) {
    return t_atomic_fetch_add_acqrel_int(ptr, -val);
}

#endif // THYLACINE_ARM64_ATOMIC_LSE_H
