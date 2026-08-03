// In-kernel test harness — minimal at v1.0.
//
// Lives at kernel/test/. Each test is a void(void) function registered
// in g_tests[]. test_run_all walks the array, runs each, prints
// PASS/FAIL per test. The harness is freestanding-friendly (no
// constructors, no linker sections), trades a tiny bit of manual
// registration for predictable behavior in the kernel environment.
//
// Tests cover **stable leaf APIs only**: pure functions, DTB-parser
// surfaces, allocator smoke flows. Internal-data-structure invariants
// (buddy free-list shape, SLUB partial-list discipline) are tested
// implicitly via the smoke flows; explicit invariant tests would need
// rewriting as those subsystems evolve, so we defer them. Phase 2's
// process / handle / BURROW surfaces will get their own tests when the
// APIs stabilize.
//
// Per CLAUDE.md "test what is stable" guidance + the user's directive
// to avoid premature rewriting.
//
// Future evolution (P1-I and beyond):
//   - Host-side test target (compile leaf modules with -fsanitize).
//   - 10000-iteration alloc/free leak check.
//   - TLA+ specs gate-tied per ARCH §25.2.

#ifndef THYLACINE_KERNEL_TEST_H
#define THYLACINE_KERNEL_TEST_H

#include <thylacine/types.h>

struct test_case {
    const char *name;       // human-readable identifier
    void (*fn)(void);       // test body; calls TEST_ASSERT / TEST_FAIL
    bool failed;            // set by the harness if the test fails
    const char *fail_msg;
};

// Sentinel-terminated array of all tests. Defined in kernel/test/test.c.
// New tests are added by extending the array (no constructors needed).
extern struct test_case g_tests[];

// Run every test in g_tests[]. Reports per-test PASS/FAIL on UART.
// Sets each test_case's failed / fail_msg fields for post-run inspection.
void test_run_all(void);

// True iff every test in g_tests[] passed. Use after test_run_all to
// gate the boot path — boot_main calls this and extinctions if false.
bool test_all_passed(void);

// Number of tests that ran + count of passes / failures.
unsigned test_total(void);
unsigned test_passed(void);
unsigned test_failed(void);

// Called from inside a test_case's fn to report a failure. Sets
// the current test's failed flag and stashes the message; caller
// should `return` immediately afterward (the TEST_ASSERT macro does
// this for you).
void test_fail(const char *msg);

// Called from inside a test_case's fn to record a non-fatal warning.
// Logs a [SOFT-WARN] line and bumps the soft-warn counter; does NOT
// fail the test. Use via TEST_SOFT_WARN, never directly to soften a
// correctness check.
void test_soft_warn(const char *msg);

// Count of soft-warns recorded during the last test_run_all. Surfaced
// in the boot summary so host throttling is visible without failing CI.
unsigned test_soft_warns(void);

// Convenience macros. TEST_ASSERT short-circuits the current test on
// failure (returns from the test_case's fn). TEST_EXPECT_EQ is a
// thin wrapper that stringifies operands for a more useful message.
#define TEST_ASSERT(cond, msg)                                  \
    do {                                                        \
        if (!(cond)) {                                          \
            test_fail(msg);                                     \
            return;                                             \
        }                                                       \
    } while (0)

#define TEST_EXPECT_EQ(a, b, msg) TEST_ASSERT((a) == (b), msg)
#define TEST_EXPECT_NE(a, b, msg) TEST_ASSERT((a) != (b), msg)

// TEST_SOFT_WARN: a host-fragility budget that must NOT fail the boot. On a
// false cond it logs a [SOFT-WARN] line and CONTINUES (the test still passes).
// Use ONLY for host-timing budgets (e.g. a QEMU-emulation latency ceiling)
// where a miss means "the host was throttled," not "the kernel is broken" --
// a hard TEST_ASSERT there extincts the boot (boot_main gates on
// test_all_passed) and thereby MASKS any real fault that would surface later
// in the SAME boot (the #860 class lived in the post-test production bringup;
// SMP-REVIEW-FINDINGS section 7d). NEVER use it to soften a correctness check.
#define TEST_SOFT_WARN(cond, msg)         \
    do {                                  \
        if (!(cond)) test_soft_warn(msg); \
    } while (0)

// Terminal park + reap for in-kernel test kthreads (#108/#109). A helper
// kthread that has finished its work cannot exits()/thread_exit_self (those
// extinct from kproc) and must not bare-return (the trampoline WFE-spins).
//
//   test_kthread_park_terminal: the kthread parks TERMINALLY -- it transitions
//   to THREAD_EXITING (never re-enqueued -> never RUNNABLE -> never work-stolen
//   -> never busy-spins a CPU; replaces the leak-prone `for (;;) sched()`),
//   publishes *exited (RELEASE), then sched()s away permanently. Never returns.
//   `*exited` must be reset to false before the kthread is run.
//
//   test_kthread_join_free: the joiner waits until *exited is published
//   (ACQUIRE pairs with the park's RELEASE -> thread_free's not-RUNNING gate is
//   guaranteed), then reclaims t (its on_cpu spin covers the switch-away).
//
// Bodies in kernel/test/test.c.
struct Thread;
void test_kthread_park_terminal(volatile bool *exited);
void test_kthread_join_free(struct Thread *t, volatile bool *exited);

