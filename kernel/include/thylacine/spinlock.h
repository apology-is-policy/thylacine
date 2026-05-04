// Spin lock — STUB AT P1-D. Single CPU, no IRQs yet, so locking is
// trivially correct. The interface is fixed now so subsequent work
// (real LL/SC at P1-F when SMP arrives, LSE atomics at P1-H where
// the hardware supports them) doesn't ripple through every caller.
//
// Per ARCHITECTURE.md §24.4 (LSE atomics) and §28 (invariants I-9 /
// I-17 / I-18 — load-bearing for SMP correctness later).

#ifndef THYLACINE_SPINLOCK_H
#define THYLACINE_SPINLOCK_H

#include <thylacine/types.h>

typedef struct spin_lock {
    u32 _stub;                  // unused at P1-D; real lock state at P1-F
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

// IRQ-disabling variants. P1-F installs exception vectors and the
// IRQ enable/mask machinery; until then, IRQs are masked from boot
// (DAIF=1 in SPSR_EL2 across the EL2 drop, plus EL1 entry from
// firmware always has them masked). The state token returned here is
// a placeholder for whatever DAIF-save state we need at P1-F.
typedef u64 irq_state_t;

static inline irq_state_t spin_lock_irqsave(spin_lock_t *l) {
    (void)l;
    return 0;
}

static inline void spin_unlock_irqrestore(spin_lock_t *l, irq_state_t s) {
    (void)l;
    (void)s;
}

#endif // THYLACINE_SPINLOCK_H
