// Deadline-bounded Rendez sleep (tsleep) tests (P5-tsleep).
//
// Five tests covering the contract in rendez.h / specs/tsleep.tla:
//
//   tsleep.fast_path_cond_true
//     cond already true at entry → tsleep returns TSLEEP_AWOKEN without
//     enqueuing. The deadline is irrelevant on the fast path.
//
//   tsleep.no_deadline_degrades
//     deadline_ns == 0 → tsleep is exactly sleep() (ARCH §8.8); cond
//     true → TSLEEP_AWOKEN, no enqueue.
//
//   tsleep.past_deadline_immediate
//     A deadline already in the past + cond false → tsleep returns
//     TSLEEP_TIMEDOUT immediately, never enqueuing on the Rendez or the
//     timer-wait list.
//
//   tsleep.woken_before_deadline
//     Two-thread handoff. A consumer tsleeps with a far deadline; boot
//     sets cond and wakeup()s it before the deadline. The consumer
//     resumes, sees cond true, returns TSLEEP_AWOKEN — cond wins the
//     race against the (unreached) deadline. Exercises the eager
//     timer-wait unlink in wakeup() (tsleep.tla NoStaleTimerEntry).
//
//   tsleep.timeout_via_tick
//     Two-thread handoff. A consumer tsleeps with a short deadline;
//     boot never wakes it. The scheduler-tick scan (timerwait_tick)
//     expires the deadline and wakes the consumer, which returns
//     TSLEEP_TIMEDOUT. The end-to-end proof that a hung producer cannot
//     wedge a tsleep waiter (CORVUS-DESIGN §6.2; tsleep.tla
//     TsleepTerminates).

#include "test.h"

#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../../arch/arm64/timer.h"

// ---------------------------------------------------------------------------
// cond predicates.
// ---------------------------------------------------------------------------

static volatile int g_tsl_cond;

static int tsl_cond_check(void *arg) { (void)arg; return g_tsl_cond; }
static int tsl_cond_true(void *arg)  { (void)arg; return 1; }
static int tsl_cond_false(void *arg) { (void)arg; return 0; }

// ---------------------------------------------------------------------------
// tsleep.fast_path_cond_true
// ---------------------------------------------------------------------------

void test_tsleep_fast_path_cond_true(void) {
    struct Rendez r = RENDEZ_INIT;

    TEST_EXPECT_EQ(current_thread()->state, THREAD_RUNNING,
        "boot must be RUNNING at test entry");

    // A real (future) deadline — but cond is already true, so the fast
    // path returns before the deadline is ever consulted.
    int ret = tsleep(&r, tsl_cond_true, NULL,
                     timer_now_ns() + 1000000000ull);

    TEST_EXPECT_EQ(ret, TSLEEP_AWOKEN,
        "cond true at entry → TSLEEP_AWOKEN fast path");
    TEST_EXPECT_EQ(current_thread()->state, THREAD_RUNNING,
        "fast path must not change state");
    TEST_ASSERT(r.waiter == NULL,
        "fast path must not enqueue a waiter");
    TEST_ASSERT(current_thread()->rendez_blocked_on == NULL,
        "fast path must not set the rendez backref");
}

// ---------------------------------------------------------------------------
// tsleep.no_deadline_degrades
// ---------------------------------------------------------------------------

void test_tsleep_no_deadline_degrades(void) {
    struct Rendez r = RENDEZ_INIT;

    // deadline_ns == 0 → tsleep is exactly sleep(). cond true → returns
    // TSLEEP_AWOKEN via sleep()'s fast path; no enqueue.
    int ret = tsleep(&r, tsl_cond_true, NULL, 0);

    TEST_EXPECT_EQ(ret, TSLEEP_AWOKEN,
        "no-deadline tsleep degrades to sleep → TSLEEP_AWOKEN");
    TEST_EXPECT_EQ(current_thread()->state, THREAD_RUNNING,
        "no-deadline fast path must not change state");
    TEST_ASSERT(r.waiter == NULL,
        "no-deadline fast path must not enqueue a waiter");
}

// ---------------------------------------------------------------------------
// tsleep.past_deadline_immediate
// ---------------------------------------------------------------------------

void test_tsleep_past_deadline_immediate(void) {
    struct Rendez r = RENDEZ_INIT;

    // deadline_ns == 1 — a timestamp long in the past (uptime is many
    // seconds by the time tests run). cond is false, so tsleep must
    // return TSLEEP_TIMEDOUT without ever transitioning to SLEEPING.
    int ret = tsleep(&r, tsl_cond_false, NULL, 1);

    TEST_EXPECT_EQ(ret, TSLEEP_TIMEDOUT,
        "past deadline + cond false → TSLEEP_TIMEDOUT");
    TEST_EXPECT_EQ(current_thread()->state, THREAD_RUNNING,
        "immediate timeout must not change state");
    TEST_ASSERT(r.waiter == NULL,
        "immediate timeout must not enqueue a waiter");
    TEST_ASSERT(current_thread()->rendez_blocked_on == NULL,
        "immediate timeout must not set the rendez backref");
}

