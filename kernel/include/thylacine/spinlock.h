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
#include <atomic_lse.h>   // t_atomic_xchg_acq_u32 (W1.5 LSE-patchable test-and-set)
#include <thylacine/extinction.h>  // ASSERT_OR_DIE (the ARCH 8.12 IRQ-state asserts)

typedef struct spin_lock {
    // 0 = unlocked, 1 = locked. The test-and-set goes through the
    // W1.5 patchable atomic (t_atomic_xchg_acq_u32): the kernel compiles
    // to the ARMv8.0 LL/SC form (`ldaxr`/`stxr`), and apply_alternatives
    // rewrites it to single-instruction LSE (`swpa`) at boot on FEAT_LSE
    // cores. Both forms preserve acquire/release semantics.
    u32 value;
} spin_lock_t;

#define SPIN_LOCK_INIT          ((spin_lock_t){ 0 })

// #360 preemption discipline: a plain spin_lock hold makes the HOLDING
// THREAD non-preemptible (the Linux "spin_lock disables preemption" rule).
//
// Why load-bearing: syscalls run IRQ-MASKED end-to-end (SVC masks DAIF;
// no handler unmasks), so a syscall spinning on a contended lock cannot
// be preempted. A holder that runs IRQ-ENABLED (a kproc kthread, or a
// thunk on a fresh thread -- thread_trampoline `daifclr`s) COULD be
// preempted mid-hold before #360: it goes RUNNABLE off-CPU still holding
// the lock, IRQ-masked spinners occupy every CPU waiting for it, and the
// holder never gets a CPU again -- a permanent whole-guest deadlock
// (#359: the parallel `go build` wedge on the shared dev9p client's
// c->lock; the same shape was latent on l->lock, g_dev9p_poll_lock, the
// poll hook-list locks -- any lock shared between a preemptible context
// and syscall paths).
//
// Mechanism: a PER-THREAD hold count (Thread.preempt_count; thread.h has
// the full rationale for per-thread over per-CPU -- the count must travel
// with the thread across a migration, or a mid-increment preempt+migrate
// poisons the old CPU's slot). spin_lock/spin_trylock-success increment
// BEFORE the acquire (so no IRQ can observe "held but count 0");
// spin_unlock decrements AFTER the release store (the count>0 window
// around a transition defers preemption harmlessly -- always the safe
// direction; an IRQ landing MID-increment reads the pre-value and may
// preempt, which is equally safe: the thread holds nothing yet, and the
// half-done RMW targets its own field, completing correctly wherever it
// resumes). preempt_check_irq (sched.c) refuses to sched() while the
// interrupted thread's count is nonzero, LEAVING need_resched pending so
// the deferred preempt fires at the first IRQ-return after the hold drops
// (<= 1 tick). sched() asserts count == 0 at entry: sleeping or yielding
// while holding a plain spinlock is forbidden (lock-across-sleep).
//
// The count is thread-private: mutated only by its own thread (and that
// thread's nested IRQ handlers, which are inc/dec-balanced before IRQ-
// return), read only by the same CPU's IRQ-return gate and by sched() --
// same-PE program order suffices; the fences in the .c bodies stop
// compiler reordering against the lock-word accesses.
//
// Out-of-line in sched.c: the ops need struct Thread (TPIDR_EL1 ->
// current_thread()), and thread.h includes THIS header (wait_lock), so
// only the declarations live here.
void spin_preempt_inc(void);
void spin_preempt_dec(void);

static inline void spin_lock_init(spin_lock_t *l) {
    __atomic_store_n(&l->value, 0u, __ATOMIC_RELAXED);
}

static inline void spin_lock(spin_lock_t *l) {
    // #360: non-preemptible from before the acquire attempt. The spin
    // window is covered too -- benign (holders can no longer be preempted,
    // so every spin is bounded by a running holder's critical section).
    spin_preempt_inc();
    // Test-and-set with acquire. Spin on the relaxed-load fast path
    // when the lock is held (avoids a stream of LL/SC traffic into
    // the contended cacheline). Yield in the inner spin so the
    // host scheduler under emulation gets a hint we're idle-spinning.
    while (t_atomic_xchg_acq_u32(&l->value, 1u) != 0u) {
        while (__atomic_load_n(&l->value, __ATOMIC_RELAXED) != 0u) {
            __asm__ __volatile__("yield" ::: "memory");
        }
    }
}

static inline void spin_unlock(spin_lock_t *l) {
    __atomic_store_n(&l->value, 0u, __ATOMIC_RELEASE);
    // #360: preemptible again only after the release store (order matters;
    // see spin_preempt_dec in sched.c).
    spin_preempt_dec();
}

// P2-Ce: try-lock for cross-CPU access. Returns true if the lock was
// acquired, false if held. Used by work-stealing to attempt access to
// a peer CPU's run tree without blocking -- if try_lock fails, the
// stealer moves to the next peer.
static inline bool spin_trylock(spin_lock_t *l) {
    spin_preempt_inc();
    if (t_atomic_xchg_acq_u32(&l->value, 1u) == 0u)
        return true;
    spin_preempt_dec();
    return false;
}

