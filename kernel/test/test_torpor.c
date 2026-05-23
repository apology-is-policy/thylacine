// P6-pouch-wait-addr: SYS_TORPOR_WAIT / SYS_TORPOR_WAKE tests.
//
// Test layout:
//
//   - kproc-side (single-threaded) tests for paths that don't reach a
//     successful uaccess_load_u32: argument validation, EFAULT on an
//     unmapped user VA, WAKE on an empty bucket. The fault path refuses
//     to demand-page into kproc's TTBR0 (`p->pgtable_root == 0` short-
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
#include <thylacine/thread.h>
#include <thylacine/torpor.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>

#include "../../arch/arm64/timer.h"
#include "../../arch/arm64/uaccess.h"

void test_torpor_wait_rejects_bad_args(void);
void test_torpor_wait_rejects_unmapped_va(void);
void test_torpor_wake_rejects_bad_args(void);
void test_torpor_wake_empty_bucket_returns_zero(void);
void test_torpor_wait_value_mismatch_fast_path(void);
void test_torpor_wait_timeout_zero_returns_etimedout(void);
void test_torpor_wait_wake_handoff(void);
void test_torpor_wake_two_waiters_count_bound(void);

// The non-static inners of the SVC handlers (defined in kernel/syscall.c /
// kernel/torpor.c).
extern s64 sys_burrow_attach_for_proc(struct Proc *p, u64 length_raw);
extern s64 sys_burrow_detach_for_proc(struct Proc *p, u64 vaddr_raw,
                                      u64 length_raw);

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

static void torpor_consumer_entry(void) {
    g_torpor_done++;       // → 1: pre-wait
    g_torpor_wait_rc = sys_torpor_wait_for_proc(g_torpor_proc,
                                                g_torpor_vaddr,
                                                g_torpor_expected,
                                                g_torpor_timeout);
    g_torpor_done++;       // → 2: post-wait
    // Park forever — a return from entry WFE-halts the trampoline.
    for (;;) sched();
}

// Set up the per-test consumer fixture. Returns 0 on success, -1 on a
// setup failure (caller's TEST_ASSERT short-circuits the test then).
static int torpor_consumer_setup(u32 expected, s64 timeout_us) {
    g_torpor_proc      = NULL;
    g_torpor_vaddr     = 0;
    g_torpor_expected  = expected;
    g_torpor_timeout   = timeout_us;
    g_torpor_done      = 0;
    g_torpor_wait_rc   = (s64)-999;

    g_torpor_proc = proc_alloc();
    if (!g_torpor_proc) return -1;

    s64 r = sys_burrow_attach_for_proc(g_torpor_proc, PAGE_SIZE);
    if (r <= 0) {
        g_torpor_proc->state = 2;            // ZOMBIE
        proc_free(g_torpor_proc);
        g_torpor_proc = NULL;
        return -1;
    }
    g_torpor_vaddr = (u64)r;
    return 0;
}

