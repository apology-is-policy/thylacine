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
    // #811 (ARCH §8.8.1): the WAKER no longer clears rendez_blocked_on -- only
    // the owning Thread clears it, on its sleep-resume under wait_lock, so the
    // group-terminate cascade can read it lock-safely. Until the consumer
    // resumes (the sched() below) its backref still points at the rendez.
    TEST_ASSERT(consumer->rendez_blocked_on == &g_handoff_rendez,
        "consumer's rendez backref persists until the owner resumes (#811)");
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
    // #811: the owner cleared its backref on resume (under wait_lock) before
    // returning from sleep.
    TEST_ASSERT(consumer->rendez_blocked_on == NULL,
        "consumer's rendez backref cleared on the owner's sleep-resume (#811)");

    // thread_free unlinks consumer from run tree + reclaims stack.
    thread_free(consumer);
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree empty after consumer freed");
}

// ---------------------------------------------------------------------------
// rendez.death_interrupts_sleep  (#811, ARCH §8.8.1)
// ---------------------------------------------------------------------------
//
// The deterministic regression for the universal death-interruptible-sleep
// mechanism: a peer parked INDEFINITELY in a never-satisfiable sleep() (the
// poll(-1) / pipe / devnotes_read hang class) is woken by its Proc's
// group-termination cascade and returns SLEEP_INTR. Without #811 the cascade
// woke only torpor sleepers, so this consumer would stay SLEEPING forever and
// its group-exiting Proc would never reap (the #809-audit F1 hang). The test
// FAILS on pre-#811 code (consumer remains SLEEPING after
// proc_group_terminate) and passes after.

static volatile u32   g_death_run_cnt;
static volatile int   g_death_sleep_rc;
static struct Rendez  g_death_rendez;
static struct Proc   *g_death_proc;

static int death_cond_false(void *arg) {
    (void)arg;
    return 0;                                    // never satisfiable
}

static void death_consumer_entry(void) {
    g_death_run_cnt++;                           // → 1: pre-sleep run
    // Park on a Rendez no producer will ever wake. The ONLY exit is the
    // #811 death-wake: proc_group_terminate flags g_death_proc and the
    // universal cascade wakes us; sleep's resume-path group_exit_msg check
    // returns SLEEP_INTR (do NOT loop on a false cond -- that is the point).
    g_death_sleep_rc = sleep(&g_death_rendez, death_cond_false, NULL);
    g_death_run_cnt++;                           // → 2: post-INTR run
    // Park forever (never fall off entry; mirrors test_torpor's consumer). A
    // RUNNABLE-but-parked thread is safe for boot to thread_free under the
    // deterministic single-CPU harness (secondaries stay idle -> not stolen).
    for (;;) sched();
}

void test_rendez_death_interrupts_sleep(void) {
    g_death_run_cnt  = 0;
    g_death_sleep_rc = 0x7fffffff;               // sentinel: sleep never returned
    rendez_init(&g_death_rendez);

    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree must be empty at test entry");

    // A genuine (non-kproc) Proc: proc_group_terminate's kproc guard skips
    // kproc, so the cascade can only be exercised on a real Proc. thread_create
    // links the consumer into g_death_proc->threads (the list the cascade
    // walks); no proc-table link is needed.
    g_death_proc = proc_alloc();
    TEST_ASSERT(g_death_proc != NULL, "proc_alloc failed");

    struct Thread *consumer = thread_create(g_death_proc, death_consumer_entry);
    TEST_ASSERT(consumer != NULL, "thread_create(consumer) failed");
    ready(consumer);

    // Yield: consumer runs, increments to 1, parks SLEEPING on g_death_rendez
    // (its register-then-observe sees group_exit_msg == NULL, so it sleeps).
    sched();

    TEST_EXPECT_EQ(g_death_run_cnt, 1u, "consumer ran once before sleeping");
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer must be SLEEPING (parked on the never-woken rendez)");
    TEST_EXPECT_EQ(consumer->rendez_blocked_on, &g_death_rendez,
        "consumer's backref points at the never-woken rendez");

    // Reproduce the #811 cascade's per-sleeper effect WITHOUT the machine-wide
    // smp_resched_others() broadcast that proc_group_terminate issues: that IPI
    // would wake the idle secondary CPUs, which would then steal this RUNNABLE
    // consumer and race boot's thread_free under -smp > 1 (the deterministic
    // in-kernel harness relies on secondaries staying idle). The cascade does
    // exactly two things to a sleeping peer: publish the Proc's group_exit_msg
    // (the set-once CAS) and wakeup() the peer's rendez_blocked_on. Do both
    // here. The full proc_group_terminate -- the p->threads walk + the
    // broadcast IPI + the last-out reap -- is exercised end-to-end by the
    // /pouch-hello-exitgroup E2E prover and the joey/stratumd boot path.
    __atomic_store_n(&g_death_proc->group_exit_msg, "killed", __ATOMIC_RELEASE);
    int woke = wakeup(&g_death_rendez);

    // The wake readied the indefinite sleeper. Pre-#811 it is ALSO readied --
    // but on resume it re-checks the false cond and re-sleeps (the hang). The
    // #811 close is that on resume it observes group_exit_msg and returns
    // SLEEP_INTR instead (asserted after the yield below): run_cnt reaches 2.
    TEST_EXPECT_EQ(woke, 1, "the per-sleeper wake reports exactly one waiter");
    TEST_EXPECT_EQ(consumer->state, THREAD_RUNNABLE,
        "sleeper readied by the cascade-equivalent wake");

    // Yield: consumer resumes inside sleep; the resume-path group_exit_msg
    // check fires -> SLEEP_INTR, increments to 2, parks.
    sched();

    TEST_EXPECT_EQ(g_death_run_cnt, 2u, "consumer resumed after the death-wake");
    TEST_EXPECT_EQ(g_death_sleep_rc, SLEEP_INTR,
        "sleep returned SLEEP_INTR for the group-terminating Proc (#811)");
    TEST_ASSERT(consumer->rendez_blocked_on == NULL,
        "consumer cleared its backref on the death-interrupted resume");

    thread_free(consumer);
    g_death_proc->state = 2;                     // ZOMBIE -- proc_free precondition
    proc_free(g_death_proc);
    g_death_proc = NULL;
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree empty after cleanup");
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
