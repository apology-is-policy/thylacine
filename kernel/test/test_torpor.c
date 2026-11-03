// P6-pouch-wait-addr: SYS_TORPOR_WAIT / SYS_TORPOR_WAKE tests.
//
// Test layout:
//
//   - kproc-side (single-threaded) tests for paths that don't reach a
//     successful uaccess_load_u32: argument validation, EFAULT on an
//     unmapped user VA, WAKE on an empty bucket. The fault path refuses
//     to demand-page into kproc's TTBR0 (`p->as->pgtable_root == 0` short-
//     circuits userland_demand_page), so these tests cover the negative
//     paths only.
//
//   - Consumer-thread tests for paths that DO need the uaccess load to
//     succeed. The boot thread proc_alloc()s a fresh Proc, installs an
//     anon page in it via the burrow-attach API, then spawns a consumer
//     thread on that Proc. The consumer thread inherits the Proc's
//     TTBR0; uaccess_load_u32 demand-pages cleanly. (Two threads in one
//     Proc — a sibling-thread sharing the address space is the
//     pthread_create case sub-chunk 9 builds on; this test stands as
//     the v1.0 dress rehearsal.)
//
// Coverage:
//
//   torpor.wait_rejects_bad_args            — null Proc / null addr /
//                                              misaligned / above
//                                              USER_VA_TOP / timeout
//                                              over cap → -EINVAL
//   torpor.wait_rejects_unmapped_va         — valid user-VA shape, no
//                                              VMA → -EFAULT
//   torpor.wake_rejects_bad_args            — null Proc / bad addr
//                                              → -EINVAL
//   torpor.wake_empty_bucket_returns_zero   — no waiters → 0; count==0
//                                              → 0
//   torpor.wait_value_mismatch_fast_path    — consumer-thread test;
//                                              value != expected
//                                              → 0 without sleeping
//   torpor.wait_timeout_zero_returns_etimedout — consumer-thread test;
//                                              value == expected,
//                                              timeout_us == 0 → -ETIMEDOUT
//                                              via the tsleep
//                                              past-deadline-immediate path
//   torpor.wait_wake_handoff                — consumer-thread test;
//                                              full register-sleep-wake-
//                                              resume chain.

#include "test.h"

#include <thylacine/burrow.h>
#include <thylacine/exec.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>
#include <thylacine/smp.h>
#include <thylacine/thread.h>
#include <thylacine/torpor.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>

#include "../../arch/arm64/timer.h"
#include "../../arch/arm64/uaccess.h"
#include <thylacine/syscall.h>

// ---------------------------------------------------------------------------
// torpor.wait_rejects_bad_args
// ---------------------------------------------------------------------------

void test_torpor_wait_rejects_bad_args(void) {
    TEST_EXPECT_EQ(sys_torpor_wait_for_proc(NULL, 0x10000000ull, 0, -1),
                   (s64)TORPOR_ERR_EINVAL,
        "wait with NULL Proc rejected");
    TEST_EXPECT_EQ(sys_torpor_wait_for_proc(kproc(), 0, 0, -1),
                   (s64)TORPOR_ERR_EINVAL,
        "wait with addr_va == 0 rejected");
    TEST_EXPECT_EQ(sys_torpor_wait_for_proc(kproc(), 0x10000001ull, 0, -1),
                   (s64)TORPOR_ERR_EINVAL,
        "wait with 1-byte-misaligned addr rejected");
    TEST_EXPECT_EQ(sys_torpor_wait_for_proc(kproc(), 0x10000002ull, 0, -1),
                   (s64)TORPOR_ERR_EINVAL,
        "wait with 2-byte-misaligned addr rejected");
    TEST_EXPECT_EQ(sys_torpor_wait_for_proc(kproc(), UACCESS_USER_VA_TOP, 0, -1),
                   (s64)TORPOR_ERR_EINVAL,
        "wait with addr == USER_VA_TOP rejected");
    TEST_EXPECT_EQ(
        sys_torpor_wait_for_proc(kproc(), UACCESS_USER_VA_TOP - 1, 0, -1),
        (s64)TORPOR_ERR_EINVAL,
        "wait with addr straddling USER_VA_TOP rejected");
    TEST_EXPECT_EQ(
        sys_torpor_wait_for_proc(kproc(), 0x10000000ull, 0,
                                 (s64)(TORPOR_MAX_TIMEOUT_US + 1ull)),
        (s64)TORPOR_ERR_EINVAL,
        "wait with timeout above the cap rejected");
}

// ---------------------------------------------------------------------------
// torpor.wait_rejects_unmapped_va
// ---------------------------------------------------------------------------