// Tear down: free the consumer thread, then free the proc (which drains
// its VMAs). Caller passes the consumer thread; safe to pass NULL.
static void torpor_consumer_teardown(struct Thread *consumer) {
    if (consumer) thread_free(consumer);
    if (g_torpor_proc) {
        g_torpor_proc->state = 2;            // ZOMBIE
        proc_free(g_torpor_proc);
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

    // Yield: consumer runs, takes torpor_lock, loads the user VA
    // (demand-zero, reads 0), 0 != 0xDEADBEEF → fast path returns 0
    // without sleeping. Consumer parks. Boot resumes.
    sched();

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
    sched();

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
    ready(consumer);

    // Yield to consumer: runs, increments to 1, takes torpor_lock,
    // loads (0 == 0 match), registers in the bucket, tsleeps with the
    // far deadline → SLEEPING. sched returns boot here.
    sched();

    TEST_EXPECT_EQ(g_torpor_done, 1u,
        "consumer must have run once before sleeping");
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer must be SLEEPING inside torpor_wait");

    // WAKE: walks the bucket, finds the consumer's waiter, sets
    // awoken + wakeup(rendez). Consumer transitions RUNNABLE.
    s64 wake_rc = sys_torpor_wake_for_proc(g_torpor_proc, g_torpor_vaddr, 1);
    TEST_EXPECT_EQ(wake_rc, (s64)1, "wake reports one waiter woken");
    TEST_EXPECT_EQ(consumer->state, THREAD_RUNNABLE,
        "consumer must be RUNNABLE after wake");

    // Yield: consumer resumes inside tsleep (cond true now), returns
    // TSLEEP_AWOKEN. torpor_wait unlinks the waiter + returns
    // TORPOR_OK. Consumer increments to 2 and parks.
    sched();

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

static void torpor_consumer_a_entry(void) {
    g_torpor_done_a++;
    g_torpor_rc_a = sys_torpor_wait_for_proc(g_torpor_proc, g_torpor_vaddr,
                                             /*expected=*/0,
                                             /*timeout_us=*/60ll * 1000000ll);
    g_torpor_done_a++;
    for (;;) sched();
}

static void torpor_consumer_b_entry(void) {
    g_torpor_done_b++;
    g_torpor_rc_b = sys_torpor_wait_for_proc(g_torpor_proc, g_torpor_vaddr,
                                             /*expected=*/0,
                                             /*timeout_us=*/60ll * 1000000ll);
    g_torpor_done_b++;
    for (;;) sched();
}

void test_torpor_wake_two_waiters_count_bound(void) {
    g_torpor_done_a = 0; g_torpor_rc_a = (s64)-999;
    g_torpor_done_b = 0; g_torpor_rc_b = (s64)-999;
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
    ready(ca);
    ready(cb);

    // Yield: both consumers run (one per sched()) and end up SLEEPING in
    // the bucket. After two yields control returns to boot with both
    // consumers in THREAD_SLEEPING.
    sched();
    sched();

    TEST_EXPECT_EQ(g_torpor_done_a, 1u, "consumer A reached pre-wait");
    TEST_EXPECT_EQ(g_torpor_done_b, 1u, "consumer B reached pre-wait");
    TEST_EXPECT_EQ(ca->state, THREAD_SLEEPING, "A must be SLEEPING");
    TEST_EXPECT_EQ(cb->state, THREAD_SLEEPING, "B must be SLEEPING");

    // First WAKE(count=1) wakes exactly ONE. Order is bucket-walk order
    // (most-recently-inserted first), but we don't depend on which:
    // assert the count + the partition "exactly one of {A, B} is now
    // RUNNABLE, the other is still SLEEPING".
    s64 wake1 = sys_torpor_wake_for_proc(g_torpor_proc, g_torpor_vaddr, 1);
    TEST_EXPECT_EQ(wake1, (s64)1,
        "first WAKE(count=1) reports exactly one woken");
    int a_woke_first = (ca->state == THREAD_RUNNABLE);
    int b_woke_first = (cb->state == THREAD_RUNNABLE);
    TEST_ASSERT(a_woke_first != b_woke_first,
        "exactly one of {A, B} must be RUNNABLE after first WAKE");

    // Yield: the first-woken consumer resumes, unlinks its waiter,
    // returns TORPOR_OK, parks. The other consumer stays SLEEPING.
    sched();

    if (a_woke_first) {
        TEST_EXPECT_EQ(g_torpor_done_a, 2u, "A completed wait after first WAKE");
        TEST_EXPECT_EQ(g_torpor_rc_a, (s64)TORPOR_OK, "A returned OK");
        TEST_EXPECT_EQ(g_torpor_done_b, 1u, "B is still waiting");
        TEST_EXPECT_EQ(cb->state, THREAD_SLEEPING, "B remains SLEEPING");
    } else {
        TEST_EXPECT_EQ(g_torpor_done_b, 2u, "B completed wait after first WAKE");
        TEST_EXPECT_EQ(g_torpor_rc_b, (s64)TORPOR_OK, "B returned OK");
        TEST_EXPECT_EQ(g_torpor_done_a, 1u, "A is still waiting");
        TEST_EXPECT_EQ(ca->state, THREAD_SLEEPING, "A remains SLEEPING");
    }

    // Second WAKE(count=1) wakes the remaining consumer.
    s64 wake2 = sys_torpor_wake_for_proc(g_torpor_proc, g_torpor_vaddr, 1);
    TEST_EXPECT_EQ(wake2, (s64)1,
        "second WAKE(count=1) reports exactly one woken");

    sched();

    TEST_EXPECT_EQ(g_torpor_done_a, 2u, "A completed wait by end");
    TEST_EXPECT_EQ(g_torpor_done_b, 2u, "B completed wait by end");
    TEST_EXPECT_EQ(g_torpor_rc_a, (s64)TORPOR_OK, "A returned OK overall");
    TEST_EXPECT_EQ(g_torpor_rc_b, (s64)TORPOR_OK, "B returned OK overall");

    // Third WAKE — bucket emptied — returns 0.
    s64 wake3 = sys_torpor_wake_for_proc(g_torpor_proc, g_torpor_vaddr, 100);
    TEST_EXPECT_EQ(wake3, (s64)0,
        "third WAKE on emptied bucket returns 0");

    thread_free(ca);
    thread_free(cb);
    g_torpor_proc->state = 2;        // ZOMBIE
    proc_free(g_torpor_proc);
    g_torpor_proc = NULL;
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree empty after both consumers freed");
}
