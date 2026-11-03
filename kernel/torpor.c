// P6-pouch-wait-addr: Thylacine's wait-on-address primitive (sub-chunk 8).
//
// Implements `sys_torpor_wait_for_proc` and `sys_torpor_wake_for_proc` —
// the kernel-side primitive over which pouch's pthread mutex/condvar
// implementation runs (sub-chunk 9 `pouch-threads`). Public surface +
// design rationale: `<thylacine/torpor.h>`.
//
// Mechanism:
//
//   - A table of buckets, each with its own lock, keyed by
//     `hash(AddrSpace *, user_va)`. Each bucket is a list of
//     `struct torpor_waiter` records, oldest first.
//   - Each waiter has its own `struct Rendez` for the actual sleep
//     transition. `tsleep` provides the timed/cond-checked sleep
//     primitive; the `awoken` flag is the cond.
//   - WAIT: take the bucket's lock, `uaccess_load_u32` the futex word,
//     if `!= expected` unlock + return 0; else append the stack-allocated
//     waiter to its bucket, drop the lock, `tsleep` on the waiter's
//     private rendez; on resume re-take the lock, unlink if still linked,
//     unlock, wait for any waker still holding the record, return.
//   - WAKE: under the bucket's lock, walk from the oldest. For each
//     waiter whose `(as, user_va)` matches: set `awoken` and unlink it.
//     One found asleep is counted and joins a batch. Drop the lock, then
//     `wakeup` the batch.
//
// Lock order: bucket lock -> `waiter->rendez.lock`. The consumer drops the
// bucket lock before entering `tsleep`, so it never holds both. Bucket locks
// never nest.
//
// specs/torpor.tla models the wake and the record's lifetime.

#include <thylacine/torpor.h>

#include <thylacine/extinction.h>
#include <thylacine/notes.h>    // LS-5c: thread_die_pending (the widened #811 predicate)
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/spinlock.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../arch/arm64/timer.h"
#include "../arch/arm64/uaccess.h"

// Per-call waiter record. Lives on the consumer's kernel stack for the
// duration of the WAIT call; every exit path unlinks it from the
// bucket before the stack frame is reaped (lifetime invariant — see
// the "lifetime" note in WAIT below).
struct torpor_waiter {
    struct Rendez         rendez;       // private rendez for the sleep
    struct Proc          *proc;         // whose death and stop walks reach it
    struct AddrSpace     *as;           // key component 1
    u64                   user_va;      // address being waited on (k2)
    struct torpor_waiter *next;         // bucket chain, or a waker's batch
    struct torpor_waiter *prev;         // bucket chain
    u32                   linked;       // on its bucket's chain; under its lock
    u32                   awoken;       // 0 sleeping, 1 woken (tsleep cond)
    u32                   wake_held;    // a waker's batch still holds this record
    u32                   _pad;         // explicit alignment padding
};

// F6 (P3): pin struct layout against silent drift. `_pad` is explicit
// because a future field reorder must keep 8-byte total alignment; the
// _Static_assert traps any breakage at build time. Not an ABI type
// (waiter is kernel-stack-only; no cross-boundary visibility), but the
// padding's purpose is structural — best pinned, not implicit.
_Static_assert(sizeof(struct torpor_waiter) % 8 == 0,
               "struct torpor_waiter must be 8-byte aligned");

struct torpor_bucket {
    spin_lock_t           lock;
    struct torpor_waiter *head;         // oldest
    struct torpor_waiter *tail;         // newest
} __attribute__((aligned(64)));

static struct torpor_bucket torpor_buckets[TORPOR_HASH_BUCKETS];

// The address space's pointer has its low bits fixed by the allocator, and a
// futex word's are fixed by its alignment. A multiplicative mix moves what
// varies into the high half, where the mask reads.
static inline struct torpor_bucket *torpor_bucket_of(const struct AddrSpace *as, u64 user_va) {
    u64 mix = ((u64)(uintptr_t)as ^ (user_va >> 2)) * 0x9E3779B97F4A7C15ull;
    return &torpor_buckets[(u32)(mix >> 32) & (TORPOR_HASH_BUCKETS - 1u)];
}

// tsleep condition: woken iff `waiter->awoken != 0`. Called by tsleep
// under `waiter->rendez.lock`, and every waker sets it under that lock, so
// the read is exact: a waiter that resumes reads what a wake's mark wrote.
static int torpor_cond_awoken(void *arg) {
    const struct torpor_waiter *w = (const struct torpor_waiter *)arg;
    return (int)w->awoken;
}