void test_torpor_wait_rejects_unmapped_va(void) {
    // F5 (P3 audit): the EFAULT contract depends on this test running
    // on a kproc thread — the user-VA load walks current's TTBR0; for
    // EFAULT to bite, current must be kproc (pgtable_root == 0 short-
    // circuits userland_demand_page). Pin the precondition.
    TEST_ASSERT(current_thread() != NULL &&
                current_thread()->proc == kproc(),
        "torpor.wait_rejects_unmapped_va must run on a kproc thread");

    // 0x10000000 is a valid user-VA shape but kproc has no VMA covering
    // it. uaccess_load_u32 → fault → demand-page refuses (kproc's
    // pgtable_root == 0) → uaccess returns -1 → torpor maps to -EFAULT.
    // This also exercises the userland_demand_page kproc-refusal path.
    TEST_EXPECT_EQ(
        sys_torpor_wait_for_proc(kproc(), 0x10000000ull, 0, -1),
        (s64)TORPOR_ERR_EFAULT,
        "wait on an unmapped user VA returns EFAULT");
}

// ---------------------------------------------------------------------------
// torpor.wake_rejects_bad_args
// ---------------------------------------------------------------------------

void test_torpor_wake_rejects_bad_args(void) {
    TEST_EXPECT_EQ(sys_torpor_wake_for_proc(NULL, 0x10000000ull, 1),
                   (s64)TORPOR_ERR_EINVAL,
        "wake with NULL Proc rejected");
    TEST_EXPECT_EQ(sys_torpor_wake_for_proc(kproc(), 0, 1),
                   (s64)TORPOR_ERR_EINVAL,
        "wake with addr_va == 0 rejected");
    TEST_EXPECT_EQ(sys_torpor_wake_for_proc(kproc(), 0x10000001ull, 1),
                   (s64)TORPOR_ERR_EINVAL,
        "wake with misaligned addr rejected");
    TEST_EXPECT_EQ(sys_torpor_wake_for_proc(kproc(), UACCESS_USER_VA_TOP, 1),
                   (s64)TORPOR_ERR_EINVAL,
        "wake with addr == USER_VA_TOP rejected");
}

// ---------------------------------------------------------------------------
// torpor.wake_empty_bucket_returns_zero
// ---------------------------------------------------------------------------

void test_torpor_wake_empty_bucket_returns_zero(void) {
    // No waiters registered → walk finds nothing → return 0. count == 0
    // is a no-op (returns 0 without locking).
    TEST_EXPECT_EQ(sys_torpor_wake_for_proc(kproc(), 0x10000000ull, 100),
                   (s64)0,
        "wake on an empty bucket returns 0");
    TEST_EXPECT_EQ(sys_torpor_wake_for_proc(kproc(), 0x10000000ull, 0),
                   (s64)0,
        "wake with count == 0 returns 0");
}

// ---------------------------------------------------------------------------
// Consumer-thread fixture — shared by the uaccess-success tests below.
// ---------------------------------------------------------------------------
//
// Each test proc_allocs a fresh Proc, attaches an anon page in it
// (sys_burrow_attach_for_proc — same path pouch would use), creates a
// consumer thread on that Proc, and lets the boot thread interact with
// it. Boot's TTBR0 stays on kproc; the consumer's TTBR0 swaps to the
// fresh Proc's on its first cpu_switch_context — so the consumer's
// uaccess_load_u32 demand-pages through the fresh Proc's VMA list.
//
// Per-test globals (one consumer at a time; tests run serially):
//   g_torpor_proc      — the fresh Proc whose VMAs the consumer touches.
//   g_torpor_vaddr     — the attached user VA inside g_torpor_proc.
//   g_torpor_expected  — the value the consumer passes as `expected`.
//   g_torpor_timeout   — the timeout the consumer passes (µs / -1).
//   g_torpor_done      — consumer's progress beacon (boot reads).
//   g_torpor_wait_rc   — return code the consumer's WAIT produced.

static struct Proc  *g_torpor_proc;
static u64           g_torpor_vaddr;
static u32           g_torpor_expected;
static s64           g_torpor_timeout;
static volatile u32  g_torpor_done;
static volatile s64  g_torpor_wait_rc;
static volatile bool g_torpor_exited;   // #109: terminal-park reap handshake

static void torpor_consumer_entry(void) {
    g_torpor_done++;       // → 1: pre-wait
    g_torpor_wait_rc = sys_torpor_wait_for_proc(g_torpor_proc,
                                                g_torpor_vaddr,
                                                g_torpor_expected,
                                                g_torpor_timeout);
    g_torpor_done++;       // → 2: post-wait
    // #109: terminal EXITING park (was for(;;)sched()); reaped by the teardown.
    test_thread_park_terminal(&g_torpor_exited);
}

// Set up the per-test consumer fixture. Returns 0 on success, -1 on a
// setup failure (caller's TEST_ASSERT short-circuits the test then).
static int torpor_consumer_setup(u32 expected, s64 timeout_us) {
    g_torpor_proc      = NULL;
    g_torpor_vaddr     = 0;
    g_torpor_expected  = expected;
    g_torpor_timeout   = timeout_us;
    g_torpor_done      = 0;
    g_torpor_exited    = false;
    g_torpor_wait_rc   = (s64)-999;

    g_torpor_proc = proc_alloc();
    if (!g_torpor_proc) return -1;

    s64 r = sys_burrow_attach_for_proc(g_torpor_proc, PAGE_SIZE);
    if (r <= 0) {
        test_proc_free_drained(g_torpor_proc);
        g_torpor_proc = NULL;
        return -1;
    }
    g_torpor_vaddr = (u64)r;
    return 0;
}

