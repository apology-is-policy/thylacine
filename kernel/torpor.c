// P6-pouch-wait-addr: Thylacine's wait-on-address primitive (sub-chunk 8).
//
// Implements `sys_torpor_wait_for_proc` and `sys_torpor_wake_for_proc` —
// the kernel-side primitive over which pouch's pthread mutex/condvar
// implementation runs (sub-chunk 9 `pouch-threads`). Public surface +
// design rationale: `<thylacine/torpor.h>`.
//
// Mechanism:
//
//   - One global `torpor_lock` (a `spin_lock_t`) protects the wait
//     queue + the per-waiter `awoken` flag. Hash table of 64 buckets
//     keyed by `hash(Proc *, user_va)`. Each bucket is a singly-linked
//     list of `struct torpor_waiter` records.
//   - Each waiter has its own `struct Rendez` for the actual sleep
//     transition. `tsleep` provides the timed/cond-checked sleep
//     primitive; the `awoken` flag is the cond.
//   - WAIT: take `torpor_lock`, `uaccess_load_u32` the futex word, if
//     `!= expected` unlock + return 0; else link the stack-allocated
//     waiter into its bucket, drop `torpor_lock`, `tsleep` on the
//     waiter's private rendez; on resume re-take `torpor_lock`,
//     unlink, unlock, return.
//   - WAKE: take `torpor_lock`, walk the matching bucket, for each
//     waiter whose `(proc, user_va)` matches AND whose `awoken` is
//     still 0: set `awoken = 1` and call `wakeup(&waiter->rendez)`.
//     Drop `torpor_lock`.
//
// Lock order: `torpor_lock` → `waiter->rendez.lock` (only WAKE ever
// takes both; the consumer takes `torpor_lock` then RELEASES it before
// entering `tsleep`, so it never holds both simultaneously). No
// inversion possible.
//
// The no-lost-wakeup invariant is validated by reasoning, not a TLA+
// model — see `<thylacine/torpor.h>` for the proof sketch (CLAUDE.md
// "Spec-to-code suspended", 2026-05-23 broadening).

#include <thylacine/torpor.h>

#include <thylacine/extinction.h>
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
    struct Proc          *proc;         // owner Proc (key component 1)
    u64                   user_va;      // address being waited on (k2)
    struct torpor_waiter *next;         // bucket-chain link
    u32                   awoken;       // 0 sleeping, 1 woken (tsleep cond)
    u32                   _pad;         // explicit alignment padding
};

// F6 (P3): pin struct layout against silent drift. `_pad` is explicit
// because a future field reorder must keep 8-byte total alignment; the
// _Static_assert traps any breakage at build time. Not an ABI type
// (waiter is kernel-stack-only; no cross-boundary visibility), but the
// padding's purpose is structural — best pinned, not implicit.
_Static_assert(sizeof(struct torpor_waiter) % 8 == 0,
               "struct torpor_waiter must be 8-byte aligned");

static spin_lock_t           torpor_lock = SPIN_LOCK_INIT;
static struct torpor_waiter *torpor_buckets[TORPOR_HASH_BUCKETS];

// Hash `(Proc *, user_va) -> bucket index`. The Proc pointer's low
// bits carry SLUB-slot identity (Procs come from the proc kmem cache)
// and the user_va's middle bits hold the dominant variation across
// pthread-call sites (the per-thread mutex word is a heap offset).
// XOR'ing the two with a >>3 user-VA shift (drop the within-word bits)
// gives a workable v1.0 hash. v1.x can pick a smarter hash if
// `pouch-threads` shows bucket-imbalance.
static inline u32 torpor_hash(struct Proc *p, u64 user_va) {
    u64 mix = (u64)(uintptr_t)p ^ (user_va >> 3);
    return (u32)mix & (TORPOR_HASH_BUCKETS - 1u);
}

// tsleep condition: woken iff `waiter->awoken != 0`. Called by tsleep
// under `waiter->rendez.lock`; ordering of the awoken set (under
// `torpor_lock`) vs. this read is established by the WAKE-side's
// `wakeup(&waiter->rendez)` call (which acquires/releases
// `waiter->rendez.lock` itself), pairing release-acquire with this
// cond's read.
static int torpor_cond_awoken(void *arg) {
    const struct torpor_waiter *w = (const struct torpor_waiter *)arg;
    return (int)w->awoken;
}

