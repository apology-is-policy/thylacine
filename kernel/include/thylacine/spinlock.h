// Spin lock — single-CPU at v1.0; real LL/SC + per-CPU contention
// arrives at Phase 2 SMP secondary bring-up. The lock_/unlock_ pair
// is a no-op on UP. The IRQ-disabling variants (spin_lock_irqsave /
// spin_unlock_irqrestore) DO real work: they save and restore the
// PSTATE.DAIF mask so a critical section over shared mutable state
// (allocator free lists, magazine refill/drain) can't be preempted
// by an IRQ handler that re-enters the same code path.
//
// Why this matters at P1-I (audit r3 F30): P1-G turned IRQ delivery
// LIVE (msr daifclr, #2 in boot_main). At v1.0 only the timer IRQ
// fires and its handler doesn't touch the allocator — but the moment
// any future IRQ source (Phase 2 scheduler-tick wakeup, Phase 3
// device IRQ deferred work) calls into mm code, the
// spin_lock_irqsave call on the path is the contract that makes the
// allocator critical sections safe against re-entry. Land it now,
// once, before that caller appears.
//
// Per ARCHITECTURE.md §24.4 (LSE atomics) and §28 (invariants I-9 /
// I-17 / I-18 — load-bearing for SMP correctness later).

#ifndef THYLACINE_SPINLOCK_H
#define THYLACINE_SPINLOCK_H

#include <thylacine/types.h>

typedef struct spin_lock {
    u32 _stub;                  // unused at P1-I; real lock state at Phase 2
} spin_lock_t;

#define SPIN_LOCK_INIT          ((spin_lock_t){ 0 })

static inline void spin_lock_init(spin_lock_t *l) {
    l->_stub = 0;
}

static inline void spin_lock(spin_lock_t *l) {
    (void)l;                    // single CPU: no contention possible
}

static inline void spin_unlock(spin_lock_t *l) {
    (void)l;
}

// IRQ-disabling variants — REAL implementation (P1-I audit F30 close).
//
// `daif` PSTATE bits encode the four mask flags: D (debug), A
// (SError), I (IRQ), F (FIQ). We save the current value and set
// DAIF.I (IRQ mask) for the duration of the critical section,
// then restore from the saved state on unlock.
//
// `mrs DAIF` reads the current PSTATE.DAIF as bits 9..6 of the
// returned register; `msr daifset, #N` ORs the mask bits into
// PSTATE; `msr DAIF, x` writes the full DAIF value back. The
// `daifset, #2` immediate-form sets PSTATE.I (bit 7).
//
// On UP at v1.0 the spin part of spin_lock_irqsave is still a
// no-op — but the IRQ mask discipline is real. Phase 2's SMP
// adds the LL/SC contention.
typedef u64 irq_state_t;

static inline irq_state_t spin_lock_irqsave(spin_lock_t *l) {
    (void)l;
    irq_state_t state;
    // %x0 forces the 64-bit register form (xN); plain %0 may default
    // to wN on the output constraint and emit a sub-word read of a
    // 64-bit system register.
    __asm__ __volatile__(
        "mrs %x0, daif\n"
        "msr daifset, #2\n"     // mask IRQs (PSTATE.I = 1)
        : "=r" (state)
        :: "memory"
    );
    return state;
}

static inline void spin_unlock_irqrestore(spin_lock_t *l, irq_state_t s) {
    (void)l;
    __asm__ __volatile__(
        "msr daif, %x0\n"
        :: "r" (s) : "memory"
    );
}

#endif // THYLACINE_SPINLOCK_H
