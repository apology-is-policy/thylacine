
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t u64;
#define TEST_YIELD_BUDGET_NS 4u
#define KP_ZERO 0
struct Loom { char bytes[4096]; };
struct Thread { int unused; };
static struct Loom storage;
static struct Thread worker;
static void *volatile g_lp_cross_obj;
static volatile bool g_lp_cross_exited;
static void (*entry)(void);
static int mode, frees, joined, helper_live, inner_entered, shared_timeouts;
static bool armed, fired, in_helper;
static u64 clock_ns;
#define REQUIRE(c, msg) do { if (!(c)) { puts(msg); exit(11); } } while (0)
static u64 timer_now_ns(void) { return clock_ns++; }
static void *kmalloc(size_t n, int flags) {
    (void)n; (void)flags; return mode == 3 ? NULL : &storage;
}
static void kfree(void *p) {
    if (!p) return;
    REQUIRE(p == &storage && frees == 0, "wrong or duplicate object free");
    frees++;
    if (armed && (!in_helper || mode == 5)) fired = true;
}
static void kfree_large_watch_arm_for_test(void *p) {
    REQUIRE(p == &storage, "wrong watched object"); armed = true; fired = false;
}
static bool kfree_large_watch_fired_for_test(void) { return fired; }
static void test_kthread_park_terminal(volatile bool *exited) {
    __atomic_store_n(exited, true, __ATOMIC_RELEASE);
}
static void run_helper(void) { in_helper = true; entry(); in_helper = false; }
static void kfree_large_watch_disarm_for_test(void) {
    // Completion AFTER the wait chose timeout must not turn it into success.
    if (mode == 2 && helper_live && !g_lp_cross_exited) run_helper();
    armed = false; fired = false;
}
static void *kproc(void) { return NULL; }
static struct Thread *thread_create(void *p, void (*fn)(void)) {
    (void)p; if (mode == 4) return NULL;
    entry = fn; helper_live = 1; return &worker;
}
static void ready(struct Thread *t) { (void)t; }
static void sched(void) {
    if (mode != 1 && mode != 2 && !g_lp_cross_exited) run_helper();
}
static void test_kthread_join_free(struct Thread *t, volatile bool *exited) {
    REQUIRE(t == &worker && helper_live, "wrong helper reap");
    u64 start = timer_now_ns();
    while (!__atomic_load_n(exited, __ATOMIC_ACQUIRE)) {
        if (timer_now_ns() - start >= TEST_YIELD_BUDGET_NS) {
            shared_timeouts++; return; // actual void helper's timeout contract
        }
        sched();
    }
    helper_live = 0; joined++;
}
#define LP_CHECK(x, msg) do { if (!(x)) { error = msg; goto done; } } while (0)
static void lp_cross_free(void) {
    kfree((void *)g_lp_cross_obj);
    test_kthread_park_terminal(&g_lp_cross_exited);
}
static bool lp_cross_wait(void) {
    u64 start = timer_now_ns();
    while (!__atomic_load_n(&g_lp_cross_exited, __ATOMIC_ACQUIRE)) {
        if (timer_now_ns() - start >= TEST_YIELD_BUDGET_NS) return false;
        sched();
    }
    return true;
}
static const char *exercise(void) {
const char *error = NULL;
    void *wc = kmalloc(sizeof(struct Loom), KP_ZERO);
    bool cross_ran = false, cross_fired = false;
    if (wc) {
        kfree_large_watch_arm_for_test(wc);
        g_lp_cross_obj = wc;
        g_lp_cross_exited = false;
        struct Thread *helper = thread_create(kproc(), lp_cross_free);
        if (helper) {
            ready(helper);
            test_kthread_join_free(helper, &g_lp_cross_exited);
            cross_ran = true;
            cross_fired = kfree_large_watch_fired_for_test();
        } else {
            kfree(wc);
        }
        wc = NULL;
    }
    kfree_large_watch_disarm_for_test();
    LP_CHECK(cross_ran, "the cross-thread watch control ran");
    LP_CHECK(!cross_fired, "a watched free on another thread leaves the watch quiet");


    inner_entered++; // would arm the inner fault next in the real fixture
done:
    return error;
}
int main(int argc, char **argv) {
    REQUIRE(argc == 2, "mode required"); mode = atoi(argv[1]);
    if (mode == 6) clock_ns = UINT64_MAX - 2;
    const char *error = exercise();
    REQUIRE(!armed, "watch survives exit");
    if (mode == 1 || mode == 2) {
        REQUIRE(error && !strcmp(error,
            "the cross-thread watch helper completed before the deadline"),
            "timeout lost: fixture advanced beyond incomplete helper");
        REQUIRE(inner_entered == 0 && joined == 0 && shared_timeouts == 0,
            "timeout entered inner leg or infallible-join assumption survived");
        REQUIRE(helper_live && frees == (mode == 2), "timeout reclaimed helper/object");
        if (!g_lp_cross_exited) run_helper();
        REQUIRE(frees == 1, "late helper lost transferred object");
    } else if (mode == 3 || mode == 4) {
        REQUIRE(error && !strcmp(error, "the cross-thread watch control ran"),
            "allocation refusal misattributed");
        REQUIRE(!inner_entered && !joined && !helper_live && frees == (mode == 4),
            "allocation failure ownership wrong");
    } else {
        REQUIRE(joined == 1 && frees == 1 && !helper_live && !shared_timeouts,
            "completed helper not reaped exactly once");
        if (mode == 5) {
            REQUIRE(error && !strcmp(error,
                "a watched free on another thread leaves the watch quiet"),
                "M3 no longer discriminated");
            REQUIRE(!inner_entered, "M3 entered inner leg");
        } else REQUIRE(!error && inner_entered == 1, "healthy completion refused");
    }
    printf("PASS mode=%d free=%d join=%d inner=%d\n", mode, frees, joined, inner_entered);
    return 0;
}
