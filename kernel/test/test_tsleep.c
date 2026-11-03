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
//     TSLEEP_TIMEDOUT immediately, never enqueuing on the Rendez or a
//     timer base.
//
//   tsleep.woken_before_deadline
//     Two-thread handoff. A consumer tsleeps with a far deadline; boot
//     sets cond and wakeup()s it before the deadline. The consumer
//     resumes, sees cond true, returns TSLEEP_AWOKEN — cond wins the
//     race against the (unreached) deadline. The consumer takes its own
//     timer entry off on its way out (tsleep.tla NoStaleTimerEntry).
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
#include <thylacine/smp.h>
#include <thylacine/spinlock.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../../arch/arm64/gic.h"
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
    TEST_YIELD_UNTIL(g_tsl_ran >= 1u && consumer->state == THREAD_SLEEPING);

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
    TEST_EXPECT_NE(consumer->state, THREAD_SLEEPING,
        "consumer left the rendez after wakeup");
    TEST_ASSERT(g_tsl_rendez.waiter == NULL,
        "rendez waiter must be cleared after wakeup");
    // #811 (ARCH §8.8.1): the waker no longer clears rendez_blocked_on; the
    // owner clears it on its tsleep-resume under wait_lock. Until the consumer
    // resumes (the sched() below) its backref still points at the rendez.
    TEST_ASSERT(consumer->rendez_blocked_on == &g_tsl_rendez,
        "consumer rendez backref persists until the owner resumes (#811)");

    // Yield: consumer resumes inside tsleep, re-checks cond (now true),
    // returns TSLEEP_AWOKEN, increments to 2, parks.
    TEST_YIELD_UNTIL(g_tsl_ran >= 2u);

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
    TEST_YIELD_UNTIL(g_tsl_ran >= 1u && consumer->state == THREAD_SLEEPING);

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
// base lock acquisition, not all under one hold).

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
    // this CPU's timer base). The cap turns a stuck park into a clean failure.
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

// ---------------------------------------------------------------------------
// timerbase.* -- the per-CPU timer bases (specs/tsleep.tla).
// ---------------------------------------------------------------------------
//
// No secondary's tick runs in the test phase, so every sleeper here, whatever CPU it
// runs on, links on cpu0's base, and that is the base the walk reads. A wake leaves the
// entry on the base; its owner takes it off, and a tick that finds it first wakes nothing.

#define TB_N    6
#define TB_S    1000000000ull
#define TB_MS   1000000ull
#define TB_TRIES 6u

static volatile int  g_tb_cond[TB_N];
static struct Rendez g_tb_rendez[TB_N];
static u64           g_tb_deadline[TB_N];
static volatile int  g_tb_ret[TB_N];
static volatile int  g_tb_cpu[TB_N];
static volatile bool g_tb_done[TB_N];
static volatile bool g_tb_exited[TB_N];

static int tb_cond(void *arg) { return g_tb_cond[(unsigned)(uintptr_t)arg]; }

static void tb_consumer(void *arg) {
    unsigned i = (unsigned)(uintptr_t)arg;
    g_tb_cpu[i] = (int)smp_cpu_idx_self();
    g_tb_ret[i] = tsleep(&g_tb_rendez[i], tb_cond, arg, g_tb_deadline[i]);
    __atomic_store_n(&g_tb_done[i], true, __ATOMIC_RELEASE);
    test_kthread_park_terminal(&g_tb_exited[i]);
}

static struct Thread *tb_create(unsigned i, u64 deadline_ns) {
    rendez_init(&g_tb_rendez[i]);
    g_tb_cond[i]     = 0;
    g_tb_ret[i]      = -1;
    g_tb_cpu[i]      = -1;
    g_tb_deadline[i] = deadline_ns;
    g_tb_done[i]     = false;
    g_tb_exited[i]   = false;
    return thread_create_with_arg(kproc(), tb_consumer, (void *)(uintptr_t)i);
}

// On this CPU, to its sleep: *c is NULL on a failure. A short deadline may be gone
// before the consumer gets to sleep, so done ends the wait too.
static void tb_start(struct Thread **c, unsigned i, u64 deadline_ns) {
    struct Thread *t = tb_create(i, deadline_ns);
    *c = NULL;
    TEST_ASSERT(t != NULL, "thread_create_with_arg(timerbase consumer) failed");
    ready(t);
    TEST_YIELD_UNTIL(t->state == THREAD_SLEEPING ||
                     __atomic_load_n(&g_tb_done[i], __ATOMIC_ACQUIRE));
    *c = t;
}