static void torpor_append_locked(struct torpor_bucket *b, struct torpor_waiter *w) {
    w->next = NULL;
    w->prev = b->tail;
    if (b->tail) b->tail->next = w; else b->head = w;
    b->tail = w;
    w->linked = 1;
}

// A waiter a wake has already taken off the chain is a quiet no-op.
static void torpor_unlink_locked(struct torpor_bucket *b, struct torpor_waiter *w) {
    if (!w->linked) return;
    if (w->prev) w->prev->next = w->next; else b->head = w->next;
    if (w->next) w->next->prev = w->prev; else b->tail = w->prev;
    w->next = NULL;
    w->prev = NULL;
    w->linked = 0;
}

// Set the cond under the rendez's lock, and answer whether the waiter is asleep on
// it. One that is asleep will read the mark when it resumes: its deadline and a caught
// note both ask the cond first, and only death goes before it. So it has taken this
// wake, whichever of them gets to it first. One that is not asleep may be past its
// wait, on its way out with a timeout or an EINTR, and has taken nothing. The lock's
// release is also what delivers the mark to a waiter a stop has parked, on its
// re-read (PTY-4e R2): this replaces the wakeup() that used to carry it.
static bool torpor_mark_awoken(struct torpor_waiter *w) {
    irq_state_t s = spin_lock_irqsave(&w->rendez.lock);
    bool asleep = w->rendez.waiter != NULL;
    w->awoken = 1;
    spin_unlock_irqrestore(&w->rendez.lock, s);
    return asleep;
}

// Validate the addr_va arg shared by WAIT + WAKE: must be 4-byte
// aligned, non-zero, and under UACCESS_USER_VA_TOP. Returns 1 on OK,
// 0 on reject. Centralised so the two syscalls agree on the rules.
//
// F10 (P3): the 4-byte alignment check is load-bearing for
// `uaccess_load_u32` — the asm primitive's fault-fixup table catches
// translation/permission faults only (`arch/arm64/exception.c`); an
// unaligned LDR on SCTLR_EL1.A-checking memory would alignment-fault
// to the "unhandled kernel alignment fault" extinction, not the fixup
// recovery. Rejecting unaligned addr_va here keeps the fault-class
// invariant intact at the C boundary.
static int torpor_addr_va_ok(u64 addr_va) {
    if (addr_va == 0)                          return 0;
    if (addr_va & 0x3ull)                      return 0;      // 4-byte align
    if (addr_va >= UACCESS_USER_VA_TOP)        return 0;
    // 4-byte word must not straddle the user-VA top.
    if (addr_va > UACCESS_USER_VA_TOP - 4ull)  return 0;
    return 1;
}

#ifdef KERNEL_TESTS
// Test hook: run once in the next wait, between its first load and its lock, which
// is the window a peer's MAP_FIXED lands in.
static void (*g_torpor_after_prefault)(void *);
static void *g_torpor_after_prefault_arg;
void torpor_after_prefault_for_test(void (*fn)(void *), void *arg) {
    g_torpor_after_prefault_arg = arg;
    __atomic_store_n(&g_torpor_after_prefault, fn, __ATOMIC_RELEASE);
}
static void torpor_after_prefault_point(void) {
    void (*fn)(void *) = __atomic_exchange_n(&g_torpor_after_prefault, NULL, __ATOMIC_ACQ_REL);
    if (fn) fn(g_torpor_after_prefault_arg);
}

// Times a waiter has found a wake still holding its record: the positive half of a
// test that a waiter does not leave, which a waiter that never ran would also pass.
static volatile u64 g_torpor_held_spins;
u64 torpor_held_spins_for_test(void) {
    return __atomic_load_n(&g_torpor_held_spins, __ATOMIC_ACQUIRE);
}
#endif