// Tear down: wait for the consumer thread to be freed, then free the proc
// (which drains its VMAs). Caller passes the consumer thread; safe to pass NULL.
static void torpor_consumer_teardown(struct Thread *consumer) {
    if (consumer) test_thread_join_last(g_torpor_proc, &g_torpor_exited);
    if (g_torpor_proc) {
        test_proc_free_drained(g_torpor_proc);
        g_torpor_proc = NULL;
    }
}

// ---------------------------------------------------------------------------
// torpor.wait_value_mismatch_fast_path
// ---------------------------------------------------------------------------
//
// Consumer's WAIT compares the (demand-zero anon) page word against
// `expected = 0xDEADBEEF`; mismatch → returns 0 immediately without
// sleeping. The consumer runs to completion in a single sched() yield.

void test_torpor_wait_value_mismatch_fast_path(void) {
    TEST_EXPECT_EQ(torpor_consumer_setup(/*expected=*/0xDEADBEEFu,
                                         /*timeout_us=*/-1),
                   0, "consumer fixture setup");

    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree must be empty at test entry");

    struct Thread *consumer = thread_create(g_torpor_proc,
                                            torpor_consumer_entry);
    TEST_ASSERT(consumer != NULL, "thread_create(consumer) failed");
    ready(consumer);

    // Consumer runs, takes the bucket's lock, loads the user VA (demand-zero,
    // reads 0), 0 != 0xDEADBEEF → fast path returns 0 without sleeping.
    // SMP (#20): ready() may place the consumer on a peer CPU's runqueue,
    // so one local sched() does not guarantee it ran — yield until it
    // completes. Bounded (#134): an unbounded spin here hangs the boot with
    // no message if the consumer never completes.
    TEST_YIELD_UNTIL(g_torpor_done >= 2u);

    TEST_EXPECT_EQ(g_torpor_done, 2u,
        "consumer must have completed (no sleep)");
    TEST_EXPECT_EQ(g_torpor_wait_rc, (s64)TORPOR_OK,
        "value-mismatch fast path returns 0");

    torpor_consumer_teardown(consumer);
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree empty after consumer freed");
}

// ---------------------------------------------------------------------------
// torpor.wait_timeout_zero_returns_etimedout
// ---------------------------------------------------------------------------
//
// Consumer's WAIT compares value (0) against expected (0) — match;
// timeout_us == 0 → deadline lapses at the tsleep-entry past-deadline
// check; tsleep returns TIMEDOUT; torpor maps to -ETIMEDOUT. No actual
// sleep happens.

void test_torpor_wait_timeout_zero_returns_etimedout(void) {
    TEST_EXPECT_EQ(torpor_consumer_setup(/*expected=*/0u, /*timeout_us=*/0),
                   0, "consumer fixture setup");

    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree must be empty at test entry");

    struct Thread *consumer = thread_create(g_torpor_proc,
                                            torpor_consumer_entry);
    TEST_ASSERT(consumer != NULL, "thread_create(consumer) failed");
    ready(consumer);

    // SMP (#20): yield until the consumer completes — see
    // wait_value_mismatch_fast_path.
    TEST_YIELD_UNTIL(g_torpor_done >= 2u);

    TEST_EXPECT_EQ(g_torpor_done, 2u,
        "consumer must have completed (no real sleep)");
    TEST_EXPECT_EQ(g_torpor_wait_rc, (s64)TORPOR_ERR_ETIMEDOUT,
        "timeout_us == 0 returns ETIMEDOUT");

    torpor_consumer_teardown(consumer);
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree empty after consumer freed");
}

// ---------------------------------------------------------------------------
// torpor.wait_wake_handoff
// ---------------------------------------------------------------------------
//
// End-to-end register-sleep-wake-resume chain. Consumer thread on the
// fresh Proc tsleeps inside torpor_wait with a far deadline; boot thread
// WAKEs via sys_torpor_wake_for_proc. The wake walks the bucket, finds
// the consumer's registered waiter, sets awoken + calls wakeup on the
// private rendez. Consumer returns TORPOR_OK.

