// Plan 9 wait/wake (sleep + wakeup over Rendez) tests (P2-Bb).
//
// Three tests covering the core invariants:
//
//   rendez.sleep_immediate_cond_true
//     The fast path: cond is already true at sleep entry. sleep returns
//     without state transition; the rendez stays empty; the calling
//     thread stays RUNNING. Mirrors scheduler.tla WaitOnCond's
//     `IF cond THEN UNCHANGED vars` branch — and structurally is the
//     proof that the impl can never produce the missed-wakeup race
//     (cond check is atomic with sleep transition under r->lock).
//
//   rendez.basic_handoff
//     Two-thread producer/consumer over a Rendez. Boot creates a
//     consumer thread, ready()s it, yields to it. Consumer enters
//     sleep with cond=false; transitions SLEEPING; sched picks boot
//     back. Boot sets cond=true, wakeup()s — consumer goes RUNNABLE.
//     Boot yields again; consumer resumes inside sleep, sees cond=true,
//     exits the sleep loop, yields back to boot. Boot asserts the
//     full lifecycle: counter advances, state transitions match the
//     spec actions, no leaks.
//
//   rendez.wakeup_no_waiter
//     Idempotency / no-op: wakeup on an empty Rendez returns 0 without
//     side effects.

#include "test.h"

#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

// ---------------------------------------------------------------------------
// rendez.sleep_immediate_cond_true
// ---------------------------------------------------------------------------

static int cond_always_true(void *arg) {
    (void)arg;
    return 1;
}

void test_rendez_sleep_immediate_cond_true(void) {
    struct Rendez r = RENDEZ_INIT;

    TEST_EXPECT_EQ(current_thread()->state, THREAD_RUNNING,
        "boot must be RUNNING at test entry");
    TEST_ASSERT(r.waiter == NULL,
        "fresh rendez has no waiter");
    TEST_ASSERT(current_thread()->rendez_blocked_on == NULL,
        "boot has no rendez backref pre-sleep");

    sleep(&r, cond_always_true, NULL);

    TEST_EXPECT_EQ(current_thread()->state, THREAD_RUNNING,
        "fast path must not change state");
    TEST_ASSERT(r.waiter == NULL,
        "fast path must not enqueue a waiter");
    TEST_ASSERT(current_thread()->rendez_blocked_on == NULL,
        "fast path must not set rendez backref");
}

// ---------------------------------------------------------------------------
// rendez.basic_handoff
// ---------------------------------------------------------------------------
//
// Shared state between boot kthread (test runner) + consumer thread.
//
//   g_handoff_cond    — the condition variable. Producer sets to 1
//                        before calling wakeup. cond fn returns it.
//   g_handoff_run_cnt — counter incremented by consumer. Pre-sleep:
//                        ++ → 1. Post-wake: ++ → 2.
//   g_handoff_rendez  — the Rendez instance.

static volatile int      g_handoff_cond;
static volatile u32      g_handoff_run_cnt;
static struct Rendez     g_handoff_rendez;

static int handoff_cond_check(void *arg) {
    (void)arg;
    return g_handoff_cond;
}

static void handoff_consumer_entry(void) {
    g_handoff_run_cnt++;                         // → 1: pre-sleep run
    sleep(&g_handoff_rendez, handoff_cond_check, NULL);
    g_handoff_run_cnt++;                         // → 2: post-wake run
    sched();                                     // yield back to boot
    // Unreachable: boot doesn't switch back. If it ever does, the
    // trampoline halts on entry-return.
}

void test_rendez_basic_handoff(void) {
    g_handoff_cond    = 0;
    g_handoff_run_cnt = 0;
    rendez_init(&g_handoff_rendez);

    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree must be empty at test entry");

    struct Thread *consumer = thread_create(kproc(), handoff_consumer_entry);
    TEST_ASSERT(consumer != NULL,
        "thread_create(consumer) failed");
    TEST_EXPECT_EQ(consumer->state, THREAD_RUNNABLE,
        "fresh consumer must be RUNNABLE");

    ready(consumer);
    TEST_EXPECT_EQ(sched_runnable_count(), 1u,
        "consumer must be in run tree after ready");

    // Yield to consumer. sched: boot → RUNNABLE+insert; pick consumer;
    // switch. Consumer runs, increments counter to 1, calls sleep.
    // sleep observes cond=0, transitions consumer → SLEEPING under
    // r->lock, drops lock, calls sched. sched picks boot (only
    // runnable thread); switches back. We resume here.
    sched();

    // Resumed in boot. Consumer ran once and is now SLEEPING on
    // g_handoff_rendez.
    TEST_EXPECT_EQ(g_handoff_run_cnt, 1u,
        "consumer must have run once before sleeping");
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer must be SLEEPING after entering sleep");
    TEST_EXPECT_EQ(g_handoff_rendez.waiter, consumer,
        "consumer must be the rendez waiter");
    TEST_EXPECT_EQ(consumer->rendez_blocked_on, &g_handoff_rendez,
        "consumer's rendez backref points to handoff rendez");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "no other runnable thread (consumer is SLEEPING; boot is RUNNING)");

    // Producer side: set cond and wake.
    g_handoff_cond = 1;
    int n = wakeup(&g_handoff_rendez);

    TEST_EXPECT_EQ(n, 1,
        "wakeup reported exactly one waiter woken");
    TEST_EXPECT_EQ(consumer->state, THREAD_RUNNABLE,
        "consumer must be RUNNABLE after wakeup");
    TEST_ASSERT(g_handoff_rendez.waiter == NULL,
        "rendez waiter must be cleared after wakeup");
    TEST_ASSERT(consumer->rendez_blocked_on == NULL,
        "consumer's rendez backref must be cleared");
    TEST_EXPECT_EQ(sched_runnable_count(), 1u,
        "consumer must be back in the run tree");

    // Yield. Consumer resumes inside sleep's loop, reacquires r->lock,
    // re-evaluates cond (now true), exits the loop, returns from
    // sleep. Increments counter to 2. Calls sched to come back. We
    // resume here.
    sched();

    TEST_EXPECT_EQ(g_handoff_run_cnt, 2u,
        "consumer must have run again post-wake");
    TEST_EXPECT_EQ(current_thread(), kthread(),
        "back in boot kthread after final yield");
    TEST_EXPECT_EQ(consumer->state, THREAD_RUNNABLE,
        "consumer must be RUNNABLE (suspended inside sched after handoff)");

    // thread_free unlinks consumer from run tree + reclaims stack.
    thread_free(consumer);
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree empty after consumer freed");
}

// ---------------------------------------------------------------------------
// rendez.wakeup_no_waiter
// ---------------------------------------------------------------------------

void test_rendez_wakeup_no_waiter(void) {
    struct Rendez r = RENDEZ_INIT;

    int n = wakeup(&r);
    TEST_EXPECT_EQ(n, 0,
        "wakeup with no waiter must return 0");
    TEST_ASSERT(r.waiter == NULL,
        "rendez waiter must remain NULL");

    // Idempotent: a second wakeup is also a no-op.
    n = wakeup(&r);
    TEST_EXPECT_EQ(n, 0,
        "second wakeup must also return 0");
}
