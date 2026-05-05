// Rendez — Plan 9 single-waiter rendezvous (P2-Bb).
//
// Per ARCH §8.5 (wakeup atomicity, invariant I-9) and §8.8 (Plan 9
// idiom layer). A Rendez pairs one waiter with one waker over a
// caller-supplied condition; sleep(r, cond, arg) blocks until cond
// returns true, wakeup(r) wakes the waiting thread.
//
// Atomicity comes from r->lock: cond is checked under the lock, and
// any sleep transition happens before the lock is released. wakeup
// holds the lock while clearing the waiter and transitioning the
// thread back to RUNNABLE. The discipline maps directly to the
// scheduler.tla actions:
//
//   sleep   ↔ WaitOnCond   (atomic cond-check + sleep + enqueue).
//   wakeup  ↔ WakeAll      (atomic clear + RUNNABLE transition).
//
// scheduler.tla NoMissedWakeup proves that under this protocol, a
// wakeup cannot be lost between the cond check and the sleep
// transition. The buggy variant (BuggyCheck + BuggySleep splitting
// the protocol) produces a counterexample at depth 4.
//
// Single-waiter: at most one thread may sleep on a Rendez at a time.
// Multi-waiter wait queues are a Phase 5 addition (poll, futex). The
// single-waiter restriction is structurally a special case of the
// scheduler.tla multi-waiter spec — invariants carry over (a
// singleton-or-empty waiters set still satisfies "cond=TRUE ⇒
// waiters={}" under the same protocol).

#ifndef THYLACINE_RENDEZ_H
#define THYLACINE_RENDEZ_H

#include <thylacine/spinlock.h>

struct Thread;

struct Rendez {
    spin_lock_t    lock;
    struct Thread *waiter;     // NULL or the single sleeper
};

// Static initializer for file-scope or struct-embedded Rendez.
#define RENDEZ_INIT  ((struct Rendez){ SPIN_LOCK_INIT, NULL })

// Initialize a Rendez at runtime (e.g., for kmem-allocated objects
// that contain one).
static inline void rendez_init(struct Rendez *r) {
    spin_lock_init(&r->lock);
    r->waiter = NULL;
}

// Sleep until cond(arg) returns nonzero. Atomic with respect to a
// concurrent wakeup(r): if cond is already true at sleep entry,
// returns immediately without state transition (fast path); otherwise
// transitions current thread to SLEEPING under r->lock, drops the
// lock, calls sched() to switch out, and on resume re-checks cond.
//
// Preconditions:
//   - r is initialized (rendez_init / RENDEZ_INIT).
//   - cond is a side-effect-free predicate that may be evaluated
//     multiple times. cond is called under r->lock; cond's reads of
//     the producer's state must therefore be paired with a producer
//     write that also holds r->lock (or is followed by wakeup(r)
//     which takes r->lock).
//   - Caller is NOT in IRQ context; sleep() may yield indefinitely.
//   - At most one thread may sleep on a given Rendez at a time
//     (single-waiter convention). Extincts on second sleeper.
void sleep(struct Rendez *r, int (*cond)(void *arg), void *arg);

// Wake the (at most one) thread sleeping on r. If no thread is
// sleeping, wakeup is a no-op. Returns 1 if a waiter was woken,
// 0 otherwise.
//
// The caller is expected to have made cond's predicate true BEFORE
// calling wakeup — sleep's resume re-checks cond under r->lock and
// loops back if it's still false. The scheduler.tla WakeAll model
// is "set cond := TRUE atomically with waking the waiters"; the
// impl achieves the same atomicity by holding r->lock during both
// the producer's state mutation (caller-side) and the wakeup's
// waiter-clear (here). Concretely: producer must take r->lock, set
// the condition, wakeup(r) (which re-takes r->lock — re-entrancy not
// supported; the lock is dropped between).
//
// May be called from IRQ context (the lock is irqsave).
int wakeup(struct Rendez *r);

#endif // THYLACINE_RENDEZ_H
