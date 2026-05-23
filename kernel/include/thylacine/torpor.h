// P6-pouch-wait-addr: Thylacine's wait-on-address primitive (sub-chunk 8).
//
// `torpor` is the kernel-side primitive for blocking on a user-VA word
// and waking blocked threads when the word may have changed. It is the
// Thylacine-native equivalent of Linux `futex(FUTEX_WAIT)` /
// `futex(FUTEX_WAKE)` — the substrate pouch's pthread mutex/condvar
// implementation (sub-chunk 9 `pouch-threads`) builds on.
//
// Two operations:
//   - `sys_torpor_wait_for_proc(p, addr_va, expected, timeout_us)` —
//     atomic-compare `*addr_va` against `expected` under the kernel
//     `torpor_lock`; if they match, register on the wait-queue keyed
//     by `(p, addr_va)`, drop the lock, and `tsleep` until a matching
//     WAKE arrives OR the timeout lapses. If they don't match, return
//     0 immediately.
//   - `sys_torpor_wake_for_proc(p, addr_va, count)` — wake up to
//     `count` waiters whose `(p, addr_va)` matches.
//
// Invariant (I-9, specialized to wait-on-address): no wakeup is lost
// between the value check and the sleep. The proof: the consumer's
// atomic-load runs UNDER `torpor_lock`, then the registration runs
// before the lock is released; the producer's WAKE takes the same
// `torpor_lock` to walk the bucket. By lock acquire/release ordering:
//
//   (a) WAKE-unlock happens-before WAIT-lock — the consumer's load
//       sees the producer's preceding user-side store and returns 0
//       immediately without sleeping.
//   (b) WAIT-lock happens-before WAKE-lock — the consumer is
//       registered in the bucket before WAKE walks it, and WAKE
//       delivers the wakeup.
//   (c) WAIT registered + tsleeped, then the consumer's own deadline
//       lapsed (timer-driven timeout). WAKE may arrive concurrently
//       with the timeout: the bucket walk sets `awoken = 1` and
//       attempts wakeup, but if `wake_rendez_waiter` already cleared
//       the rendez (the timer ran first), wakeup is a no-op (returns
//       0). The consumer returns ETIMEDOUT; the user's atomic re-
//       check on return — the standard pthread retry discipline — re-
//       reads the user-side word and proceeds. F1 (P2 audit) makes
//       the WAKE-side count reflect ACTUAL wakeups, so the user-
//       visible "wake delivered" count + the per-thread return code
//       agree on whether a wake landed.
//
// The remaining case is a spurious wake (consumer's load returned the
// pre-store value but registers under the lock just before the
// producer stores+WAKEs); the user's atomic re-check on return
// (musl's standard discipline) handles it.
//
// Per CLAUDE.md "Spec-to-code suspended" (broadened 2026-05-23): no
// `specs/futex.tla` is written for sub-chunk 8. The reasoning above is
// the canonical validation of the no-lost-wakeup invariant; the audit
// round + the runtime tests are the rigor.
//
// Key design choices:
//   - Per-Proc keying: the bucket key is `(Proc *, user_va)`. v1.0 has
//     no cross-Proc shared anonymous memory (POUCH-DESIGN.md §10, Tier
//     2 burrows are deferred), so a `(Proc, VA)` key is sufficient.
//     Cross-Proc shared-futex semantics land with Tier 2.
//   - Plain `LDR` (not `LDAR`): the consumer's load runs under the
//     torpor_lock; the lock-acquire (acquire op) synchronizes with any
//     prior lock-release on the same lock — including the producer's
//     WAKE-unlock that follows a user-side store. Standard Linux-futex
//     memory-ordering discipline (the producer is contractually
//     required to call WAKE after every value change that should
//     unblock a waiter).
//   - Stack-allocated waiters: the per-call waiter struct lives on the
//     consumer's kernel stack. Every exit path unlinks it from the
//     bucket before the stack frame is reaped, so its address never
//     outlives its lifetime.

#ifndef THYLACINE_TORPOR_H
#define THYLACINE_TORPOR_H

#include <thylacine/types.h>

struct Proc;

// Maximum permitted `timeout_us` for `sys_torpor_wait_for_proc`. v1.0
// caps the deadline at one hour so callers cannot easily overflow the
// (u64 ns) timer arithmetic and so a runaway "block forever" callsite
// is bounded by a sanity ceiling. The pthread API exposes per-call
// timeouts that comfortably fit (mutex/condvar typical timeouts are
// seconds-to-minutes; a 1-hour cap is generous). Indefinite blocking
// is requested with `timeout_us < 0`.
#define TORPOR_MAX_TIMEOUT_US  (60ull * 60ull * 1000000ull)