// On a secondary, to its sleep. A spin: only the IPI's dispatch there is being waited for.
static void tb_start_on_peer(struct Thread **c, unsigned i, u64 deadline_ns, unsigned peer) {
    struct Thread *t = tb_create(i, deadline_ns);
    *c = NULL;
    TEST_ASSERT(t != NULL, "thread_create_with_arg(timerbase consumer) failed");
    ready_on(peer, t);
    (void)gic_send_ipi(peer, IPI_RESCHED);
    u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
    while (t->state != THREAD_SLEEPING &&
           !__atomic_load_n(&g_tb_done[i], __ATOMIC_ACQUIRE) && timer_now_ns() < dl)
        __asm__ __volatile__("yield" ::: "memory");
    *c = t;
}

static void tb_expect_base(struct Thread *const *want, unsigned n, const char *what) {
    struct Thread *got[TB_N];
    struct Thread *tail = NULL;
    u64 earliest = ~0ull;
    unsigned count = timer_base_walk_for_test(got, TB_N, &tail, &earliest);
    TEST_EXPECT_EQ(count, n, what);
    for (unsigned k = 0; k < n; k++) TEST_EXPECT_EQ(got[k], want[k], what);
    TEST_EXPECT_EQ(tail, n ? want[n - 1] : NULL, what);
    TEST_EXPECT_EQ(earliest, n ? want[0]->sleep_deadline : 0ull, what);
}

// The wake leaves the entry; the owner, running, takes it off before it returns.
static void tb_finish(struct Thread *c, unsigned i) {
    g_tb_cond[i] = 1;
    TEST_EXPECT_EQ(wakeup(&g_tb_rendez[i]), 1, "wakeup found the sleeper");
    TEST_YIELD_UNTIL(__atomic_load_n(&g_tb_done[i], __ATOMIC_ACQUIRE));
    TEST_EXPECT_EQ(g_tb_ret[i], TSLEEP_AWOKEN, "the woken consumer saw its condition");
    TEST_EXPECT_EQ(c->timer_base_cpu, 0u, "the consumer left no entry behind");
}

void test_timerbase_sorted_and_fifo(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    tb_expect_base(NULL, 0, "no timed sleeper at test entry");

    u64 now = timer_now_ns();
    // c[1] and c[3] share one value: equal deadlines expire in the order they slept.
    u64 d[TB_N] = { now + 40 * TB_S, now + 20 * TB_S, now + 30 * TB_S,
                    now + 20 * TB_S, now + 25 * TB_S, now + 50 * TB_S };
    struct Thread *c[TB_N];
    for (unsigned i = 0; i < 4; i++) {
        tb_start(&c[i], i, d[i]);
        TEST_ASSERT(c[i] != NULL && c[i]->state == THREAD_SLEEPING,
            "a far-deadline consumer must reach its sleep");
    }
    struct Thread *four[] = { c[1], c[3], c[2], c[0] };
    tb_expect_base(four, 4, "sorted by deadline, then by the order they slept");

    tb_finish(c[3], 3);
    struct Thread *no_middle[] = { c[1], c[2], c[0] };
    tb_expect_base(no_middle, 3, "off the middle: both neighbours relinked");

    tb_finish(c[0], 0);
    struct Thread *no_tail[] = { c[1], c[2] };
    tb_expect_base(no_tail, 2, "off the tail: the tail moved back");

    tb_finish(c[1], 1);
    struct Thread *no_head[] = { c[2] };
    tb_expect_base(no_head, 1, "off the head: head and earliest moved on");

    tb_start(&c[4], 4, d[4]);
    TEST_ASSERT(c[4] != NULL, "consumer 4 must start");
    struct Thread *new_head[] = { c[4], c[2] };
    tb_expect_base(new_head, 2, "a nearer deadline links at the head");

    tb_start(&c[5], 5, d[5]);
    TEST_ASSERT(c[5] != NULL, "consumer 5 must start");
    struct Thread *new_tail[] = { c[4], c[2], c[5] };
    tb_expect_base(new_tail, 3, "a later deadline links at the tail");

    tb_finish(c[4], 4);
    tb_finish(c[2], 2);
    tb_finish(c[5], 5);
    tb_expect_base(NULL, 0, "the base is empty after the last owner left");

    for (unsigned i = 0; i < TB_N; i++) test_kthread_join_free(c[i], &g_tb_exited[i]);
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after the consumers freed");
}

