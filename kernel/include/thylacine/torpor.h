// P6-pouch-wait-addr: Thylacine's wait-on-address primitive (sub-chunk 8).
//
// `torpor` is the kernel-side primitive for blocking on a user-VA word
// and waking blocked threads when the word may have changed. It is the
// Thylacine-native equivalent of Linux `futex(FUTEX_WAIT)` /
// `futex(FUTEX_WAKE)` — the substrate pouch's pthread mutex/condvar
// implementation (sub-chunk 9 `pouch-threads`) builds on.
//
// Two operations:
//   - `sys_torpor_wait_for_proc(p, addr_va, expected, timeout_us)` --
//     atomic-compare `*addr_va` against `expected` under its bucket's
//     lock; if they match, register on the wait-queue keyed by
//     `(p->as, addr_va)`, drop the lock, and `tsleep` until a matching
//     WAKE arrives OR the timeout lapses. If they don't match, return
//     0 immediately.
//   - `sys_torpor_wake_for_proc(p, addr_va, count)` -- wake up to
//     `count` waiters whose `(as, addr_va)` matches, oldest first.
//
// Invariant (I-9, specialized to wait-on-address): no wakeup is lost
// between the value check and the sleep; specs/torpor.tla. The consumer's
// atomic-load runs UNDER the bucket's lock, then the registration runs
// before the lock is released; the producer's WAKE takes the same lock
// to walk the bucket. By lock acquire/release ordering:
//
//   (a) WAKE-unlock happens-before WAIT-lock -- the consumer's load
//       sees the producer's preceding user-side store and returns 0
//       immediately without sleeping.
//   (b) WAIT-lock happens-before WAKE-lock -- the consumer is
//       registered in the bucket before WAKE walks it, and WAKE
//       delivers the wakeup.
//   (c) WAIT registered + tsleeped, then the consumer's own deadline
//       lapsed. A mark that finds it asleep counts it, and it reads the
//       mark on resume, whichever wake reaches it first, so it returns 0.
//       A mark that finds it past its wait does not count it, and the walk
//       goes on to the next waiter. The user's atomic re-check on return,
//       the standard pthread retry discipline, covers both.
//
// The remaining case is a spurious wake (consumer's load returned the
// pre-store value but registers under the lock just before the
// producer stores+WAKEs); the user's atomic re-check on return
// (musl's standard discipline) handles it.
//
// Key design choices:
//   - Keyed on the address space: the bucket key is `(AddrSpace *, user_va)`,
//     so Procs sharing a space (rfork(RFPROC|RFMEM)) wake each other. Procs
//     sharing a page at their own VAs do not (seam-torpor-cross-proc).
//   - First in, first out: a wake takes the oldest, as Linux's futex does
//     within a priority, so a mutex's oldest waiter cannot starve.
//   - A lock per bucket, and no wake under it: wakeup() can spin on a
//     thread's on_cpu, and a woken waiter's first act is to take that lock.
//   - Plain `LDR` (not `LDAR`): a consumer that PARKS loads under the
//     bucket's lock; the lock-acquire (acquire op) synchronizes with
//     the producer's prior WAKE-unlock that follows a user-side store.
//     Standard Linux-futex memory-ordering discipline (the producer is
//     contractually required to call WAKE after every value change that
//     should unblock a waiter).
//   - Lock-free pre-lock compare (the go-arc osyield floor): a
//     `prefault != expected` mismatch returns TORPOR_OK WITHOUT taking
//     a lock. No waiter registers on that path, so the a/b/c
//     no-lost-wakeup cases above are never entered; the mismatch
//     return deliberately provides ONLY the plain aligned-u32 load's
//     ordering (single-copy-atomic), weaker than the old locked path's
//     incidental acquire -- sound because every futex client re-checks
//     its own predicate with its own atomics on return (musl, the Go
//     runtime, libthyla-rs torpor::wait's documented contract). A
//     stale `== expected` read does NOT early-return; it falls through
//     to the authoritative under-lock load, so the parking path's
//     ordering is unchanged.
//   - Stack-allocated waiters: the per-call waiter struct lives on the
//     consumer's kernel stack. Every exit path unlinks it from the
//     bucket, and waits out any wake still holding it, before the stack
//     frame is reaped.

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
// A caught note ended the wait (ARCH 8.8.3). Only a Linux futex wait (a call on
// signal(7)'s list) can receive it; a native torpor_wait stays death-only.
#define TORPOR_ERR_EINTR         (-4)

// Bucket count for the torpor wait-queue hash table, each bucket a lock and
// its list on a cache line of their own (16 KiB of BSS). Two words that hash
// together share a lock, so this is what buys their threads' independence.
// The bucket count must be a power of two so the hash modulo can be a bitmask.
#define TORPOR_HASH_BUCKETS  256u
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

// Wake up to `count` threads whose `(p->as, addr_va)` matches. Returns the
// number of waiters actually woken (`>= 0`), or `TORPOR_ERR_EINVAL` /
// `TORPOR_ERR_EFAULT` for bad args. `count == 0` is legal (no-op,
// returns 0). `count == UINT32_MAX` means "wake all"
// (pthread_cond_broadcast's path).
//
// WAKE does NOT read the user-VA word — it only hashes the address +
// walks the bucket. The producer is contractually responsible for
// performing the user-side atomic store BEFORE calling WAKE; the
// kernel observes that store via the lock-acquire ordering when the
// consumer's later WAIT reads `*addr_va` under its bucket's lock.
s64 sys_torpor_wake_for_proc(struct Proc *p, u64 addr_va, u32 count);

// SYS_EXIT_GROUP / cross-Proc kill cross-thread shootdown (ARCH §7.9.1,
// invariant I-24). Wake EVERY torpor waiter owned by `p` (all addresses) so
// each returns from sys_torpor_wait_for_proc to its EL0-return die-check.
// Called only by proc_group_terminate, after it publishes p->group_exit_msg;
// the post-register group_exit_msg re-check in sys_torpor_wait_for_proc closes
// the register-after-this-walk lost-wakeup. Not a syscall surface.
void torpor_wake_all_for_proc(struct Proc *p);

// PTY-4e (#19): the STOP cascade's non-completing variant. Wakes every one of
// `p`'s torpor waiters WITHOUT setting `awoken`, so the woken thread's
// torpor_cond_awoken re-check fails, its tsleep re-loop hits the 8c-2 stop
// detour, and it parks with the WAIT PRESERVED -- on resume it re-registers
// with its original deadline (the parks-and-reparks contract, exactly the
// tsleep-sleeper semantics; the Linux SIGSTOP-over-futex_wait restart shape).
// The completing torpor_wake_all_for_proc above is the DEATH cascade's (the
// proc dies at its tail, so the fabricated TORPOR_OK is never observed);
// using it for a SURVIVING stop made every torpor-timed wait -- e.g. a
// time::sleep-based /bin/sleep -- spuriously COMPLETE on fg/SYS_TTY_CONT
// resume (the job "finished" instead of continuing; the #19 root cause).
// A REAL wake landing during the stop-park still delivers: the waiter stays
// bucket-linked across the park, the poster sets `awoken`, and the resumed
// re-loop's cond check returns AWOKEN immediately.
void torpor_stop_wake_all_for_proc(struct Proc *p);

#ifdef KERNEL_TESTS
void torpor_after_prefault_for_test(void (*fn)(void *), void *arg);
u64  torpor_held_spins_for_test(void);
void torpor_stall_wakes_for_test(bool on);
bool torpor_wake_stalled_for_test(void);
#endif

#endif // THYLACINE_TORPOR_H
