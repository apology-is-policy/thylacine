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

static void tsl_consumer_entry(void) {
    g_tsl_ran++;                                         // → 1: pre-tsleep
    g_tsl_ret = tsleep(&g_tsl_rendez, tsl_cond_check, NULL, g_tsl_deadline);
    g_tsl_ran++;                                         // → 2: post-tsleep
    // Park safely: yield forever rather than returning from the entry
    // function (a return WFE-halts the trampoline). boot frees this
    // thread once it observes g_tsl_ran == 2.
    for (;;) sched();
}

// ---------------------------------------------------------------------------
// tsleep.woken_before_deadline
// ---------------------------------------------------------------------------

void test_tsleep_woken_before_deadline(void) {
    g_tsl_cond     = 0;
    g_tsl_ran      = 0;
    g_tsl_ret      = -1;
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
    TEST_ASSERT(consumer->rendez_blocked_on == NULL,
        "consumer rendez backref must be cleared after wakeup");

    // Yield: consumer resumes inside tsleep, re-checks cond (now true),
    // returns TSLEEP_AWOKEN, increments to 2, parks.
    sched();

    TEST_EXPECT_EQ(g_tsl_ran, 2u,
        "consumer must have run again post-wake");
    TEST_EXPECT_EQ(g_tsl_ret, TSLEEP_AWOKEN,
        "tsleep must return TSLEEP_AWOKEN — cond met before the deadline");

    thread_free(consumer);
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

    thread_free(consumer);
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree empty after consumer freed");
}