// F4 (P3): the passed `p` MUST be `current_thread()->proc`. The user-VA
// load implicitly walks the CURRENT thread's TTBR0 (the running thread's
// `proc->pgtable_root`); the fault dispatcher recovers via
// `current_thread()->proc`, not via the passed-in `p`. If `p` were a
// foreign Proc, the load would walk current's namespace — at best
// faulting (-EFAULT, self-healing failure mode), at worst reading the
// WRONG word from a coincidentally-mapped VA in current's namespace
// (silent correctness bug). The SVC wrapper always passes
// `current_thread()->proc`; the `_for_proc` shape is reachable from the
// kernel test harness only. We enforce it via an `extinction` assert so
// a regression at either site is loud.
s64 sys_torpor_wait_for_proc(struct Proc *p, u64 addr_va, u32 expected,
                             s64 timeout_us) {
    if (!p)                                    return TORPOR_ERR_EINVAL;
    {
        struct Thread *t = current_thread();
        if (t && t->proc != p)
            extinction("torpor_wait: p must equal current_thread()->proc "
                       "(uaccess_load_u32 walks the current thread's TTBR0 "
                       "— mismatched p would silently read the wrong word)");
    }
    if (!torpor_addr_va_ok(addr_va))           return TORPOR_ERR_EINVAL;
    // `timeout_us` semantics:
    //   - < 0   : block indefinitely (no deadline).
    //   - == 0  : probe-style: register, check, no real sleep — if the
    //             value still matches at entry, return ETIMEDOUT
    //             immediately (the deadline lapsed before any wake).
    //   - > 0   : block at most `timeout_us` microseconds.
    //   - > TORPOR_MAX_TIMEOUT_US: reject as EINVAL.
    if (timeout_us > 0 && (u64)timeout_us > TORPOR_MAX_TIMEOUT_US)
        return TORPOR_ERR_EINVAL;

    // Stack-allocated per-call waiter. Initialise rendez + awoken
    // BEFORE linking into the bucket (registers are still in the
    // current_thread()'s frame — no other CPU can see the waiter
    // until we publish it under the bucket's lock). `p` is the caller's own,
    // and every thread of a Proc is gone before an exec swaps its space, so
    // the key stays this record's for its whole life.
    struct torpor_waiter w;
    rendez_init(&w.rendez);
    w.proc      = p;
    w.as        = p->as;
    w.user_va   = addr_va;
    w.next      = NULL;
    w.prev      = NULL;
    w.linked    = 0;
    w.awoken    = 0;
    w.wake_held = 0;
    w._pad      = 0;
    struct torpor_bucket *b = torpor_bucket_of(w.as, addr_va);

    // REVENANT R-5 audit F1: fault the futex word in BEFORE taking the bucket's lock,
    // because a file page's demand-page does a blocking dev->read and a held lock may
    // not sleep. This makes the page resident, so the load under the lock normally
    // takes no fault.
    //
    // It can still miss: a peer's munmap, or a MAP_FIXED that puts an unfaulted file
    // page here, can land between the two loads, and EL0 can hold the window open by
    // keeping this bucket busy. A kernel access holding a counted lock takes no page-in
    // (demand_page_locked), so that load fails rather than sleeps, the shape of Linux's
    // pagefault_disable() around get_futex_value_locked. See its arm below.
    {
        u32 prefault = 0;
        if (uaccess_load_u32(addr_va, &prefault) != 0)
            return TORPOR_ERR_EFAULT;

        // Lock-free pre-lock compare (the Linux futex
        // get_futex_value_locked / compare-before-queue shape). If the word
        // already differs from `expected`, NO waiter is registered, so there is
        // no register-vs-WAKE race to close: I-9's missed-wakeup window exists
        // ONLY between a waiter's bucket-register and its sleep, which only the
        // EQUAL path below ever reaches. A stale mismatch (the word equalled
        // `expected` and changed to != between this load and now) returning
        // TORPOR_OK is a benign spurious return the futex contract permits --
        // the caller re-evaluates its own predicate. Returning here WITHOUT
        // touching a bucket's lock removes that acquire from every
        // osyield (torpor_wait(&sleepDummy, 1, 1)), the #343 go-build floor:
        // 36.8M of the 67.7M torpor_wait calls are this mismatch path, all on
        // the single sleepDummy address (one bucket, which a lock per bucket
        // cannot shard, so the lock-free skip is the fix).
        if (prefault != expected)
            return TORPOR_OK;
    }

#ifdef KERNEL_TESTS
    torpor_after_prefault_point();
#endif

    spin_lock(&b->lock);

    // Load the user-VA word and compare under the bucket's lock. The lock
    // serialises with WAKE's same-lock walk: either WAKE's unlock has
    // happened-before this lock (and we see the producer's pre-WAKE
    // store via the lock release/acquire chain — load returns the new
    // value, we return 0 without sleeping), or this lock happens-
    // before WAKE's lock (and we register before WAKE walks the
    // bucket — WAKE delivers the wakeup).
    //
    // A load that fails here is a spurious return, the same as a word that changed.
    // The address was good a moment ago, and it may be good now with a page this load
    // could not wait for. The caller re-evaluates, and its next call faults the word in
    // above, or answers EFAULT there if the address really has gone.
    u32 observed = 0;
    if (uaccess_load_u32(addr_va, &observed) != 0) {
        spin_unlock(&b->lock);
        return TORPOR_OK;
    }
    if (observed != expected) {
        // Rare post-lock-free-compare: the word changed from `expected` to !=
        // between the prefault compare and this authoritative under-lock
        // reload. Still correct -- no waiter is registered, caller
        // re-evaluates. (Pre-#343 this was the common osyield path; the
        // lock-free compare above now catches nearly all of them.)
        spin_unlock(&b->lock);
        return TORPOR_OK;
    }

    // Register: the newest waiter goes last, and a wake takes the oldest
    // first (Linux's futex order within a priority). Under this lock, so WAKE's
    // walk sees it.
    torpor_append_locked(b, &w);

    // SYS_EXIT_GROUP / kill cross-thread shootdown lost-wakeup close (I-24).
    // Re-check the death predicate AFTER registering, UNDER the bucket's lock --
    // a lock proc_group_terminate's torpor_wake_all walk takes on its way through
    // every bucket, this one included. This
    // makes the wake airtight against the register-vs-walk race:
    //   - if we registered BEFORE the wake-all walk, the walk finds us +
    //     wakes us (awoken=1) -> tsleep returns immediately;
    //   - if we register AFTER the walk (it missed us), then the flag-set
    //     happened-before the walk, the walk released this lock, and we
    //     acquired it to register -- so this acquire-load observes the set
    //     flag here and we do NOT sleep.
    // Either way a group-terminating Proc's futex sleeper does not sleep
    // through the exit; it returns + dies at its EL0-return die-check.
    //
    // LS-5c (ARCH 8.8.2): widened to thread_die_pending (group-exit death OR
    // a pending terminate-disposition `interrupt`). For the interrupt leg
    // this early-exit is conservative-prompt, not the I-9 closure -- the
    // interrupt waker (proc_interrupt_terminate_wake) takes no bucket's
    // lock; its register-after-walk race is closed one layer down by
    // tsleep's own register-then-observe under wait_lock (the torpor waiter's
    // stack rendez is reachable via rendez_blocked_on for the wake itself).
    if (thread_die_pending(current_thread())) {
        torpor_unlink_locked(b, &w);
        spin_unlock(&b->lock);
        return TORPOR_OK;
    }

    // Compute the tsleep deadline on the timer_now_ns timebase. < 0
    // timeout maps to `deadline_ns = 0` (the "no deadline" sentinel
    // tsleep treats as `sleep`).
    u64 deadline_ns = 0;
    if (timeout_us == 0) {
        // Probe path: deadline is "now" (already past); tsleep will
        // observe TSLEEP_TIMEDOUT on entry unless awoken is already
        // set (a same-CPU producer could not, since we hold the lock;
        // a cross-CPU producer that was already mid-walk would have
        // blocked on this lock above and runs after we drop it).
        // We must drop the lock before tsleep — see below.
        u64 now = timer_now_ns();
        deadline_ns = (now == 0) ? 1 : now;     // 0 reads as "no deadline"
    } else if (timeout_us > 0) {
        u64 now = timer_now_ns();
        u64 add = (u64)timeout_us * 1000ull;     // µs → ns
        u64 dl  = now + add;
        if (dl < now) dl = (u64)-1ll;            // wrap → far future
        if (dl == 0)  dl = 1;                    // 0 = "no deadline"; nudge
        deadline_ns = dl;
    }
    // timeout_us < 0 leaves deadline_ns = 0 → tsleep == sleep.

    spin_unlock(&b->lock);

    // tsleep: blocks on w.rendez until torpor_cond_awoken(&w) returns
    // non-zero OR the deadline lapses. cond is checked under
    // w.rendez.lock at entry; if WAKE already set w.awoken before we
    // got here, tsleep returns AWOKEN immediately (the "wakeup happened
    // between our register and our tsleep" race is benign).
    //
    // A caught note (ARCH 8.8.3) unwinds the wait only while w.awoken is clear
    // (its arm tests the cond first). A WAKE counts a waiter only when its mark
    // found it asleep, and a sleeper's resume reads awoken set, so a counted wake
    // is never lost to EINTR; an uncounted mark leaves the walk going on to the
    // next waiter.
    int sleep_rc = tsleep_noteintr(&w.rendez, torpor_cond_awoken, &w, deadline_ns);

    // Re-take the bucket's lock to unlink. Lifetime invariant: this MUST
    // happen on every return path that reached the bucket-publish
    // step; the waiter's address (a stack local) must not outlive
    // this frame in the bucket chain.
    spin_lock(&b->lock);
    torpor_unlink_locked(b, &w);
    spin_unlock(&b->lock);

    // A wake that took this record off the chain may not have reached it yet, and
    // the record is this frame. wake_held is written under the lock just dropped, so
    // this load sees any wake that took the record. The waker is non-preemptible until
    // it clears it, so this spin is bounded by a running thread.
    while (__atomic_load_n(&w.wake_held, __ATOMIC_ACQUIRE)) {
#ifdef KERNEL_TESTS
        __atomic_fetch_add(&g_torpor_held_spins, 1u, __ATOMIC_RELAXED);
#endif
        __asm__ __volatile__("yield" ::: "memory");
    }

    // Defense-in-depth: scribble the rendez pointer to NULL so a
    // future use-after-stack-return would null-deref instead of
    // walking dirty stack memory.
    w.rendez.waiter = NULL;

    if (sleep_rc == TSLEEP_TIMEDOUT) return TORPOR_ERR_ETIMEDOUT;
    if (sleep_rc == TSLEEP_NOTEINTR) return TORPOR_ERR_EINTR;
    // #811: tsleep may also return TSLEEP_INTR when the Proc is group-
    // terminating (the universal death-wake, ARCH §8.8.1). torpor maps it to
    // TORPOR_OK (same as AWOKEN) deliberately: SYS_TORPOR_WAIT returns through
    // the standard syscall tail, whose el0_return_die_check observes the set
    // group_exit_msg and self-terminates this Thread before it can resume
    // userspace -- so the return value is immaterial, and the waiter must NOT
    // re-sleep (its bucket is already unlinked above). Made explicit because
    // torpor is the one blocking site that absorbs *_INTR via fall-through
    // rather than a dedicated arm.
    return TORPOR_OK;
}