// ---------------------------------------------------------------------------
// tsleep.woken_before_deadline + tsleep.timeout_via_tick — shared state.
// ---------------------------------------------------------------------------
//
//   g_tsl_cond     — the condition; producer sets to 1 before wakeup.
//   g_tsl_ran      — consumer increments: pre-tsleep → 1, post-tsleep → 2.
//   g_tsl_ret      — the value tsleep returned in the consumer.
//   g_tsl_deadline — absolute deadline (ns) boot passes to the consumer.
//   g_tsl_rendez   — the Rendez.

static volatile u32  g_tsl_ran;
static volatile int  g_tsl_ret;
static u64           g_tsl_deadline;
static struct Rendez g_tsl_rendez;
static volatile bool g_tsl_exited;   // #109: terminal-park reap handshake

static void tsl_consumer_entry(void) {
    g_tsl_ran++;                                         // → 1: pre-tsleep
    g_tsl_ret = tsleep(&g_tsl_rendez, tsl_cond_check, NULL, g_tsl_deadline);
    g_tsl_ran++;                                         // → 2: post-tsleep
    // #109: terminal EXITING park (was for(;;)sched()); boot reaps via
    // test_kthread_join_free once it observes g_tsl_ran == 2.
    test_kthread_park_terminal(&g_tsl_exited);
}

// ---------------------------------------------------------------------------
// tsleep.woken_before_deadline
// ---------------------------------------------------------------------------

void test_tsleep_woken_before_deadline(void) {
    g_tsl_cond     = 0;
    g_tsl_ran      = 0;
    g_tsl_ret      = -1;
    g_tsl_exited   = false;
    rendez_init(&g_tsl_rendez);
    // A far deadline — the wakeup must win this race.
    g_tsl_deadline = timer_now_ns() + 60ull * 1000000000ull;   // +60 s

    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree must be empty at test entry");

    struct Thread *consumer = thread_create(kproc(), tsl_consumer_entry);
    TEST_ASSERT(consumer != NULL, "thread_create(consumer) failed");
    ready(consumer);

    // Yield to consumer: it runs, increments to 1, tsleeps (cond 0,
    // deadline far) → SLEEPING; sched picks boot back.
    sched();

    TEST_EXPECT_EQ(g_tsl_ran, 1u,
        "consumer must have run once before sleeping");
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer must be SLEEPING inside tsleep");
    TEST_EXPECT_EQ(g_tsl_rendez.waiter, consumer,
        "consumer must be the rendez waiter");

    // Producer: set cond, wake. The wakeup beats the 60 s deadline.
    g_tsl_cond = 1;
    int n = wakeup(&g_tsl_rendez);

    TEST_EXPECT_EQ(n, 1, "wakeup reported exactly one waiter woken");
    TEST_EXPECT_EQ(consumer->state, THREAD_RUNNABLE,
        "consumer must be RUNNABLE after wakeup");
    TEST_ASSERT(g_tsl_rendez.waiter == NULL,
        "rendez waiter must be cleared after wakeup");
    // #811 (ARCH §8.8.1): the waker no longer clears rendez_blocked_on; the
    // owner clears it on its tsleep-resume under wait_lock. Until the consumer
    // resumes (the sched() below) its backref still points at the rendez.
    TEST_ASSERT(consumer->rendez_blocked_on == &g_tsl_rendez,
        "consumer rendez backref persists until the owner resumes (#811)");

    // Yield: consumer resumes inside tsleep, re-checks cond (now true),
    // returns TSLEEP_AWOKEN, increments to 2, parks.
    sched();

    TEST_EXPECT_EQ(g_tsl_ran, 2u,
        "consumer must have run again post-wake");
    TEST_EXPECT_EQ(g_tsl_ret, TSLEEP_AWOKEN,
        "tsleep must return TSLEEP_AWOKEN — cond met before the deadline");
    // #811: the owner cleared its backref on its tsleep-resume.
    TEST_ASSERT(consumer->rendez_blocked_on == NULL,
        "consumer rendez backref cleared on the owner's tsleep-resume (#811)");

    test_kthread_join_free(consumer, &g_tsl_exited);
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree empty after consumer freed");
}

// ---------------------------------------------------------------------------
// tsleep.timeout_via_tick
// ---------------------------------------------------------------------------