void test_torpor_wait_wake_handoff(void) {
    TEST_EXPECT_EQ(torpor_consumer_setup(
        /*expected=*/0u,
        /*timeout_us=*/60ll * 1000000ll),   // 60 s — wake wins this race
                   0, "consumer fixture setup");

    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree must be empty at test entry");

    struct Thread *consumer = thread_create(g_torpor_proc,
                                            torpor_consumer_entry);
    TEST_ASSERT(consumer != NULL, "thread_create(consumer) failed");
    int ctid = consumer->tid;
    ready(consumer);

    // Consumer: runs, increments to 1, takes the bucket's lock, loads (0 == 0
    // match), registers in the bucket, tsleeps with the far deadline →
    // SLEEPING. SMP (#20): it may run on a peer CPU — yield until it is
    // observably SLEEPING (registration precedes the SLEEPING
    // transition, so SLEEPING implies registered).
    TEST_YIELD_UNTIL(test_thread_sleeping(g_torpor_proc, ctid));

    TEST_EXPECT_EQ(g_torpor_done, 1u,
        "consumer must have run once before sleeping");
    TEST_ASSERT(test_thread_sleeping(g_torpor_proc, ctid),
        "consumer must be SLEEPING inside torpor_wait");

    // WAKE: walks the bucket, finds the consumer's waiter, sets
    // awoken + wakeup(rendez). Consumer transitions RUNNABLE.
    s64 wake_rc = sys_torpor_wake_for_proc(g_torpor_proc, g_torpor_vaddr, 1);
    TEST_EXPECT_EQ(wake_rc, (s64)1, "wake reports one waiter woken");

    // Consumer resumes inside tsleep (cond true now), returns
    // TSLEEP_AWOKEN; torpor_wait unlinks the waiter + returns TORPOR_OK;
    // consumer increments to 2 and parks. SMP (#20): it may resume on a
    // peer CPU, so a RUNNABLE point-check here is unsound (it can
    // already be RUNNING or parked) — completion is the sound witness.
    TEST_YIELD_UNTIL(g_torpor_done >= 2u);

    TEST_EXPECT_EQ(g_torpor_done, 2u,
        "consumer must have completed wait and parked");
    TEST_EXPECT_EQ(g_torpor_wait_rc, (s64)TORPOR_OK,
        "torpor_wait must return TORPOR_OK after the wake");

    torpor_consumer_teardown(consumer);
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree empty after consumer freed");
}

// ---------------------------------------------------------------------------
// torpor.wake_two_waiters_count_bound — F9 (P3 audit) coverage.
// ---------------------------------------------------------------------------
//
// Two consumer threads on the same Proc sleep on the same `(Proc, addr_va)`.
// First WAKE(count=1) transitions exactly one — F1 (P2 audit): wakeup()'s
// return value is now the count source, so the count is precise. The
// other consumer stays SLEEPING. A second WAKE(count=1) transitions the
// other. Both return TORPOR_OK on their own resume.
//
// Verifies: (i) the bucket walk visits multiple waiters on the same key;
// (ii) the `count` bound stops the walk; (iii) WAKE() wakes exactly N
// matched (not "N matched-and-skipped-because-awoken"); (iv) the
// `awoken == 0` filter prevents re-waking on the subsequent call.

static volatile u32  g_torpor_done_a;
static volatile u32  g_torpor_done_b;
static volatile s64  g_torpor_rc_a;
static volatile s64  g_torpor_rc_b;
static volatile bool g_torpor_exited_a;  // #109: terminal-park reap handshake (consumer A)
static volatile bool g_torpor_exited_b;  // #109: terminal-park reap handshake (consumer B)

static void torpor_consumer_a_entry(void) {
    g_torpor_done_a++;
    g_torpor_rc_a = sys_torpor_wait_for_proc(g_torpor_proc, g_torpor_vaddr,
                                             /*expected=*/0,
                                             /*timeout_us=*/60ll * 1000000ll);
    g_torpor_done_a++;
    test_thread_park_terminal(&g_torpor_exited_a);  // #109: EXITING park
}

static void torpor_consumer_b_entry(void) {
    g_torpor_done_b++;
    g_torpor_rc_b = sys_torpor_wait_for_proc(g_torpor_proc, g_torpor_vaddr,
                                             /*expected=*/0,
                                             /*timeout_us=*/60ll * 1000000ll);
    g_torpor_done_b++;
    test_thread_park_terminal(&g_torpor_exited_b);  // #109: EXITING park
}