// #360 RAW (uncounted) variants -- for exactly TWO holders, each of which
// is non-preemptible by MASKING rather than by the count, so the count
// would balance nothing:
//
//   1. sched()'s cs->lock pending-release handoff, the one lock acquired
//      by one thread (prev, in sched()) and released by ANOTHER (the
//      resuming thread, or a fresh thread's trampoline via
//      sched_finish_task_switch). A per-thread count cannot balance a
//      cross-thread pair; sched() runs fully IRQ-masked from its entry
//      mask through cpu_switch_context to the release.
//   2. The extinction winner's claim of the console TX ring lock
//      (cons_tx_claim_for_dump): taken IRQ-masked on a dying CPU and
//      NEVER released -- every path through extinction() ends in
//      _torpor(), so the holder is never scheduled again and there is
//      no hold to account for. Raw on purpose: the counted variants read
//      current_thread() through TPIDR_EL1, which is exactly the kind of
//      state a crash may have destroyed, and a fault inside the claim
//      would turn a diagnosable crash into a recursion banner.
//
// Any OTHER use is a bug -- it silently opts a lock out of the #360
// discipline.
static inline void spin_lock_raw(spin_lock_t *l) {
    while (t_atomic_xchg_acq_u32(&l->value, 1u) != 0u) {
        while (__atomic_load_n(&l->value, __ATOMIC_RELAXED) != 0u) {
            __asm__ __volatile__("yield" ::: "memory");
        }
    }
}

static inline void spin_unlock_raw(spin_lock_t *l) {
    __atomic_store_n(&l->value, 0u, __ATOMIC_RELEASE);
}

// One raw try (holder 2 above). Interoperates with the counted variants:
// the lock word protocol is identical, only the hold accounting differs.
static inline bool spin_trylock_raw(spin_lock_t *l) {
    return t_atomic_xchg_acq_u32(&l->value, 1u) == 0u;
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
// ARCH 8.12: ASSERTIONS ON THE INTERRUPT STATE.
//
// Until these landed there were NONE, anywhere in the tree. Searched
// irq_disabled / irqs_disabled / in_irq / ASSERT.*daif: zero hits across
// kernel/, arch/, mm/ and lib/. Every one of those files depends on a masking
// discipline that was load-bearing ONLY IN PROSE -- so when the premise
// changed, nothing would fail loudly; it would just be quietly wrong in a
// place nobody was looking.
//
// The ARCH 8.1 chunk changes exactly that premise (syscall bodies run with
// interrupts ON), which is why the assert lands with it rather than after.
// Read the messages as documentation of which model the code is in: when 8.1
// lands, the assertions at the syscall entry INVERT, and the inversion is
// visible in the diff.
//
// Cost is one `mrs` and a branch -- cheap enough to leave in every build, and
// an assert compiled out of the build that ships is an assert that never
// catches the thing it was written for.
#define DAIF_I_BIT  (1ULL << 7)         // PSTATE.I -- the IRQ mask

static inline bool irqs_masked(void) {
    u64 daif;
    __asm__ __volatile__("mrs %x0, daif" : "=r" (daif) :: "memory");
    return (daif & DAIF_I_BIT) != 0;
}

#define ASSERT_IRQS_MASKED(why) \
    ASSERT_OR_DIE(irqs_masked(), "IRQs must be MASKED here: " why)
#define ASSERT_IRQS_ENABLED(why) \
    ASSERT_OR_DIE(!irqs_masked(), "IRQs must be ENABLED here: " why)

// ARCH 8.12: the syscall body's unmask / re-mask. A DIRECT PSTATE.DAIF write
// takes effect with no barrier -- Linux's __daif_local_irq_enable is a bare
// `msr daifclr, #3` and carries none -- so there is no `isb` here and none is
// owed. (Only the ICC_PMR_EL1 path needs pmr_sync(); this is not that path.
// The B-0 preemption point once claimed an isb was load-bearing for DELIVERY;
// a sabotage that removed it PASSED, and the claim was corrected rather than
// the test.)
//
// These are deliberately NOT save/restore: the syscall body's entry state is
// known by construction (the SVC vector masked), so a saved value would be a
// variable standing in for a constant, and the re-mask must be unconditional
// -- an eret out of KERNEL_EXIT under an inherited unmask is #713.
static inline void irq_unmask_local(void) {
    __asm__ __volatile__("msr daifclr, #2" ::: "memory");
}

static inline void irq_mask_local(void) {
    __asm__ __volatile__("msr daifset, #2" ::: "memory");
}

typedef u64 irq_state_t;

static inline irq_state_t spin_lock_irqsave(spin_lock_t *l) {
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
    // P2-Ce: real lock acquire after IRQ disable. Order matters —
    // disabling IRQs first prevents an IRQ handler from being
    // interrupted while it holds the lock. NULL lock pointer is
    // legal (per-CPU lockless sections — sched.c's earlier discipline
    // used it as "IRQ mask only").
    if (l) spin_lock(l);
    return state;
}

static inline void spin_unlock_irqrestore(spin_lock_t *l, irq_state_t s) {
    if (l) spin_unlock(l);
    __asm__ __volatile__(
        "msr daif, %x0\n"
        :: "r" (s) : "memory"
    );
}

#endif // THYLACINE_SPINLOCK_H
