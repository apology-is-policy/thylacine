// scheduler dispatch tests (P2-Ba).
//
// scheduler.dispatch_smoke
//   Boot kthread creates two threads, ready()s both, calls sched(). The
//   scheduler rotates through ta → tb → boot. Each rotation increments
//   a per-thread counter; on resume, boot verifies both counters reached
//   1 (i.e., each fresh thread ran exactly once before yielding back).
//
// scheduler.runnable_count
//   sched_runnable_count returns 0 with no ready'd threads; advances
//   correctly with ready() / sched_remove_if_runnable().

#include "test.h"

#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

static volatile u32 g_test_sched_state[2];

static void sched_test_thread_a(void) {
    g_test_sched_state[0]++;
    sched();
    // Unreachable in dispatch_smoke (boot doesn't switch back to ta);
    // if a future test does, the trampoline halts cleanly.
}

static void sched_test_thread_b(void) {
    g_test_sched_state[1]++;
    sched();
    // Unreachable in dispatch_smoke.
}

void test_sched_dispatch_smoke(void) {
    g_test_sched_state[0] = 0;
    g_test_sched_state[1] = 0;

    // The boot kthread is the test's "boot." Other threads in the run
    // tree from prior tests would skew our ordering — assert the tree
    // is empty before we set up.
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree must be empty at test entry");

    struct Thread *ta = thread_create(kproc(), sched_test_thread_a);
    struct Thread *tb = thread_create(kproc(), sched_test_thread_b);
    TEST_ASSERT(ta != NULL, "thread_create(ta) failed");
    TEST_ASSERT(tb != NULL, "thread_create(tb) failed");

    ready(ta);
    ready(tb);
    TEST_EXPECT_EQ(sched_runnable_count(), 2u,
        "two ready() calls must produce two runnable");

    // Yield. sched picks ta (head of NORMAL band, vd_t=0). ta runs,
    // increments counter[0], sched()s. ta's vd_t advanced past the
    // tree; sched picks tb. tb runs, counter[1]++, sched()s. tb's
    // vd_t advanced; sched picks boot (now the lowest vd_t in the
    // tree because ta and tb both had their vd_t advanced past
    // boot's). Boot resumes here.
    sched();

    TEST_EXPECT_EQ(g_test_sched_state[0], 1u,
        "ta did not run exactly once");
    TEST_EXPECT_EQ(g_test_sched_state[1], 1u,
        "tb did not run exactly once");
    TEST_EXPECT_EQ(current_thread(), kthread(),
        "current_thread is not kthread after resume");

    // ta and tb are both RUNNABLE in the tree (each suspended inside
    // its own sched() call). thread_free unlinks them and reclaims.
    thread_free(ta);
    thread_free(tb);
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "tree must be empty after thread_free");
}

void test_sched_runnable_count(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "tree must be empty at test entry");
    TEST_EXPECT_EQ(sched_runnable_count_band(SCHED_BAND_NORMAL), 0u,
        "NORMAL band must be empty at entry");

    struct Thread *t = thread_create(kproc(), sched_test_thread_a);
    TEST_ASSERT(t != NULL, "thread_create failed");

    ready(t);
    TEST_EXPECT_EQ(sched_runnable_count(), 1u,
        "ready advanced count to 1");
    TEST_EXPECT_EQ(sched_runnable_count_band(SCHED_BAND_NORMAL), 1u,
        "NORMAL band has the new thread");
    TEST_EXPECT_EQ(sched_runnable_count_band(SCHED_BAND_INTERACTIVE), 0u,
        "INTERACTIVE band is empty");

    // thread_free removes from tree.
    thread_free(t);
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "thread_free of RUNNABLE thread restored count to 0");
}