void test_torpor_wake_two_waiters_count_bound(void) {
    g_torpor_done_a = 0; g_torpor_rc_a = (s64)-999; g_torpor_exited_a = false;
    g_torpor_done_b = 0; g_torpor_rc_b = (s64)-999; g_torpor_exited_b = false;
    g_torpor_proc   = NULL;
    g_torpor_vaddr  = 0;

    g_torpor_proc = proc_alloc();
    TEST_ASSERT(g_torpor_proc != NULL, "proc_alloc failed");
    s64 r = sys_burrow_attach_for_proc(g_torpor_proc, PAGE_SIZE);
    TEST_ASSERT(r > 0, "burrow_attach failed");
    g_torpor_vaddr = (u64)r;

    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree must be empty at test entry");

    struct Thread *ca = thread_create(g_torpor_proc, torpor_consumer_a_entry);
    struct Thread *cb = thread_create(g_torpor_proc, torpor_consumer_b_entry);
    TEST_ASSERT(ca != NULL && cb != NULL, "thread_create failed");
    int atid = ca->tid, btid = cb->tid;
    ready(ca);
    ready(cb);

    // Both consumers run and end up SLEEPING in the bucket. SMP (#20):
    // ready() may place a consumer on a peer CPU's runqueue, so N local
    // sched() calls do NOT guarantee N consumer runs (the observed
    // 1-in-10 ubsan-smp4 failure: B runnable on cpu3, not yet
    // dispatched when boot asserted after two sched() calls). Yield
    // until both are observably SLEEPING — registration precedes the
    // SLEEPING transition, so this also guarantees both are in the
    // bucket before the first WAKE (wake1 == 1 stays deterministic).
    TEST_YIELD_UNTIL(test_thread_sleeping(g_torpor_proc, atid) &&
                     test_thread_sleeping(g_torpor_proc, btid));

    TEST_EXPECT_EQ(g_torpor_done_a, 1u, "consumer A reached pre-wait");
    TEST_EXPECT_EQ(g_torpor_done_b, 1u, "consumer B reached pre-wait");
    TEST_EXPECT_EQ(ca->state, THREAD_SLEEPING, "A must be SLEEPING");
    TEST_EXPECT_EQ(cb->state, THREAD_SLEEPING, "B must be SLEEPING");

    // First WAKE(count=1) wakes exactly ONE. They raced to the bucket, so
    // which is not known here; torpor.wake_takes_the_oldest orders them.
    // SMP (#20): the woken consumer may resume on a peer CPU, so a
    // RUNNABLE point-check right here is unsound (it can already be
    // RUNNING or parked) — the sound partition is over COMPLETION:
    // yield until exactly one has finished its wait; the other must
    // still be waiting (no spurious wakes; only wake2 can release it).
    s64 wake1 = sys_torpor_wake_for_proc(g_torpor_proc, g_torpor_vaddr, 1);
    TEST_EXPECT_EQ(wake1, (s64)1,
        "first WAKE(count=1) reports exactly one woken");

    // De Morgan: `while (a < 2 && b < 2)` waits until EITHER completes.
    TEST_YIELD_UNTIL(g_torpor_done_a >= 2u || g_torpor_done_b >= 2u);

    int a_woke_first = (g_torpor_done_a == 2u);
    TEST_ASSERT((g_torpor_done_a == 2u) != (g_torpor_done_b == 2u),
        "exactly one of {A, B} completes after first WAKE");

    if (a_woke_first) {
        TEST_EXPECT_EQ(g_torpor_rc_a, (s64)TORPOR_OK, "A returned OK");
        TEST_EXPECT_EQ(g_torpor_done_b, 1u, "B is still waiting");
        TEST_EXPECT_EQ(cb->state, THREAD_SLEEPING, "B remains SLEEPING");
    } else {
        TEST_EXPECT_EQ(g_torpor_rc_b, (s64)TORPOR_OK, "B returned OK");
        TEST_EXPECT_EQ(g_torpor_done_a, 1u, "A is still waiting");
        TEST_EXPECT_EQ(ca->state, THREAD_SLEEPING, "A remains SLEEPING");
    }

    // Second WAKE(count=1) wakes the remaining consumer (deterministic:
    // the survivor was just asserted SLEEPING + registered; its 60 s
    // deadline cannot lapse).
    s64 wake2 = sys_torpor_wake_for_proc(g_torpor_proc, g_torpor_vaddr, 1);
    TEST_EXPECT_EQ(wake2, (s64)1,
        "second WAKE(count=1) reports exactly one woken");

    // SMP (#20): yield until both have completed. De Morgan: `while (a < 2 ||
    // b < 2)` waits until BOTH are done -- the opposite of the wait above.
    TEST_YIELD_UNTIL(g_torpor_done_a >= 2u && g_torpor_done_b >= 2u);

    TEST_EXPECT_EQ(g_torpor_rc_a, (s64)TORPOR_OK, "A returned OK overall");
    TEST_EXPECT_EQ(g_torpor_rc_b, (s64)TORPOR_OK, "B returned OK overall");

    // Third WAKE — bucket emptied — returns 0.
    s64 wake3 = sys_torpor_wake_for_proc(g_torpor_proc, g_torpor_vaddr, 100);
    TEST_EXPECT_EQ(wake3, (s64)0,
        "third WAKE on emptied bucket returns 0");

    TEST_YIELD_UNTIL(__atomic_load_n(&g_torpor_exited_a, __ATOMIC_ACQUIRE));
    test_thread_join_last(g_torpor_proc, &g_torpor_exited_b);
    test_proc_free_drained(g_torpor_proc);
    g_torpor_proc = NULL;
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree empty after both consumers freed");
}

// ---------------------------------------------------------------------------
// torpor.caught_note_ends_wait -- signal(7)'s list (ARCH 8.8.3)
// ---------------------------------------------------------------------------
//
// futex WAIT is a listed call. A caught note ends a Linux waiter's WAIT with
// TORPOR_ERR_EINTR, distinct from the WAKE's TORPOR_OK; a native waiter rides
// it out and ends on the WAKE.

static struct Proc *g_ctw_proc;
static u64          g_ctw_vaddr;