#ifdef KERNEL_TESTS
// Test hook: a wake with a batch waits here, its marks made and the bucket's lock
// dropped, until the stall is lifted or its budget runs out, which is the window a
// batch holds records it has not reached. Holds no lock, and stays non-preemptible,
// so a test on one CPU cannot use it.
#define TORPOR_STALL_BUDGET_NS 10000000000ull
static volatile bool g_torpor_stall, g_torpor_stalled;
void torpor_stall_wakes_for_test(bool on) {
    __atomic_store_n(&g_torpor_stall, on, __ATOMIC_RELEASE);
}
bool torpor_wake_stalled_for_test(void) {
    return __atomic_load_n(&g_torpor_stalled, __ATOMIC_ACQUIRE);
}
static void torpor_wake_stall_point(void) {
    if (!__atomic_load_n(&g_torpor_stall, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&g_torpor_stalled, true, __ATOMIC_RELEASE);
    u64 dl = timer_now_ns() + TORPOR_STALL_BUDGET_NS;
    while (__atomic_load_n(&g_torpor_stall, __ATOMIC_ACQUIRE) && timer_now_ns() < dl)
        __asm__ __volatile__("yield" ::: "memory");
    __atomic_store_n(&g_torpor_stalled, false, __ATOMIC_RELEASE);
}
#endif

s64 sys_torpor_wake_for_proc(struct Proc *p, u64 addr_va, u32 count) {
    if (!p)                                    return TORPOR_ERR_EINVAL;
    if (!torpor_addr_va_ok(addr_va))           return TORPOR_ERR_EINVAL;
    // F7 (P3): `count == 0` is a literal no-op — it does NOT take the
    // bucket's lock, so it does NOT serve as a memory barrier between
    // a producer's preceding user-side store and a consumer's
    // subsequent WAIT-side load. Producers MUST NOT rely on
    // `WAKE(addr, 0)` for any memory-ordering effect; use a real
    // `count > 0` WAKE if the bucket may be non-empty, or a userspace
    // atomic-release for pure announce-without-wake semantics.
    if (count == 0)                            return 0;

    const struct AddrSpace *as = p->as;
    struct torpor_bucket   *b  = torpor_bucket_of(as, addr_va);
    struct torpor_waiter   *batch = NULL, **batch_tail = &batch;
    u32 woken = 0;

    // Non-preemptible from the first mark to the last clear of wake_held: a waiter
    // spins on that clear, and it must be waiting for a running thread.
    spin_preempt_inc();
    spin_lock(&b->lock);

    // F1 (P2 audit fix): the count reflects ACTUAL wakeups. A waiter past its
    // wait, its deadline lapsed or a note taken, is marked and unlinked and not
    // counted, and the walk goes on to the next: the user-side outcome is "this
    // wake went to whoever else was asleep", as it should be. The pthread re-check
    // discipline absorbs a waiter marked before it slept, which returns at once.
    //
    // Bounded by `count`; the common pthread_cond_broadcast case passes
    // UINT32_MAX. The walk visits at most the bucket's length, once, under one
    // hold, so a thread re-waiting in a loop cannot keep it going.
    for (struct torpor_waiter *w = b->head, *n; w != NULL && woken < count; w = n) {
        n = w->next;
        if (w->as != as)               continue;
        if (w->user_va != addr_va)     continue;
        if (w->awoken)                 continue;
        bool asleep = torpor_mark_awoken(w);
        torpor_unlink_locked(b, w);
        if (!asleep) continue;
        __atomic_store_n(&w->wake_held, 1u, __ATOMIC_RELAXED);
        *batch_tail = w;
        batch_tail  = &w->next;
        woken++;
    }
    spin_unlock(&b->lock);
#ifdef KERNEL_TESTS
    if (batch) torpor_wake_stall_point();
#endif

    // wakeup() may spin on a waiter's on_cpu until its CPU has switched it out
    // (ARCH 8.8), and a woken waiter's first act is to come for this bucket's lock.
    // So neither happens under it. Each record stays alive for this because its
    // waiter waits for the clear. Read `next` first: after the clear it may be gone.
    while (batch) {
        struct torpor_waiter *w = batch;
        batch = w->next;
        (void)wakeup(&w->rendez);
        __atomic_store_n(&w->wake_held, 0u, __ATOMIC_RELEASE);
    }
    spin_preempt_dec();
    return (s64)woken;
}

// SYS_EXIT_GROUP / cross-Proc kill cross-thread shootdown (ARCH §7.9.1, I-24).
// Wake EVERY torpor waiter owned by `p`, regardless of the address waited on,
// so each returns from sys_torpor_wait_for_proc to its EL0-return die-check.
// Called by proc_group_terminate AFTER it sets p->group_exit_msg. Walks all
// buckets, one lock at a time -- the group-exit path is rare + not address-keyed
// (unlike the per-address WAKE). The register-after-this-walk lost-wakeup is closed
// by the post-register group_exit_msg check in sys_torpor_wait_for_proc, under the
// same bucket's lock. The waiter stays linked, and the lock held across wakeup() is
// what keeps its record alive: it must take that lock to leave. A rare walk may hold
// one bucket across an on_cpu spin, which WAKE, the hot path, does not.
void torpor_wake_all_for_proc(struct Proc *p) {
    if (!p) return;
    for (u32 idx = 0; idx < TORPOR_HASH_BUCKETS; idx++) {
        struct torpor_bucket *b = &torpor_buckets[idx];
        spin_lock(&b->lock);
        for (struct torpor_waiter *w = b->head; w != NULL; w = w->next) {
            if (w->proc != p) continue;
            if (w->awoken)    continue;
            (void)torpor_mark_awoken(w);
            (void)wakeup(&w->rendez);
        }
        spin_unlock(&b->lock);
    }
}

// The STOP cascade's non-completing wake (contract in torpor.h). Identical
// walk + lock discipline to the death variant above -- the ONLY difference
// is that `awoken` stays clear, so the woken thread's tsleep re-loop finds
// torpor_cond_awoken false, takes the 8c-2 stop detour, and parks with the
// wait preserved. wakeup() re-validates r->waiter under r->lock, so a waiter
// already woken (or mid-transition) is a safe no-op; the stack waiter is
// pinned by its bucket link (unlinked only after its own tsleep returns).
void torpor_stop_wake_all_for_proc(struct Proc *p) {
    if (!p) return;
    for (u32 idx = 0; idx < TORPOR_HASH_BUCKETS; idx++) {
        struct torpor_bucket *b = &torpor_buckets[idx];
        spin_lock(&b->lock);
        for (struct torpor_waiter *w = b->head; w != NULL; w = w->next) {
            if (w->proc != p) continue;
            if (w->awoken)    continue;
            (void)wakeup(&w->rendez);
        }
        spin_unlock(&b->lock);
    }
}