void test_timerbase_wake_takes_no_timer_lock(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Rendez empty = RENDEZ_INIT;
    struct Thread *timed, *plain;
    tb_start(&timed, 0, timer_now_ns() + 60 * TB_S);
    TEST_ASSERT(timed != NULL && timed->state == THREAD_SLEEPING,
        "the deadlined consumer must reach its sleep");
    tb_start(&plain, 1, 0);
    TEST_ASSERT(plain != NULL && plain->state == THREAD_SLEEPING,
        "the plain consumer must reach its sleep");

    // A wake that took this lock would spin here on a lock this thread holds, to the
    // boot's timeout. No TEST_ macro under it: one would return with the lock held.
    irq_state_t s = timer_base_lock_for_test();
    int n_empty = wakeup(&empty);
    g_tb_cond[0] = 1;
    int n_timed = wakeup(&g_tb_rendez[0]);
    g_tb_cond[1] = 1;
    int n_plain = wakeup(&g_tb_rendez[1]);
    u16 mark = __atomic_load_n(&timed->timer_base_cpu, __ATOMIC_RELAXED);
    bool still_sleeping = timed->state == THREAD_SLEEPING;
    timer_base_unlock_for_test(s);

    TEST_EXPECT_EQ(n_empty, 0, "an empty rendez wakes nobody");
    TEST_EXPECT_EQ(n_timed, 1, "the deadlined sleeper was woken");
    TEST_EXPECT_EQ(n_plain, 1, "the plain sleeper was woken");
    TEST_ASSERT(!still_sleeping, "the wake took effect under the held base lock");
    TEST_EXPECT_NE(mark, 0u, "the wake left the timer entry for its owner");

    TEST_YIELD_UNTIL(__atomic_load_n(&g_tb_done[0], __ATOMIC_ACQUIRE) &&
                     __atomic_load_n(&g_tb_done[1], __ATOMIC_ACQUIRE));
    TEST_EXPECT_EQ(g_tb_ret[0], TSLEEP_AWOKEN, "the deadlined consumer saw its condition");
    TEST_EXPECT_EQ(g_tb_ret[1], TSLEEP_AWOKEN, "the plain consumer saw its condition");
    TEST_EXPECT_EQ(timed->timer_base_cpu, 0u, "the owner took its entry off");
    tb_expect_base(NULL, 0, "the base is empty after the owner left");

    test_kthread_join_free(timed, &g_tb_exited[0]);
    test_kthread_join_free(plain, &g_tb_exited[1]);
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after the consumers freed");
}

void test_timerbase_stale_entry_fires_no_wake(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    tb_expect_base(NULL, 0, "no timed sleeper at test entry");

    // The wake must beat the deadline for the entry to go stale. When the tick gets
    // there first the attempt proves nothing, so the next takes a longer deadline.
    struct Thread *c = NULL;
    bool won = false;
    for (unsigned attempt = 0; attempt < TB_TRIES && !won; attempt++) {
        tb_start(&c, 0, timer_now_ns() + (2ull * TB_MS << attempt));
        TEST_ASSERT(c != NULL, "the consumer must start");
        if (wakeup(&g_tb_rendez[0]) == 1) {
            won = true;
        } else {
            TEST_YIELD_UNTIL(__atomic_load_n(&g_tb_done[0], __ATOMIC_ACQUIRE));
            test_kthread_join_free(c, &g_tb_exited[0]);
        }
    }
    TEST_ASSERT(won, "the wake never beat the deadline in any attempt");

    // This thread holds the CPU past the deadline with IRQs on: the tick finds the entry
    // expired while its owner, runnable, has not run to take it off.
    u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
    spin_preempt_inc();
    while (__atomic_load_n(&c->timer_base_cpu, __ATOMIC_RELAXED) != 0 && timer_now_ns() < dl)
        __asm__ __volatile__("yield" ::: "memory");
    u16  mark     = __atomic_load_n(&c->timer_base_cpu, __ATOMIC_RELAXED);
    int  state    = (int)c->state;
    bool timedout = c->sleep_timedout;
    g_tb_cond[0]  = 1;
    spin_preempt_dec();

    TEST_EXPECT_EQ(mark, 0u, "the tick took the stale entry off");
    TEST_EXPECT_EQ(state, (int)THREAD_RUNNABLE, "and left its owner runnable, not woken twice");
    TEST_ASSERT(!timedout, "and did not record a timeout on a thread already woken");

    TEST_YIELD_UNTIL(__atomic_load_n(&g_tb_done[0], __ATOMIC_ACQUIRE));
    TEST_EXPECT_EQ(g_tb_ret[0], TSLEEP_AWOKEN, "the owner returned AWOKEN, its wake intact");
    tb_expect_base(NULL, 0, "the base is empty after the stale entry went");

    test_kthread_join_free(c, &g_tb_exited[0]);
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after the consumer freed");
}