static long ctw_wait(void *arg) {
    (void)arg;
    // The demand-zero word reads 0 == expected, so the WAIT sleeps.
    return (long)sys_torpor_wait_for_proc(current_thread()->proc, g_ctw_vaddr, 0u, -1);
}

static void ctw_wake(void *arg) {
    (void)arg;
    (void)sys_torpor_wake_for_proc(g_ctw_proc, g_ctw_vaddr, 1u);
}

static struct test_caught_leg ctw_leg(bool linux_pheno) {
    struct Proc *p  = test_caught_proc(linux_pheno);
    s64          va = p ? sys_burrow_attach_for_proc(p, PAGE_SIZE) : -1;
    g_ctw_proc  = p;
    g_ctw_vaddr = va > 0 ? (u64)va : 0;
    struct test_caught_leg leg = test_caught_run(va > 0 ? p : NULL, ctw_wait, NULL,
                                                 ctw_wake, NULL, false);
    test_caught_proc_free(p, &leg);
    return leg;
}

void test_torpor_caught_note_ends_wait(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct test_caught_leg leg = ctw_leg(true);
    struct test_caught_leg ctl = ctw_leg(false);

    TEST_ASSERT(leg.parked && leg.posted && leg.joined,
        "the Linux waiter slept on the word, the note posted, the WAIT returned");
    TEST_ASSERT(leg.on_post, "a caught note ends a Linux futex WAIT (ARCH 8.8.3)");
    TEST_EXPECT_EQ(leg.rc, (long)TORPOR_ERR_EINTR,
        "TORPOR_ERR_EINTR, not the WAKE's TORPOR_OK");
    TEST_ASSERT(ctl.parked && ctl.posted && ctl.joined,
        "control: the native waiter slept, the note posted, the WAIT returned");
    TEST_ASSERT(ctl.rode_out, "control: woken with the note pending, the native waiter slept again");
    TEST_EXPECT_EQ(ctl.rc, (long)TORPOR_OK, "control: the native WAIT ends on the WAKE");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after cleanup");
}

// ---------------------------------------------------------------------------
// C8: the order, the key, the count and the record's lifetime.
// ---------------------------------------------------------------------------

#define TQ_WAITERS 3u
static struct Thread *g_tq_t[TQ_WAITERS];
static volatile u32   g_tq_ticket;
static volatile u32   g_tq_done[TQ_WAITERS];
static volatile s64   g_tq_rc[TQ_WAITERS];
static volatile bool  g_tq_exited[TQ_WAITERS];
static volatile u64   g_tq_va;
static volatile s64   g_tq_timeout_us;

static void tq_entry(void) {
    u32 me = __atomic_fetch_add(&g_tq_ticket, 1u, __ATOMIC_ACQ_REL);
    g_tq_rc[me] = sys_torpor_wait_for_proc(current_thread()->proc, g_tq_va, 0, g_tq_timeout_us);
    __atomic_store_n(&g_tq_done[me], 1u, __ATOMIC_RELEASE);
    test_thread_park_terminal(&g_tq_exited[me]);
}

static void tq_reset(u64 va, s64 timeout_us) {
    g_tq_ticket = 0; g_tq_va = va; g_tq_timeout_us = timeout_us;
    for (u32 i = 0; i < TQ_WAITERS; i++) {
        g_tq_t[i] = NULL; g_tq_done[i] = 0; g_tq_rc[i] = (s64)-999; g_tq_exited[i] = false;
    }
}

// Start waiter `i` in `p` and let it reach the bucket before the next one starts.
static bool tq_start(struct Proc *p, u32 i) {
    g_tq_t[i] = thread_create(p, tq_entry);
    if (!g_tq_t[i]) return false;
    int tid = g_tq_t[i]->tid;
    ready(g_tq_t[i]);
    u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
    while (!test_thread_sleeping(p, tid) && timer_now_ns() < dl) sched();
    return test_thread_sleeping(p, tid);
}

static bool tq_done(u32 i) { return __atomic_load_n(&g_tq_done[i], __ATOMIC_ACQUIRE) != 0; }

static bool tq_finished(u32 i) {
    u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
    while (!tq_done(i) && timer_now_ns() < dl) sched();
    return tq_done(i);
}

// A wake takes the oldest waiter, so a mutex's first waiter cannot starve (F-9).
void test_torpor_wake_takes_the_oldest(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    s64 r = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    TEST_ASSERT(r > 0, "burrow_attach failed");
    tq_reset((u64)r, 60ll * 1000000ll);

    bool started = true;
    for (u32 i = 0; i < TQ_WAITERS; i++) started = tq_start(p, i) && started;

    s64  wake[TQ_WAITERS];
    bool order = started;
    for (u32 i = 0; i < TQ_WAITERS; i++) {
        wake[i] = sys_torpor_wake_for_proc(p, g_tq_va, 1);
        order = tq_finished(i) && order;
        for (u32 j = i + 1u; j < TQ_WAITERS; j++) order = !tq_done(j) && order;
    }
    (void)sys_torpor_wake_for_proc(p, g_tq_va, (u32)~0u);
    for (u32 i = 0; i < TQ_WAITERS; i++)
        if (g_tq_t[i]) test_thread_join_last(p, &g_tq_exited[i]);
    test_proc_free_drained(p);

    TEST_ASSERT(started, "the three waiters reached the bucket in turn");
    TEST_EXPECT_EQ(wake[0], (s64)1, "the first wake woke one");
    TEST_EXPECT_EQ(wake[2], (s64)1, "and so did the last");
    TEST_ASSERT(order, "each wake took the oldest, and left the younger waiting");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after cleanup");
}

