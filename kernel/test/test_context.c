// context-switch leaf-API tests (P2-A).
//
// Two tests:
//
//   context.create_destroy
//     thread_create / thread_free without ever switching into the new
//     thread. Validates: counters advance, stack is allocated/freed,
//     proc thread_count tracks correctly.
//
//   context.round_trip
//     Boot kthread switches into a fresh kthread; the fresh kthread
//     runs an entry that increments a shared counter and switches back.
//     Validates the entire context-switch primitive end-to-end:
//     cpu_switch_context save + load, thread_trampoline reach via ret,
//     blr-to-entry under BTI, return-via-thread_switch round trip.
//
// The shared counter is `volatile` so the compiler can't fold the
// increment into the test's view of state — the value MUST be observed
// after the second thread runs.

#include "test.h"

#include <thylacine/proc.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

static volatile u32   g_test_state;
static struct Thread *g_test_main;

static void test_thread_entry(void) {
    g_test_state++;
    thread_switch(g_test_main);
    // Unreachable in the test — boot kthread doesn't switch back to us.
    // If a future test does, the entry would fall off the end and the
    // trampoline's WFE halt picks up.
}

void test_context_create_destroy(void) {
    u64 created_before   = thread_total_created();
    u64 destroyed_before = thread_total_destroyed();
    int tcount_before    = kproc()->thread_count;

    struct Thread *t = thread_create(kproc(), test_thread_entry);
    TEST_ASSERT(t != NULL,
        "thread_create returned NULL");
    TEST_EXPECT_EQ(t->state, THREAD_RUNNABLE,
        "fresh thread must be RUNNABLE");
    TEST_ASSERT(t->kstack_base != NULL,
        "fresh thread must own a kstack");
    TEST_EXPECT_EQ(t->kstack_size, (size_t)(16 * 1024),
        "fresh thread's kstack must be 16 KiB");
    TEST_EXPECT_EQ(t->proc, kproc(),
        "fresh thread's proc must be kproc");
    TEST_EXPECT_EQ(thread_total_created(), created_before + 1,
        "created counter did not advance");
    TEST_EXPECT_EQ(kproc()->thread_count, tcount_before + 1,
        "proc thread_count did not advance");

    thread_free(t);
    TEST_EXPECT_EQ(thread_total_destroyed(), destroyed_before + 1,
        "destroyed counter did not advance");
    TEST_EXPECT_EQ(kproc()->thread_count, tcount_before,
        "proc thread_count did not retreat");
}

void test_context_round_trip(void) {
    g_test_main  = current_thread();
    TEST_ASSERT(g_test_main != NULL,
        "current_thread() returned NULL (thread_init not called?)");
    TEST_EXPECT_EQ(g_test_main, kthread(),
        "test must run from kthread");
    TEST_EXPECT_EQ(g_test_main->state, THREAD_RUNNING,
        "kthread must be RUNNING at test entry");

    g_test_state = 0;

    struct Thread *t = thread_create(kproc(), test_thread_entry);
    TEST_ASSERT(t != NULL,
        "thread_create returned NULL");

    // The switch + return round trip. After thread_switch(t) returns,
    // we are back in g_test_main and t has run its entry once.
    thread_switch(t);

    TEST_EXPECT_EQ(g_test_state, 1u,
        "second thread did not increment shared counter");
    TEST_EXPECT_EQ(current_thread(), g_test_main,
        "current_thread not restored after switch-back");
    TEST_EXPECT_EQ(g_test_main->state, THREAD_RUNNING,
        "main thread state must be RUNNING after resume");
    TEST_EXPECT_EQ(t->state, THREAD_RUNNABLE,
        "second thread must be RUNNABLE after switching back");

    // The second thread is suspended inside its thread_switch call.
    // We don't resume it; freeing its descriptor + stack reclaims the
    // resources cleanly.
    thread_free(t);
}