// Unlink `w` from its bucket. Caller must hold `torpor_lock`. Safe to
// call when `w` is already unlinked (a NULL bucket head or `w` not in
// the chain) — quiet no-op. Used by both the consumer's post-tsleep
// path and the early-exit fast paths.
static void torpor_bucket_unlink_locked(struct torpor_waiter *w) {
    u32 idx = torpor_hash(w->proc, w->user_va);
    struct torpor_waiter **pp = &torpor_buckets[idx];
    while (*pp != NULL) {
        if (*pp == w) {
            *pp = w->next;
            w->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
    // Already unlinked — fall through.
}

// Validate the addr_va arg shared by WAIT + WAKE: must be 4-byte
// aligned, non-zero, and under UACCESS_USER_VA_TOP. Returns 1 on OK,
// 0 on reject. Centralised so the two syscalls agree on the rules.
//
// F10 (P3): the 4-byte alignment check is load-bearing for
// `uaccess_load_u32` — the asm primitive's fault-fixup table catches
// translation/permission faults only (`arch/arm64/exception.c`); an
// unaligned LDR on SCTLR_EL1.A-checking memory would alignment-fault
// to the "unclassified kernel fault" extinction path, not the fixup
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
    // until we publish it under torpor_lock).
    struct torpor_waiter w;
    rendez_init(&w.rendez);
    w.proc    = p;
    w.user_va = addr_va;
    w.next    = NULL;
    w.awoken  = 0;
    w._pad    = 0;

    spin_lock(&torpor_lock);

    // Load the user-VA word and compare under torpor_lock. The lock
    // serialises with WAKE's same-lock walk: either WAKE's unlock has
    // happened-before this lock (and we see the producer's pre-WAKE
    // store via the lock release/acquire chain — load returns the new
    // value, we return 0 without sleeping), or this lock happens-
    // before WAKE's lock (and we register before WAKE walks the
    // bucket — WAKE delivers the wakeup).
    u32 observed = 0;
    if (uaccess_load_u32(addr_va, &observed) != 0) {
        spin_unlock(&torpor_lock);
        return TORPOR_ERR_EFAULT;
    }
    if (observed != expected) {
        // Fast path: value already differs. No sleep, no register;
        // caller re-evaluates the predicate and proceeds.
        spin_unlock(&torpor_lock);
        return TORPOR_OK;
    }

    // Register: link the waiter at the bucket head. We publish the
    // waiter under torpor_lock so WAKE's walk sees it; until we drop
    // torpor_lock, WAKE cannot proceed past its own lock acquire.
    u32 idx = torpor_hash(p, addr_va);
    w.next = torpor_buckets[idx];
    torpor_buckets[idx] = &w;

    // SYS_EXIT_GROUP / kill cross-thread shootdown lost-wakeup close (I-24).
    // Re-check the Proc's group-termination flag AFTER registering, UNDER
    // torpor_lock -- the same lock proc_group_terminate's torpor_wake_all walk
    // takes. This makes the wake airtight against the register-vs-walk race:
    //   - if we registered BEFORE the wake-all walk, the walk finds us +
    //     wakes us (awoken=1) -> tsleep returns immediately;
    //   - if we register AFTER the walk (it missed us), then the flag-set
    //     happened-before the walk, the walk released torpor_lock, and we
    //     acquired it to register -- so this acquire-load observes the set
    //     flag here and we do NOT sleep.
    // Either way a group-terminating Proc's futex sleeper does not sleep
    // through the exit; it returns + dies at its EL0-return die-check.
    if (__atomic_load_n(&p->group_exit_msg, __ATOMIC_ACQUIRE) != NULL) {
        torpor_bucket_unlink_locked(&w);
        spin_unlock(&torpor_lock);
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
        // blocked on torpor_lock above and runs after we drop it).
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

    spin_unlock(&torpor_lock);

    // tsleep: blocks on w.rendez until torpor_cond_awoken(&w) returns
    // non-zero OR the deadline lapses. cond is checked under
    // w.rendez.lock at entry; if WAKE already set w.awoken before we
    // got here, tsleep returns AWOKEN immediately (the "wakeup happened
    // between our register and our tsleep" race is benign).
    int sleep_rc = tsleep(&w.rendez, torpor_cond_awoken, &w, deadline_ns);

    // Re-take torpor_lock to unlink. Lifetime invariant: this MUST
    // happen on every return path that reached the bucket-publish
    // step; the waiter's address (a stack local) must not outlive
    // this frame in the bucket chain.
    spin_lock(&torpor_lock);
    torpor_bucket_unlink_locked(&w);
    // Defense-in-depth: scribble the rendez pointer to NULL so a
    // future use-after-stack-return would null-deref instead of
    // walking dirty stack memory.
    w.rendez.waiter = NULL;
    spin_unlock(&torpor_lock);

    if (sleep_rc == TSLEEP_TIMEDOUT) return TORPOR_ERR_ETIMEDOUT;
    return TORPOR_OK;
}

s64 sys_torpor_wake_for_proc(struct Proc *p, u64 addr_va, u32 count) {
    if (!p)                                    return TORPOR_ERR_EINVAL;
    if (!torpor_addr_va_ok(addr_va))           return TORPOR_ERR_EINVAL;
    // F7 (P3): `count == 0` is a literal no-op — it does NOT take
    // `torpor_lock`, so it does NOT serve as a memory barrier between
    // a producer's preceding user-side store and a consumer's
    // subsequent WAIT-side load. Producers MUST NOT rely on
    // `WAKE(addr, 0)` for any memory-ordering effect; use a real
    // `count > 0` WAKE if the bucket may be non-empty, or a userspace
    // atomic-release for pure announce-without-wake semantics.
    if (count == 0)                            return 0;

    u32 woken = 0;
    spin_lock(&torpor_lock);

    // Walk the matching bucket. For each unwokken waiter on
    // `(p, addr_va)`: mark awoken + try to wake its private rendez.
    // We leave the waiter linked — the consumer unlinks on its own
    // post-tsleep path. Subsequent WAKE walks skip already-woken
    // waiters via the `awoken == 0` filter.
    //
    // F1 (P2 audit fix): the count reflects ACTUAL wakeups (waiters
    // that wakeup() transitioned from SLEEPING to RUNNABLE), not just
    // "matched waiters whose awoken flag we set". A waiter whose
    // tsleep already lapsed on its own deadline (timerwait_tick wake)
    // has r->waiter == NULL by the time we get here — wakeup() returns
    // 0 and we do NOT count it. The user-side outcome is then "wake
    // call says 0 delivered, consumer returns ETIMEDOUT" — accurate.
    // (Pre-fix the count overstated by one in the timer-wake-first
    // race.) The `awoken = 1` flag is still set so a subsequent post-
    // sched cond check returns AWOKEN if the consumer hasn't yet
    // re-evaluated cond + sleep_timedout — the pthread re-check
    // discipline absorbs that case.
    //
    // Bounded by `count`; the common pthread_cond_broadcast case
    // passes UINT32_MAX (wake everyone). The walk visits at most the
    // bucket length, which at v1.0 stays small (Procs are single-
    // threaded; under multi-thread contention a bucket holds at most
    // one per Proc/VA pair plus a small set of stragglers if the
    // hash is uneven).
    //
    // F2 (P2 audit, documented hazard): `wakeup()` may call
    // `wake_rendez_waiter`, which spins on the woken thread's `on_cpu`
    // waiting for a peer CPU's context-switch-out to drain. We hold
    // `torpor_lock` across that spin — under heavy multi-Proc
    // contention this serializes everyone. v1.0 is single-Proc-mostly
    // so the hazard is dormant; v1.x can split torpor_lock per-bucket
    // OR adopt a two-pass "mark-and-list under lock, dispatch wakes
    // after lock-drop" pattern. See Known caveats in
    // `docs/reference/80-torpor.md`.
    u32 idx = torpor_hash(p, addr_va);
    for (struct torpor_waiter *w = torpor_buckets[idx];
         w != NULL && woken < count;
         w = w->next) {
        if (w->proc != p)              continue;
        if (w->user_va != addr_va)     continue;
        if (w->awoken)                 continue;
        w->awoken = 1;
        if (wakeup(&w->rendez)) woken++;
    }

    spin_unlock(&torpor_lock);
    return (s64)woken;
}

// SYS_EXIT_GROUP / cross-Proc kill cross-thread shootdown (ARCH §7.9.1, I-24).
// Wake EVERY torpor waiter owned by `p`, regardless of the address waited on,
// so each returns from sys_torpor_wait_for_proc to its EL0-return die-check.
// Called by proc_group_terminate AFTER it sets p->group_exit_msg. Walks all
// buckets -- the group-exit path is rare + not address-keyed (unlike the
// per-(proc,addr) WAKE). Same awoken-flag + wakeup() discipline as
// sys_torpor_wake_for_proc; the register-after-this-walk lost-wakeup is closed
// by the post-register group_exit_msg check in sys_torpor_wait_for_proc. Same
// F2 hazard as the per-address wake (holds torpor_lock across wakeup()'s
// on_cpu spin); dormant at v1.0, the v1.x per-bucket split applies uniformly.
void torpor_wake_all_for_proc(struct Proc *p) {
    if (!p) return;
    spin_lock(&torpor_lock);
    for (u32 idx = 0; idx < TORPOR_HASH_BUCKETS; idx++) {
        for (struct torpor_waiter *w = torpor_buckets[idx];
             w != NULL; w = w->next) {
            if (w->proc != p) continue;
            if (w->awoken)    continue;
            w->awoken = 1;
            (void)wakeup(&w->rendez);
        }
    }
    spin_unlock(&torpor_lock);
}