// The key is the space, not the Proc (F-10): a sharer wakes it, and a stranger at the
// same address does not. Sharing as rfork(RFPROC|RFMEM) does: the same AddrSpace, ref'd.
static void tq_share_space(struct Proc *q, struct Proc *with) {
    addrspace_unref(q->as);
    addrspace_ref(with->as);
    q->as = with->as;
}

void test_torpor_key_is_the_address_space(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = proc_alloc(), *sharer = proc_alloc(), *stranger = proc_alloc();
    TEST_ASSERT(p && sharer && stranger, "proc_alloc failed");
    s64 r = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    TEST_ASSERT(r > 0, "burrow_attach failed");
    tq_share_space(sharer, p);
    tq_reset((u64)r, 60ll * 1000000ll);

    bool started  = tq_start(p, 0);
    s64  strange  = sys_torpor_wake_for_proc(stranger, g_tq_va, 1);
    bool stood    = !tq_done(0);
    s64  shared   = sys_torpor_wake_for_proc(sharer, g_tq_va, 1);
    bool finished = tq_finished(0);
    s64  rc       = g_tq_rc[0];

    (void)sys_torpor_wake_for_proc(p, g_tq_va, (u32)~0u);
    if (g_tq_t[0]) test_thread_join_last(p, &g_tq_exited[0]);
    test_proc_free_drained(p);
    test_proc_free_drained(sharer);
    test_proc_free_drained(stranger);

    TEST_ASSERT(started, "the waiter reached the bucket");
    TEST_EXPECT_EQ(strange, (s64)0, "a Proc with its own space wakes nobody");
    TEST_ASSERT(stood, "and the waiter stands");
    TEST_EXPECT_EQ(shared, (s64)1, "a Proc sharing the space wakes it");
    TEST_ASSERT(finished, "and its wait ends");
    TEST_EXPECT_EQ(rc, (s64)TORPOR_OK, "as a wake");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after cleanup");
}

// A wake counts only a waiter its mark found asleep, and goes on past one it did not.
// A stopped waiter stays in the bucket and is not on its rendez, which makes it the
// deterministic case of a waiter past its sleep. Its Proc is stopped and a sharer's
// waiter is younger, so a wake(1) that counted the first mark would strand the second.
static struct Proc *volatile g_tqs_proc;
static volatile u64  g_tqs_va;
static volatile u32  g_tqs_done;
static volatile s64  g_tqs_rc;

static void tqs_stopped_thunk(void *arg) {
    (void)arg;
    struct Proc *self = current_thread()->proc;
    if (proc_setpgid(self, 0, 0) != 0) exits("setpgid failed");
    s64 va = sys_burrow_attach_for_proc(self, PAGE_SIZE);
    if (va <= 0) exits("burrow_attach failed");
    __atomic_store_n(&g_tqs_va, (u64)va, __ATOMIC_RELEASE);
    __atomic_store_n(&g_tqs_proc, self, __ATOMIC_RELEASE);
    g_tqs_rc = sys_torpor_wait_for_proc(self, (u64)va, 0, 60ll * 1000000ll);
    __atomic_store_n(&g_tqs_done, 1u, __ATOMIC_RELEASE);
    exits("ok");
}