void test_timerbase_secondary_links_on_cpu0(void) {
    if (!test_requires(smp_cpu_count() >= 2, "thylacine.smp", 0, true, "one CPU is online"))
        return;
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    tb_expect_base(NULL, 0, "no timed sleeper at test entry");
    const unsigned peer = 1;

    struct Thread *c;
    tb_start_on_peer(&c, 0, timer_now_ns() + 60 * TB_S, peer);
    TEST_ASSERT(c != NULL, "the consumer must start");
    TEST_EXPECT_EQ(c->state, THREAD_SLEEPING, "the consumer must reach its sleep on the peer");
    TEST_EXPECT_EQ(g_tb_cpu[0], (int)peer, "the consumer must have run on the peer");
    TEST_EXPECT_EQ(c->timer_base_cpu, 1u, "a secondary with no tick links on cpu0's base");
    struct Thread *one[] = { c };
    tb_expect_base(one, 1, "cpu0's base holds the secondary's sleeper");
    tb_finish(c, 0);
    test_kthread_join_free(c, &g_tb_exited[0]);

    // The deadline must outlast the walk to the peer for the tick to be what ends the
    // wait: sleep_timedout says it was.
    bool by_tick = false;
    for (unsigned attempt = 0; attempt < TB_TRIES && !by_tick; attempt++) {
        tb_start_on_peer(&c, 0, timer_now_ns() + (10ull * TB_MS << attempt), peer);
        TEST_ASSERT(c != NULL, "the consumer must start");
        u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
        while (!__atomic_load_n(&g_tb_done[0], __ATOMIC_ACQUIRE) && timer_now_ns() < dl) sched();
        TEST_ASSERT(__atomic_load_n(&g_tb_done[0], __ATOMIC_ACQUIRE),
            "cpu0's tick must end a secondary's timed sleep");
        TEST_EXPECT_EQ(g_tb_ret[0], TSLEEP_TIMEDOUT, "no one woke it: the deadline ended it");
        by_tick = c->sleep_timedout;
        test_kthread_join_free(c, &g_tb_exited[0]);
    }
    TEST_ASSERT(by_tick, "no attempt's deadline outlasted the walk to the peer");
    tb_expect_base(NULL, 0, "the base is empty after the timeout");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after the consumer freed");
}

// ---------------------------------------------------------------------------
// tickless.target_cnt -- the TI-2 idle one-shot arm decision (pure).
// ---------------------------------------------------------------------------
//
// min(nearest deadline, now + backstop), or just the backstop when no deadline.
// Pure, so every branch is driven deterministically without the live timer/list.

void test_tickless_target_cnt(void) {
    u64 now      = 1000000;
    u64 backstop = 6250000;   // an arbitrary backstop delta in counter ticks

    // No deadline (0 sentinel) -> arm to the backstop.
    TEST_EXPECT_EQ(tickless_target_cnt(now, 0, backstop), now + backstop,
        "no deadline -> arm to now + backstop");

    // A deadline nearer than the backstop -> arm to the deadline.
    TEST_EXPECT_EQ(tickless_target_cnt(now, now + 1000, backstop), now + 1000,
        "near deadline -> arm to the deadline");

    // A deadline farther than the backstop -> arm to the backstop.
    TEST_EXPECT_EQ(tickless_target_cnt(now, now + backstop + 1, backstop),
        now + backstop,
        "far deadline -> arm to the backstop");

    // A deadline exactly at the backstop boundary -> the backstop (strict <).
    TEST_EXPECT_EQ(tickless_target_cnt(now, now + backstop, backstop),
        now + backstop,
        "deadline == backstop -> the backstop (strict <)");

    // A past deadline (< now) -> selected as-is; the timer_arm_oneshot_cnt clamp
    // turns it into a fire-ASAP, so an overdue sleeper is serviced immediately.
    TEST_EXPECT_EQ(tickless_target_cnt(now, now - 500, backstop), now - 500,
        "past deadline -> selected as-is (the one-shot clamp fires it ASAP)");
}