// Return values from `sys_torpor_wait_for_proc`. The kernel returns
// the explicit `-errno` integers; pouch's `syscall_ret.c` decodes any
// value in `[-4095, -2]` as `-errno` (returns -1 to userspace with
// errno set). The numeric values mirror Linux's errno (musl's
// `bits/errno.h`) so a pouch caller observes a familiar errno.
//
// `torpor_wait` returns 0 in two cases — (1) the value matched at the
// load AND the wait was woken, OR (2) the value DIDN'T match at the
// load (the fast path: no sleep happened). Userspace tells them apart
// by re-checking the atomic word on return; in both cases the proper
// response is the same (re-evaluate the predicate). Linux returns
// EAGAIN for case (2); v1.0 collapses it onto 0 because pthread cond
// wait and mutex wait both re-check the predicate regardless. v1.x
// can extend if a PI futex or similar discriminator is needed.
#define TORPOR_OK                 0
#define TORPOR_ERR_EINVAL       (-22)
#define TORPOR_ERR_EFAULT       (-14)
#define TORPOR_ERR_ETIMEDOUT   (-110)

// Bucket count for the torpor wait-queue hash table. A static array of
// pointers (one head per bucket); chained on a per-waiter `next`. 64
// buckets keeps the table small (~512 B) and uncontended at v1.0
// (single-threaded Procs — buckets stay near-empty). v1.x can size up
// once `pouch-threads` introduces real contention. The bucket count
// must be a power of two so the hash modulo can be a bitmask.
#define TORPOR_HASH_BUCKETS  64u
_Static_assert((TORPOR_HASH_BUCKETS & (TORPOR_HASH_BUCKETS - 1u)) == 0,
               "TORPOR_HASH_BUCKETS must be a power of two");

// Public surface — the `_for_proc` inners are exported so the kernel
// test harness can drive them on a fresh `proc_alloc`'d Proc (the
// `sys_pipe_for_proc` pattern; mirrors the burrow + chroot surface).
// The SVC handlers in `kernel/syscall.c` are thin `current_thread()`
// wrappers over these.

// Block on `addr_va` until `*addr_va` may have changed and a matching
// `sys_torpor_wake_for_proc` arrives, OR the timeout elapses. Returns:
//
//   TORPOR_OK             — woken (or *addr_va already != expected at
//                            entry; userspace re-checks regardless)
//   TORPOR_ERR_ETIMEDOUT  — `timeout_us >= 0` and the deadline lapsed
//   TORPOR_ERR_EFAULT     — `addr_va` outside the user-VA bound or
//                            translation/permission fault on load
//   TORPOR_ERR_EINVAL     — `p == NULL`, `addr_va` not 4-byte aligned,
//                            `addr_va == 0`, or `timeout_us` outside
//                            the supported range
//
// `timeout_us < 0` blocks indefinitely (no deadline).
// `timeout_us == 0` is treated as a fast-path probe: register, check,
// and return TORPOR_ERR_ETIMEDOUT if the value still matched. The
// "always-block" / "always-return" cases are useful pthread building
// blocks (pthread_mutex_trylock with futex backing is the closest
// real-world consumer).
// Note: `p` MUST equal `current_thread()->proc` — the user-VA load
// inside this call walks the current thread's TTBR0, so the bucket-key
// Proc and the load-namespace must match. The SVC wrapper always
// satisfies this; tests driving the `_for_proc` form must run on a
// thread of `p`. Enforced via an `extinction` assert (F4, P3 audit).
s64 sys_torpor_wait_for_proc(struct Proc *p, u64 addr_va, u32 expected,
                             s64 timeout_us);

// Wake up to `count` threads whose `(p, addr_va)` matches. Returns the
// number of waiters actually woken (`>= 0`), or `TORPOR_ERR_EINVAL` /
// `TORPOR_ERR_EFAULT` for bad args. `count == 0` is legal (no-op,
// returns 0). `count == UINT32_MAX` means "wake all"
// (pthread_cond_broadcast's path).
//
// WAKE does NOT read the user-VA word — it only hashes the address +
// walks the bucket. The producer is contractually responsible for
// performing the user-side atomic store BEFORE calling WAKE; the
// kernel observes that store via the lock-acquire ordering when the
// consumer's later WAIT reads `*addr_va` under `torpor_lock`.
s64 sys_torpor_wake_for_proc(struct Proc *p, u64 addr_va, u32 count);

#endif // THYLACINE_TORPOR_H