void test_torpor_wake_counts_only_a_sleeper(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    g_tqs_proc = NULL; g_tqs_va = 0; g_tqs_done = 0; g_tqs_rc = (s64)-999;

    int pid = rfork(RFPROC, tqs_stopped_thunk, NULL);
    TEST_ASSERT(pid > 0, "rfork failed");
    struct Proc *stopped;
    TEST_YIELD_UNTIL((stopped = __atomic_load_n(&g_tqs_proc, __ATOMIC_ACQUIRE)) != NULL);
    irq_state_t ts = proc_threads_lock(stopped);
    struct Thread *th = stopped->threads;
    int stid = th ? th->tid : 0;
    proc_threads_unlock(stopped, ts);
    TEST_YIELD_UNTIL(test_thread_sleeping(stopped, stid));

    TEST_EXPECT_EQ(proc_job_stop_pgrp((u32)pid), 1, "the fan stops the first waiter's group");
    bool parked = false;
    u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
    while (!parked && timer_now_ns() < dl) {
        irq_state_t ws = spin_lock_irqsave(&th->wait_lock);
        parked = th->rendez_blocked_on == &th->debug_rendez;
        spin_unlock_irqrestore(&th->wait_lock, ws);
        if (!parked) sched();
    }

    struct Proc *sharer = proc_alloc();
    TEST_ASSERT(sharer != NULL, "proc_alloc failed");
    tq_share_space(sharer, stopped);
    tq_reset(g_tqs_va, 60ll * 1000000ll);
    bool started = tq_start(sharer, 0);

    s64  woken    = sys_torpor_wake_for_proc(sharer, g_tqs_va, 1);
    bool finished = tq_finished(0);
    s64  again    = sys_torpor_wake_for_proc(sharer, g_tqs_va, (u32)~0u);

    TEST_EXPECT_EQ(proc_job_cont_pgrp((u32)pid), 1, "cont the group");
    TEST_YIELD_UNTIL(__atomic_load_n(&g_tqs_done, __ATOMIC_ACQUIRE) != 0u);
    s64 stopped_rc = g_tqs_rc;
    int st = -42;
    int reaped = wait_pid_for(pid, 0, &st);
    if (g_tq_t[0]) test_thread_join_last(sharer, &g_tq_exited[0]);
    test_proc_free_drained(sharer);

    TEST_ASSERT(parked, "the first waiter is parked for the stop");
    TEST_ASSERT(started, "the younger waiter reached the bucket");
    TEST_EXPECT_EQ(woken, (s64)1, "the wake counted the sleeper alone");
    TEST_ASSERT(finished, "and the sleeper's wait ended: the stopped waiter did not use it up");
    TEST_EXPECT_EQ(again, (s64)0, "both had left the bucket");
    TEST_EXPECT_EQ(stopped_rc, (s64)TORPOR_OK, "the stopped waiter read its mark on resume");
    TEST_EXPECT_EQ(reaped, pid, "reap the stopped waiter");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after cleanup");
}

// A wake's batch holds a stack record after it has dropped the lock. The waiter's
// deadline fires inside that window, so it runs, and must not leave while the wake can
// still reach its frame. Meanwhile the bucket is free: E-7's point. A wake from here
// comes straight back, where a wake done under the lock would hold it until the stall
// ends. Needs a second CPU, as the wake stays non-preemptible.
static volatile s64  g_tqw_woken;
static volatile bool g_tqw_exited;
static struct Proc  *g_tqw_proc;

static void tqw_entry(void) {
    g_tqw_woken = sys_torpor_wake_for_proc(g_tqw_proc, g_tq_va, 1);
    test_thread_park_terminal(&g_tqw_exited);
}

void test_torpor_record_outlives_the_wake(void) {
    if (smp_cpu_count() < 2u) return;
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    s64 r = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    TEST_ASSERT(r > 0, "burrow_attach failed");
    tq_reset((u64)r, 200000);           // 200 ms, inside the stall's budget
    g_tqw_proc = p; g_tqw_woken = (s64)-999; g_tqw_exited = false;

    bool started = tq_start(p, 0);
    torpor_stall_wakes_for_test(true);
    struct Thread *w = thread_create(p, tqw_entry);
    bool stalled = false;
    if (w) {
        ready(w);
        u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
        while (!(stalled = torpor_wake_stalled_for_test()) && timer_now_ns() < dl) sched();
    }

    // Past the waiter's deadline: it has been woken and has run by now, and it waits.
    u64 until = timer_now_ns() + 500000000ull;
    bool left = false;
    while (timer_now_ns() < until && !left) {
        left = tq_done(0);
        sched();
    }

    // The bucket is free while the wake is stalled.
    u64 t0 = timer_now_ns();
    s64 peer = sys_torpor_wake_for_proc(p, g_tq_va, 1);
    u64 peer_ns = timer_now_ns() - t0;
    bool still_stalled = torpor_wake_stalled_for_test();

    torpor_stall_wakes_for_test(false);
    bool finished = tq_finished(0);
    u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
    while (g_tqw_woken == (s64)-999 && timer_now_ns() < dl) sched();
    s64 woken = g_tqw_woken, rc = g_tq_rc[0];

    if (w) test_thread_join_last(p, &g_tqw_exited);
    if (g_tq_t[0]) test_thread_join_last(p, &g_tq_exited[0]);
    test_proc_free_drained(p);

    TEST_ASSERT(started, "the waiter reached the bucket");
    TEST_ASSERT(stalled, "the wake reached its stall");
    TEST_ASSERT(!left, "the waiter did not leave while the wake held its record");
    TEST_ASSERT(still_stalled, "the stall lasted past the peer's wake");
    TEST_EXPECT_EQ(peer, (s64)0, "the peer's wake found nobody: the waiter had left the bucket");
    TEST_ASSERT(peer_ns < 1000000000ull, "and it came straight back: no wake holds the lock");
    TEST_ASSERT(finished, "lifting the stall let the waiter go");
    TEST_EXPECT_EQ(woken, (s64)1, "the wake counted it");
    TEST_EXPECT_EQ(rc, (s64)TORPOR_OK, "and the waiter read the mark, deadline or no");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after cleanup");
}