// TEST_YIELD_UNTIL(cond) — wait for a PEER-THREAD observable, bounded.
//
// The thread_create / ready / sched pattern assumes one sched() runs the peer
// to its next block. That stops being true under SMP: select_target_cpu can
// place the woken peer on ANOTHER CPU, so it is RUNNABLE but not yet dispatched
// when we resume, and an assert on its progress counter reads the pre-wake
// value. Two independent witnesses, both 1-in-10, both with the peer proven
// woken: srvconn.teardown_wakes_blocked on ubsan-smp8 (2026-07-20, runnable-dump
// showed it RUNNABLE on cpu=4) and srvconn.role_park_second_reader on
// default-smp4 (2026-07-28, state=2 == THREAD_RUNNABLE on cpu=1). The wake was
// delivered in both; only the observation was early. I-9 was never at issue.
//
// Spinning on the OBSERVABLE does not weaken the test: a genuinely LOST wake
// never satisfies `cond`, so the budget expires and the test fails.
//
// The budget is WALL-CLOCK, not an iteration count. An iteration count is the
// wrong unit here — when the run tree is empty sched() returns immediately, so
// a bounded spin can burn its whole budget in microseconds while the peer is
// still legitimately running on another CPU, converting a healthy test into a
// spurious failure. Time is what we are actually waiting on.
//
// Exhaustion FAILS, naming the condition, rather than falling through to the
// downstream assert. Falling through is how a WRONG observable stays invisible:
// the wait does nothing, the downstream assert passes on its own, and the site
// is silently racy again with no signal that the guard never guarded.
//
// `sched()` is declared by the includer (every user already calls it directly);
// timer_now_ns is forward-declared here to keep an arch header out of test.h.
u64 timer_now_ns(void);

#define TEST_YIELD_BUDGET_NS (2ull * 1000ull * 1000ull * 1000ull)

// Positive control for the waits themselves. Exhaustion is loud, but the
// OPPOSITE failure is silent: a condition already true on entry exits at once,
// so the guard is a no-op and the site is racy again with nothing to show for
// it. That is indistinguishable from a healthy run by any pass/fail signal, so
// the suite reports how many waits ACTUALLY had to wait. A spun count of zero
// means every guard here is inert -- the conversion measuring itself rather
// than being taken on faith.
// Atomic because TEST_YIELD_UNTIL_PROC (below) runs these increments in a CHILD
// PROC concurrently with the boot thread's own waits. Relaxed: they are counters
// with no ordering duty, read only after the run loop ends.
extern unsigned g_test_yield_calls;   // TEST_YIELD_UNTIL* invocations
extern unsigned g_test_yield_spun;    // ...of those, ones that actually waited
extern unsigned g_test_yield_deep;    // ...of those, ones needing >1 yield
#define TEST_YIELD_BUMP(c) __atomic_fetch_add(&(c), 1u, __ATOMIC_RELAXED)
//
// `deep` is the sharp one. The loop tests the condition BEFORE its first
// sched(), so `spun` counts anything that took even one yield -- which is
// exactly what the bare sched() it replaced already did. Only a wait that
// needed a SECOND yield did work the old single sched() structurally could
// not, i.e. observed the peer-not-yet-dispatched window this change exists to
// close. A persistent deep==0 across SMP configs would say the race is not
// being exercised here, not that it is absent.