void test_tsleep_timeout_via_tick(void) {
    g_tsl_cond     = 0;
    g_tsl_ran      = 0;
    g_tsl_ret      = -1;
    g_tsl_exited   = false;
    rendez_init(&g_tsl_rendez);
    // A short deadline; boot never wakes the consumer — the scheduler-
    // tick scan must.
    g_tsl_deadline = timer_now_ns() + 10ull * 1000000ull;      // +10 ms

    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree must be empty at test entry");

    struct Thread *consumer = thread_create(kproc(), tsl_consumer_entry);
    TEST_ASSERT(consumer != NULL, "thread_create(consumer) failed");
    ready(consumer);

    // Yield to consumer: runs, increments to 1, tsleeps (cond 0,
    // deadline +10 ms) → SLEEPING; sched picks boot back.
    sched();

    TEST_EXPECT_EQ(g_tsl_ran, 1u,
        "consumer must have run once before sleeping");
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer must be SLEEPING inside tsleep");
    TEST_EXPECT_EQ(g_tsl_rendez.waiter, consumer,
        "consumer must be the rendez waiter");

    // Spin yielding until the timer-tick scan times the consumer out and
    // it runs to completion. Real time advances as we spin; sched_tick →
    // timerwait_tick wakes the consumer ~10 ms in, and the next sched()
    // here switches to it. The iteration cap turns a stuck timeout into
    // a clean test failure rather than a hang.
    for (u64 i = 0; i < 100000000ull && g_tsl_ran < 2u; i++) sched();

    TEST_EXPECT_EQ(g_tsl_ran, 2u,
        "consumer must have timed out and run to completion");
    TEST_EXPECT_EQ(g_tsl_ret, TSLEEP_TIMEDOUT,
        "tsleep must return TSLEEP_TIMEDOUT — deadline passed, no wakeup");
    TEST_ASSERT(g_tsl_rendez.waiter == NULL,
        "rendez waiter must be cleared after the timeout wake");
    TEST_ASSERT(consumer->rendez_blocked_on == NULL,
        "consumer rendez backref must be cleared after the timeout wake");

    test_kthread_join_free(consumer, &g_tsl_exited);
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree empty after consumer freed");
}

// ---------------------------------------------------------------------------
// tsleep.herd_timeout
// ---------------------------------------------------------------------------
//
// Several consumers, each in a deadlined tsleep on its own Rendez with a
// common deadline, all time out on the same tick — exercising
// timerwait_tick's one-at-a-time wake loop across multiple iterations
// (P5-tsleep-scale / audit F6: the scan wakes a herd one thread per
// g_timerwait.lock acquisition, not all under one hold).

#define TSL_HERD_N 3

static struct Rendez g_tsl_herd_rendez[TSL_HERD_N];
static volatile int  g_tsl_herd_ret[TSL_HERD_N];
static volatile u32  g_tsl_herd_done;
static u64           g_tsl_herd_deadline;
static volatile bool g_tsl_herd_exited[TSL_HERD_N];  // #109: per-consumer terminal-park handshake

static void tsl_herd_consumer(void *arg) {
    unsigned i = (unsigned)(uintptr_t)arg;
    g_tsl_herd_ret[i] = tsleep(&g_tsl_herd_rendez[i], tsl_cond_false, NULL,
                               g_tsl_herd_deadline);
    __atomic_fetch_add(&g_tsl_herd_done, 1u, __ATOMIC_RELAXED);
    test_kthread_park_terminal(&g_tsl_herd_exited[i]);  // #109: EXITING park
}

void test_tsleep_herd_timeout(void) {
    g_tsl_herd_done = 0;
    for (unsigned i = 0; i < TSL_HERD_N; i++) {
        rendez_init(&g_tsl_herd_rendez[i]);
        g_tsl_herd_ret[i] = -1;
        g_tsl_herd_exited[i] = false;
    }
    g_tsl_herd_deadline = timer_now_ns() + 10ull * 1000000ull;   // +10 ms

    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree must be empty at test entry");

    struct Thread *c[TSL_HERD_N];
    for (unsigned i = 0; i < TSL_HERD_N; i++) {
        c[i] = thread_create_with_arg(kproc(), tsl_herd_consumer,
                                      (void *)(uintptr_t)i);
        TEST_ASSERT(c[i] != NULL,
            "thread_create_with_arg(herd consumer) failed");
        ready(c[i]);
    }

    // Yield until every consumer has run, tsleeped, been timed out by
    // timerwait_tick once the common deadline passes, and run to
    // completion. The cap turns a stuck herd into a clean test failure.
    for (u64 spin = 0;
         spin < 100000000ull && g_tsl_herd_done < (u32)TSL_HERD_N;
         spin++) {
        sched();
    }

    TEST_EXPECT_EQ(g_tsl_herd_done, (u32)TSL_HERD_N,
        "every herd consumer must have timed out and finished");
    for (unsigned i = 0; i < TSL_HERD_N; i++) {
        TEST_EXPECT_EQ(g_tsl_herd_ret[i], TSLEEP_TIMEDOUT,
            "each herd consumer's tsleep must return TSLEEP_TIMEDOUT");
    }

    for (unsigned i = 0; i < TSL_HERD_N; i++) {
        test_kthread_join_free(c[i], &g_tsl_herd_exited[i]);
    }
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree empty after the herd is freed");
}

// ---------------------------------------------------------------------------
// timerwait.earliest_deadline -- the TI-1 nearest-deadline scan.
// ---------------------------------------------------------------------------
//
// Empty list -> 0; two deadlined sleepers -> the minimum sleep_deadline (the
// nearer one); back to 0 once both are woken + freed. The deadline is exact
// (read straight off the parked Thread's stored counter value), so the min
// assertion is deterministic, not timing-sensitive. The idle loop (TI-2) arms
// its one-shot to this value.

#define ED_N 2

static volatile int  g_ed_cond;
static volatile u32  g_ed_done;
static u64           g_ed_deadline[ED_N];
static struct Rendez g_ed_rendez[ED_N];
static volatile bool g_ed_exited[ED_N];

static int ed_cond_check(void *arg) { (void)arg; return g_ed_cond; }

static void ed_consumer(void *arg) {
    unsigned i = (unsigned)(uintptr_t)arg;
    tsleep(&g_ed_rendez[i], ed_cond_check, NULL, g_ed_deadline[i]);
    __atomic_fetch_add(&g_ed_done, 1u, __ATOMIC_RELAXED);
    test_kthread_park_terminal(&g_ed_exited[i]);
}

void test_timerwait_earliest_deadline(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree must be empty at test entry");
    TEST_EXPECT_EQ(timerwait_earliest_deadline(), 0ull,
        "no deadlined sleeper -> earliest deadline 0");

    g_ed_cond = 0;
    g_ed_done = 0;
    for (unsigned i = 0; i < ED_N; i++) {
        rendez_init(&g_ed_rendez[i]);
        g_ed_exited[i] = false;
    }
    // Two distinct far deadlines; consumer 1 (the +30 s sleeper) is the nearer.
    g_ed_deadline[0] = timer_now_ns() + 60ull * 1000000000ull;
    g_ed_deadline[1] = timer_now_ns() + 30ull * 1000000000ull;

    struct Thread *c[ED_N];
    for (unsigned i = 0; i < ED_N; i++) {
        c[i] = thread_create_with_arg(kproc(), ed_consumer, (void *)(uintptr_t)i);
        TEST_ASSERT(c[i] != NULL, "thread_create_with_arg(ed consumer) failed");
        ready(c[i]);
    }

    // Yield until both consumers have parked in tsleep (SLEEPING + linked on
    // g_timerwait). The cap turns a stuck park into a clean failure.
    for (u64 spin = 0; spin < 100000000ull &&
         !(c[0]->state == THREAD_SLEEPING && c[1]->state == THREAD_SLEEPING);
         spin++) {
        sched();
    }
    TEST_EXPECT_EQ(c[0]->state, THREAD_SLEEPING, "consumer 0 must be SLEEPING");
    TEST_EXPECT_EQ(c[1]->state, THREAD_SLEEPING, "consumer 1 must be SLEEPING");

    // The earliest deadline is the minimum stored sleep_deadline -- consumer 1.
    u64 expect_min = c[0]->sleep_deadline < c[1]->sleep_deadline
                   ? c[0]->sleep_deadline : c[1]->sleep_deadline;
    TEST_EXPECT_EQ(timerwait_earliest_deadline(), expect_min,
        "earliest deadline must equal the minimum sleep_deadline");
    TEST_EXPECT_EQ(timerwait_earliest_deadline(), c[1]->sleep_deadline,
        "consumer 1 (the +30 s sleeper) must be the nearest deadline");

    // Wake both: set cond, wakeup each. They resume in tsleep, see cond true,
    // return TSLEEP_AWOKEN, increment done, park. (cond false would re-sleep.)
    g_ed_cond = 1;
    for (unsigned i = 0; i < ED_N; i++) wakeup(&g_ed_rendez[i]);
    for (u64 spin = 0; spin < 100000000ull && g_ed_done < (u32)ED_N; spin++) {
        sched();
    }
    TEST_EXPECT_EQ(g_ed_done, (u32)ED_N,
        "both consumers must have resumed and finished");

    for (unsigned i = 0; i < ED_N; i++) {
        test_kthread_join_free(c[i], &g_ed_exited[i]);
    }
    TEST_EXPECT_EQ(timerwait_earliest_deadline(), 0ull,
        "earliest deadline back to 0 after both sleepers wake + free");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree empty after both consumers freed");
}