#define TEST_YIELD_UNTIL(cond)                                              \
    do {                                                                    \
        u64  _yu_deadline = timer_now_ns() + TEST_YIELD_BUDGET_NS;          \
        bool _yu_spun = false, _yu_deep = false;                            \
        TEST_YIELD_BUMP(g_test_yield_calls);                                \
        while (!(cond)) {                                                   \
            if (_yu_spun) _yu_deep = true;                                  \
            _yu_spun = true;                                                \
            TEST_ASSERT(timer_now_ns() < _yu_deadline,                      \
                "TEST_YIELD_UNTIL budget exhausted waiting for: " #cond);   \
            sched();                                                        \
        }                                                                   \
        if (_yu_spun) TEST_YIELD_BUMP(g_test_yield_spun);                   \
        if (_yu_deep) TEST_YIELD_BUMP(g_test_yield_deep);                   \
    } while (0)

// TEST_YIELD_UNTIL_SOFT — same bounded wait, but falls through on exhaustion
// instead of failing, leaving the caller's own assert to report.
//
// Use ONLY where the wait sits inside a section that owns GLOBAL state needing
// cleanup — a held console TX role, a stalled UART, an armed echo capture. The
// loud form fails via TEST_ASSERT, which RETURNS from the test function, so an
// early return there skips the release and strands that state for the rest of
// the boot: a held role parks every later console writer, and a stalled UART
// silences the console entirely. A rare spurious FAIL is worth catching; a dead
// console that swallows the rest of the suite is not.
//
// This is NOT the soft option for ordinary sites. Everywhere else the loud form
// is required, because a silent fall-through is precisely how a wrong observable
// hides: the wait does nothing and the downstream assert passes on its own.
#define TEST_YIELD_UNTIL_SOFT(cond)                                         \
    do {                                                                    \
        u64  _yus_deadline = timer_now_ns() + TEST_YIELD_BUDGET_NS;         \
        bool _yus_spun = false, _yus_deep = false;                          \
        TEST_YIELD_BUMP(g_test_yield_calls);                                \
        while (!(cond) && timer_now_ns() < _yus_deadline) {                 \
            if (_yus_spun) _yus_deep = true;                                 \
            _yus_spun = true;                                               \
            sched();                                                        \
        }                                                                   \
        if (_yus_spun) TEST_YIELD_BUMP(g_test_yield_spun);                  \
        if (_yus_deep) TEST_YIELD_BUMP(g_test_yield_deep);                  \
    } while (0)

// TEST_YIELD_UNTIL_PROC — the same bounded wait, for a wait that runs inside a
// CHILD PROC's entry thunk rather than on the test thread. (#134)
//
// TEST_ASSERT IS WRONG HERE, in two independent ways, and both are worse than
// the unbounded spin it would replace:
//
//   1. It expands to `test_fail(msg); return;`. Returning from a Proc entry
//      thunk lands on thread_trampoline's `1: wfe / b 1b` (context.S) -- an
//      entry that returns without exits() is a kernel-thread bug and the
//      trampoline halts it FOREVER. The thunk's Proc then never exits, so the
//      parent's wait_pid never returns either. That converts one stuck
//      handshake into two permanently parked threads: strictly worse than the
//      sched() spin, which at least keeps yielding.
//
//   2. test_fail writes current_test->failed / ->fail_msg. Those belong to the
//      RUNNER, on the boot thread. A child Proc writing them races the runner,
//      and a child that expires after the runner has moved on reddens whatever
//      test is running THEN -- an innocent one. That is exactly the #136-F2
//      shape (a diagnostic reaching into state it does not own), which we just
//      finished removing from the console backstop.
//
// So the thunk form terminates its own Proc instead: it records the condition
// in a global the RUNNER reads (the only safe cross-Proc channel here) and
// exits() with a distinguishable status. No hang, no foreign write, and the
// condition that never came true is named.
//
// THE RUNNER DOES NOT ATTRIBUTE IT TO A TEST. That was tried and measured: a
// sabotaged wait failed its test, which returned early and so never released
// its OWN spinner thunks, which expired 2 s later while the runner was two
// tests further on -- blaming `current_test` reddened an innocent bystander,
// the #136-F2 shape. So the expiry names its condition (which identifies the
// owner exactly, for a human) and fails the SUITE via test_all_passed. A wrong
// accusation is worse than none: the genuine failure is already red on its own.
extern volatile unsigned    g_test_proc_wait_timeouts;
extern const char *volatile g_test_proc_wait_what;

#define TEST_YIELD_UNTIL_PROC(cond)                                         \
    do {                                                                    \
        u64  _yp_deadline = timer_now_ns() + TEST_YIELD_BUDGET_NS;          \
        bool _yp_spun = false, _yp_deep = false;                            \
        TEST_YIELD_BUMP(g_test_yield_calls);                                \
        while (!(cond)) {                                                   \
            if (_yp_spun) _yp_deep = true;                                  \
            _yp_spun = true;                                                \
            if (timer_now_ns() >= _yp_deadline) {                           \
                __atomic_store_n(&g_test_proc_wait_what,                    \
                                 "" #cond, __ATOMIC_RELEASE);               \
                __atomic_fetch_add(&g_test_proc_wait_timeouts, 1u,          \
                                   __ATOMIC_ACQ_REL);                       \
                exits("test-wait-timeout");                                 \
            }                                                               \
            sched();                                                        \
        }                                                                   \
        if (_yp_spun) TEST_YIELD_BUMP(g_test_yield_spun);                   \
        if (_yp_deep) TEST_YIELD_BUMP(g_test_yield_deep);                   \
    } while (0)

#endif // THYLACINE_KERNEL_TEST_H
