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

#include <thylacine/notes.h>   // LS-5c: notes_post arm + NOTE_BIT_INTERRUPT
#include <thylacine/poll.h>    // caught_note_ends_wait4: the child_waiters wake
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>
#include <thylacine/vivarium.h>  // caught_wake_*: a Linux sigtab row per leg

#include "../../arch/arm64/exception.h"  // caught_note_tail_*: a synthetic EL0-return frame
#include "../../arch/arm64/timer.h"   // LS-5c: far tsleep deadline (timer_now_ns)
#include "../../mm/slub.h"            // caught_wake_*: kzalloc a sigtab proc_free frees

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
    TEST_YIELD_UNTIL(g_handoff_run_cnt >= 1u && consumer->state == THREAD_SLEEPING);

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
    TEST_EXPECT_NE(consumer->state, THREAD_SLEEPING,
        "consumer left the rendez after wakeup");
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
    TEST_YIELD_UNTIL(g_handoff_run_cnt >= 2u);

    TEST_EXPECT_EQ(g_handoff_run_cnt, 2u,
        "consumer must have run again post-wake");
    TEST_EXPECT_EQ(current_thread(), kthread(),
        "back in boot kthread after final yield");
    TEST_EXPECT_NE(consumer->state, THREAD_SLEEPING,
        "consumer is not blocked (suspended inside sched after handoff)");
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
static volatile bool  g_death_exited;   // #109: terminal-park reap handshake

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
    // #109: terminal EXITING park (was a RUNNABLE for(;;)sched()); the joiner
    // reaps via test_kthread_join_free.
    test_kthread_park_terminal(&g_death_exited);
}

void test_rendez_death_interrupts_sleep(void) {
    g_death_run_cnt  = 0;
    g_death_sleep_rc = 0x7fffffff;               // sentinel: sleep never returned
    g_death_exited   = false;
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
    TEST_YIELD_UNTIL(g_death_run_cnt >= 1u && consumer->state == THREAD_SLEEPING);

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
    TEST_EXPECT_NE(consumer->state, THREAD_SLEEPING,
        "sleeper readied by the cascade-equivalent wake");

    // Yield: consumer resumes inside sleep; the resume-path group_exit_msg
    // check fires -> SLEEP_INTR, increments to 2, parks.
    TEST_YIELD_UNTIL(g_death_run_cnt >= 2u);

    TEST_EXPECT_EQ(g_death_run_cnt, 2u, "consumer resumed after the death-wake");
    TEST_EXPECT_EQ(g_death_sleep_rc, SLEEP_INTR,
        "sleep returned SLEEP_INTR for the group-terminating Proc (#811)");
    TEST_ASSERT(consumer->rendez_blocked_on == NULL,
        "consumer cleared its backref on the death-interrupted resume");

    test_kthread_join_free(consumer, &g_death_exited);
    g_death_proc->state = PROC_STATE_ZOMBIE;     // proc_free precondition
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

// ---------------------------------------------------------------------------
// rendez.intr_terminate_* — LS-5c (P3-terminate, ARCH 8.8.2): the widened
// #811 wake. A terminate-disposition `interrupt` (notes_post arms the
// PROC_FLAG_INTR_TERMINATE_PENDING latch) wakes a blocked sleeper exactly
// like group-exit death: sleep returns SLEEP_INTR and the thread unwinds to
// its EL0-return tail (where, in production, the LS-5b dispatch terminates
// it). Unlike the death test above, these drive the REAL waker
// (proc_interrupt_terminate_wake) -- it issues no IPI broadcast, so it is
// safe under the deterministic single-CPU harness.
// ---------------------------------------------------------------------------

static volatile u32   g_intr_run_cnt;
static volatile int   g_intr_sleep_rc;
static struct Rendez  g_intr_rendez;
static struct Proc   *g_intr_proc;
static volatile bool  g_intr_exited;    // #109: terminal-park handshake (shared by the intr_* consumers)

static void intr_consumer_entry(void) {
    g_intr_run_cnt++;                            // -> 1: pre-sleep run
    g_intr_sleep_rc = sleep(&g_intr_rendez, death_cond_false, NULL);
    g_intr_run_cnt++;                            // -> 2: post-INTR run
    test_kthread_park_terminal(&g_intr_exited);  // #109: EXITING park
}

// The blocked-sleeper leg: consumer parks SLEEPING; boot posts a REAL
// `interrupt` (arms the latch -- fresh Proc, no handler, not self-managing)
// and runs the REAL wake walk under g_proc_table_lock. The consumer resumes,
// its resume-path thread_die_pending check fires, sleep returns SLEEP_INTR.
void test_rendez_intr_terminate_interrupts_sleep(void) {
    g_intr_run_cnt  = 0;
    g_intr_sleep_rc = 0x7fffffff;
    g_intr_exited   = false;
    rendez_init(&g_intr_rendez);

    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree must be empty at test entry");

    g_intr_proc = proc_alloc();
    TEST_ASSERT(g_intr_proc != NULL, "proc_alloc failed");

    struct Thread *consumer = thread_create(g_intr_proc, intr_consumer_entry);
    TEST_ASSERT(consumer != NULL, "thread_create(consumer) failed");
    ready(consumer);
    TEST_YIELD_UNTIL(g_intr_run_cnt >= 1u && consumer->state == THREAD_SLEEPING);

    TEST_EXPECT_EQ(g_intr_run_cnt, 1u, "consumer ran once before sleeping");
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer must be SLEEPING (parked on the never-woken rendez)");

    // The REAL production sequence: post (arms the latch under q->lock),
    // then the wake walk under g_proc_table_lock -- exactly what
    // postnote_walk_cb / proc_console_post_interrupt do.
    TEST_EXPECT_EQ(notes_post(g_intr_proc, "interrupt", 0u, NULL, true), 0,
        "interrupt post accepted");
    TEST_ASSERT(proc_intr_terminate_pending(g_intr_proc),
        "the post armed the terminate latch");
    irq_state_t s = proc_table_lock_acquire();
    proc_interrupt_terminate_wake(g_intr_proc);
    proc_table_lock_release(s);

    TEST_EXPECT_NE(consumer->state, THREAD_SLEEPING,
        "sleeper readied by the terminate wake");
    TEST_YIELD_UNTIL(g_intr_run_cnt >= 2u);

    TEST_EXPECT_EQ(g_intr_run_cnt, 2u, "consumer resumed after the wake");
    TEST_EXPECT_EQ(g_intr_sleep_rc, SLEEP_INTR,
        "sleep returned SLEEP_INTR for the terminate-pending Proc (LS-5c)");
    TEST_ASSERT(consumer->rendez_blocked_on == NULL,
        "consumer cleared its backref on the interrupted resume");

    test_kthread_join_free(consumer, &g_intr_exited);
    g_intr_proc->state = PROC_STATE_ZOMBIE;
    proc_free(g_intr_proc);
    g_intr_proc = NULL;
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after cleanup");
}

// The register-then-observe leg (I-9): the latch is armed BEFORE the thread
// ever sleeps. Its first sleep registers, re-observes the latch under
// wait_lock, undoes the registration, and returns SLEEP_INTR -- with NO wake
// call at all (the waker ran before the sleeper registered; the walk found
// nothing; the post-register check is what closes the race).
void test_rendez_intr_terminate_register_observe(void) {
    g_intr_run_cnt  = 0;
    g_intr_sleep_rc = 0x7fffffff;
    g_intr_exited   = false;
    rendez_init(&g_intr_rendez);

    g_intr_proc = proc_alloc();
    TEST_ASSERT(g_intr_proc != NULL, "proc_alloc failed");

    // Arm FIRST (the waker's walk would find no sleeper and wake nothing).
    TEST_EXPECT_EQ(notes_post(g_intr_proc, "interrupt", 0u, NULL, true), 0,
        "interrupt post accepted");
    TEST_ASSERT(proc_intr_terminate_pending(g_intr_proc), "latch armed");

    struct Thread *consumer = thread_create(g_intr_proc, intr_consumer_entry);
    TEST_ASSERT(consumer != NULL, "thread_create(consumer) failed");
    ready(consumer);
    TEST_YIELD_UNTIL(g_intr_run_cnt >= 2u);

    // One yield: the consumer's FIRST sleep observed the latch at its
    // post-register check and returned synchronously -- never SLEEPING.
    TEST_EXPECT_EQ(g_intr_run_cnt, 2u,
        "consumer's first sleep returned without ever parking");
    TEST_EXPECT_EQ(g_intr_sleep_rc, SLEEP_INTR,
        "register-then-observe returned SLEEP_INTR (no wake was issued)");
    TEST_ASSERT(g_intr_rendez.waiter == NULL,
        "the undone registration left the rendez empty");

    test_kthread_join_free(consumer, &g_intr_exited);
    g_intr_proc->state = PROC_STATE_ZOMBIE;
    proc_free(g_intr_proc);
    g_intr_proc = NULL;
}

// The masked leg: a thread that masked `interrupt` is NOT interrupted by the
// latch (masking defers) -- the wake walk still wakes it (by design: the
// walk does not read masks), but its resume-path predicate reads its own
// mask, loops, re-registers, and sleeps again. Death (group_exit_msg) then
// overrides the mask (N-4: death is not deferrable) -- the cleanup leg.
static void intr_masked_consumer_entry(void) {
    g_intr_run_cnt++;                            // -> 1: pre-sleep run
    current_thread()->note_mask |= (1u << NOTE_BIT_INTERRUPT);
    g_intr_sleep_rc = sleep(&g_intr_rendez, death_cond_false, NULL);
    g_intr_run_cnt++;                            // -> 2: post-INTR run
    test_kthread_park_terminal(&g_intr_exited);  // #109: EXITING park
}

void test_rendez_intr_terminate_masked_sleeps_through(void) {
    g_intr_run_cnt  = 0;
    g_intr_sleep_rc = 0x7fffffff;
    g_intr_exited   = false;
    rendez_init(&g_intr_rendez);

    g_intr_proc = proc_alloc();
    TEST_ASSERT(g_intr_proc != NULL, "proc_alloc failed");

    struct Thread *consumer =
        thread_create(g_intr_proc, intr_masked_consumer_entry);
    TEST_ASSERT(consumer != NULL, "thread_create(consumer) failed");
    ready(consumer);
    TEST_YIELD_UNTIL(consumer->state == THREAD_SLEEPING);

    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING, "masked consumer parked");

    // Arm + wake. The walk wakes the masked sleeper (benign spurious wake);
    // its own-mask predicate keeps it alive: it re-registers and re-sleeps.
    TEST_EXPECT_EQ(notes_post(g_intr_proc, "interrupt", 0u, NULL, true), 0,
        "interrupt post accepted");
    TEST_ASSERT(proc_intr_terminate_pending(g_intr_proc),
        "latch armed (masks are per-thread; the proc-level arm ignores them)");
    irq_state_t s = proc_table_lock_acquire();
    proc_interrupt_terminate_wake(g_intr_proc);
    proc_table_lock_release(s);
    TEST_YIELD_UNTIL(g_intr_run_cnt >= 1u && consumer->state == THREAD_SLEEPING);

    TEST_EXPECT_EQ(g_intr_run_cnt, 1u,
        "masked consumer absorbed the wake and re-slept (no INTR)");
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "masked consumer is SLEEPING again");
    TEST_ASSERT(consumer->rendez_blocked_on == &g_intr_rendez,
        "masked consumer re-registered on the rendez");
    TEST_EXPECT_EQ(g_intr_sleep_rc, 0x7fffffff,
        "sleep has not returned for the masked consumer");

    // Cleanup leg = death overrides the mask: publish group_exit_msg + wake
    // (the hand-rolled cascade-equivalent the death test uses; the full
    // proc_group_terminate would broadcast an IPI and wake idle secondaries).
    __atomic_store_n(&g_intr_proc->group_exit_msg, "killed", __ATOMIC_RELEASE);
    (void)wakeup(&g_intr_rendez);
    TEST_YIELD_UNTIL(g_intr_run_cnt >= 2u);

    TEST_EXPECT_EQ(g_intr_run_cnt, 2u, "death-wake resumed the masked consumer");
    TEST_EXPECT_EQ(g_intr_sleep_rc, SLEEP_INTR,
        "death overrides the interrupt mask (SLEEP_INTR)");

    test_kthread_join_free(consumer, &g_intr_exited);
    g_intr_proc->state = PROC_STATE_ZOMBIE;
    proc_free(g_intr_proc);
    g_intr_proc = NULL;
}

// The tsleep leg: the deadline-bounded sleep takes the same widened
// register-then-observe + resume-path checks (the surface `/sleep` blocks
// in, via torpor). A far-deadline tsleep'er is woken by the terminate wake
// and returns TSLEEP_INTR long before its deadline.
static void intr_tsleep_consumer_entry(void) {
    g_intr_run_cnt++;                            // -> 1: pre-sleep run
    g_intr_sleep_rc = tsleep(&g_intr_rendez, death_cond_false, NULL,
                             timer_now_ns() + 60ull * 1000000000ull);
    g_intr_run_cnt++;                            // -> 2: post-INTR run
    test_kthread_park_terminal(&g_intr_exited);  // #109: EXITING park
}

void test_rendez_intr_terminate_interrupts_tsleep(void) {
    g_intr_run_cnt  = 0;
    g_intr_sleep_rc = 0x7fffffff;
    g_intr_exited   = false;
    rendez_init(&g_intr_rendez);

    g_intr_proc = proc_alloc();
    TEST_ASSERT(g_intr_proc != NULL, "proc_alloc failed");

    struct Thread *consumer =
        thread_create(g_intr_proc, intr_tsleep_consumer_entry);
    TEST_ASSERT(consumer != NULL, "thread_create(consumer) failed");
    ready(consumer);
    TEST_YIELD_UNTIL(g_intr_run_cnt >= 1u && consumer->state == THREAD_SLEEPING);

    TEST_EXPECT_EQ(g_intr_run_cnt, 1u, "consumer ran once before tsleeping");
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer parked on the far-deadline tsleep");

    TEST_EXPECT_EQ(notes_post(g_intr_proc, "interrupt", 0u, NULL, true), 0,
        "interrupt post accepted");
    irq_state_t s = proc_table_lock_acquire();
    proc_interrupt_terminate_wake(g_intr_proc);
    proc_table_lock_release(s);
    TEST_YIELD_UNTIL(g_intr_run_cnt >= 2u);

    TEST_EXPECT_EQ(g_intr_run_cnt, 2u, "consumer resumed after the wake");
    TEST_EXPECT_EQ(g_intr_sleep_rc, TSLEEP_INTR,
        "tsleep returned TSLEEP_INTR for the terminate-pending Proc (LS-5c)");
    TEST_ASSERT(consumer->rendez_blocked_on == NULL,
        "consumer cleared its backref on the interrupted resume");

    test_kthread_join_free(consumer, &g_intr_exited);
    g_intr_proc->state = PROC_STATE_ZOMBIE;
    proc_free(g_intr_proc);
    g_intr_proc = NULL;
}

// ---------------------------------------------------------------------------
// rendez.death_only_* + rendez.stopped_sleeper_holds_latch +
// rendez.{latch,stop}_wake_skips_stop_park -- DEBUG-FS-DESIGN 5g:
// sleep_death_only, the sleep of the stop parks and the parent suspends,
// returns for group death alone. A terminate latch's wake that reaches it is
// absorbed: it re-checks its condition and sleeps again. The latch's wake walk
// passes over a thread in a stop park altogether, since that park could only
// absorb it, and so does the stop cascade's sleeper walk. Each test records
// what it saw, reaps its threads, and only then asserts, so a failing leg does
// not strand a thread for the tests after it.
// ---------------------------------------------------------------------------

static void death_only_consumer_entry(void) {
    g_intr_run_cnt++;                            // -> 1: pre-sleep run
    g_intr_sleep_rc = sleep_death_only(&g_intr_rendez, death_cond_false, NULL);
    g_intr_run_cnt++;                            // -> 2: post-INTR run
    test_kthread_park_terminal(&g_intr_exited);
}

// Group death, hand-rolled as the death test does it (proc_group_terminate
// would broadcast an IPI and wake the idle secondaries): publish the message,
// then wake whatever rendez the thread sleeps on.
static void intr_publish_death_and_wake(struct Thread *t) {
    __atomic_store_n(&g_intr_proc->group_exit_msg, "killed", __ATOMIC_RELEASE);
    irq_state_t ws = spin_lock_irqsave(&t->wait_lock);
    struct Rendez *r = t->rendez_blocked_on;
    if (r) (void)wakeup(r);
    spin_unlock_irqrestore(&t->wait_lock, ws);
}

static void intr_consumer_kill_reap(struct Thread *consumer) {
    intr_publish_death_and_wake(consumer);
    TEST_YIELD_UNTIL_SOFT(g_intr_run_cnt >= 2u);
    test_kthread_join_free(consumer, &g_intr_exited);
    g_intr_proc->state = PROC_STATE_ZOMBIE;
    proc_free(g_intr_proc);
    g_intr_proc = NULL;
}

static bool intr_consumer_parked_on(struct Thread *consumer, struct Rendez *r) {
    return __atomic_load_n(&consumer->state, __ATOMIC_ACQUIRE) == THREAD_SLEEPING &&
           consumer->rendez_blocked_on == r;
}

// The thread's dispatch count: stored atomically at every switch-in for
// cross-thread readers (thread.h nsched), so it moves exactly when it runs.
static u64 intr_stamp(struct Thread *t) {
    return __atomic_load_n(&t->nsched, __ATOMIC_RELAXED);
}

// Yield for a window in which a thread readied onto this CPU would be run: the
// scheduler's tick is milliseconds, and this thread gives the CPU away at once.
#define INTR_QUIET_NS (20ull * 1000ull * 1000ull)
static void intr_yield_window(void) {
    u64 deadline = timer_now_ns() + INTR_QUIET_NS;
    while (timer_now_ns() < deadline)
        sched();
}

// The latch lands while the thread sleeps: its wake readies the thread, which
// re-checks and sleeps again. A woken thread is switched in, so its stamp moves
// (monotonic, unlike its state, which reads SLEEPING again once it re-sleeps).
// Group death then returns it.
void test_rendez_death_only_absorbs_latch(void) {
    g_intr_run_cnt  = 0;
    g_intr_sleep_rc = 0x7fffffff;
    g_intr_exited   = false;
    rendez_init(&g_intr_rendez);

    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree must be empty at test entry");
    g_intr_proc = proc_alloc();
    TEST_ASSERT(g_intr_proc != NULL, "proc_alloc failed");
    struct Thread *consumer = thread_create(g_intr_proc, death_only_consumer_entry);
    TEST_ASSERT(consumer != NULL, "thread_create(consumer) failed");
    ready(consumer);
    TEST_YIELD_UNTIL_SOFT(g_intr_run_cnt >= 1u &&
                          intr_consumer_parked_on(consumer, &g_intr_rendez));
    bool slept = g_intr_run_cnt == 1u && intr_consumer_parked_on(consumer, &g_intr_rendez);
    u64 since = intr_stamp(consumer);

    bool latched = notes_post(g_intr_proc, "interrupt", 0u, NULL, true) == 0 &&
                   proc_intr_terminate_pending(g_intr_proc);
    irq_state_t s = proc_table_lock_acquire();
    proc_interrupt_terminate_wake(g_intr_proc);
    proc_table_lock_release(s);
    TEST_YIELD_UNTIL_SOFT(g_intr_run_cnt >= 2u ||
                          (intr_stamp(consumer) != since &&
                           intr_consumer_parked_on(consumer, &g_intr_rendez)));
    bool woken   = intr_stamp(consumer) != since;
    bool reslept = g_intr_run_cnt == 1u &&
                   intr_consumer_parked_on(consumer, &g_intr_rendez);
    int rc_latched = g_intr_sleep_rc;

    intr_consumer_kill_reap(consumer);
    int rc_death = g_intr_sleep_rc;

    TEST_ASSERT(slept, "premise: the consumer slept in sleep_death_only");
    TEST_ASSERT(latched, "premise: the post armed the terminate latch");
    TEST_ASSERT(woken, "premise: the latch's wake ran the sleeper");
    TEST_ASSERT(reslept,
        "the latch did not return sleep_death_only: the thread slept again");
    TEST_EXPECT_EQ(rc_latched, 0x7fffffff, "nothing returned before the death");
    TEST_EXPECT_EQ(rc_death, SLEEP_INTR, "group death returned it (SLEEP_INTR)");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after cleanup");
}

// The latch is armed before the thread sleeps. sleep() returns at once for that
// (rendez.intr_terminate_register_observe); the death-only sleep parks, and
// group death still ends it.
void test_rendez_death_only_sleeps_past_latch(void) {
    g_intr_run_cnt  = 0;
    g_intr_sleep_rc = 0x7fffffff;
    g_intr_exited   = false;
    rendez_init(&g_intr_rendez);

    g_intr_proc = proc_alloc();
    TEST_ASSERT(g_intr_proc != NULL, "proc_alloc failed");
    bool latched = notes_post(g_intr_proc, "interrupt", 0u, NULL, true) == 0 &&
                   proc_intr_terminate_pending(g_intr_proc);
    struct Thread *consumer = thread_create(g_intr_proc, death_only_consumer_entry);
    TEST_ASSERT(consumer != NULL, "thread_create(consumer) failed");
    ready(consumer);
    TEST_YIELD_UNTIL_SOFT(g_intr_run_cnt >= 2u ||
                          intr_consumer_parked_on(consumer, &g_intr_rendez));
    bool parked = g_intr_run_cnt == 1u &&
                  intr_consumer_parked_on(consumer, &g_intr_rendez);

    intr_consumer_kill_reap(consumer);
    int rc_death = g_intr_sleep_rc;

    TEST_ASSERT(latched, "premise: the latch was armed before the thread slept");
    TEST_ASSERT(parked,
        "a latch armed before the sleep did not return sleep_death_only: the "
        "thread parked");
    TEST_EXPECT_EQ(rc_death, SLEEP_INTR, "group death returned it (SLEEP_INTR)");
}

static void stop_consumer_entry(void) {
    g_intr_run_cnt++;                            // -> 1: pre-sleep run
    g_intr_sleep_rc = sleep(&g_intr_rendez, death_cond_false, NULL);
    g_intr_run_cnt++;                            // -> 2: post-INTR run
    test_kthread_park_terminal(&g_intr_exited);
}

// A job stop, hand-rolled as its cascade does it for a sleeper: the RELEASE
// store of the flag, then a wake of the rendez the thread sleeps on.
static void intr_job_stop_sleeper(struct Rendez *r) {
    __atomic_store_n(&g_intr_proc->job_stop_req, 1, __ATOMIC_RELEASE);
    (void)wakeup(r);
}

// The nested stop park (DEBUG-FS-DESIGN 5c.2): a thread asleep in an ordinary
// sleep is stopped and detours to park on its own debug_rendez. A latch lands:
// the thread stays stopped. Once the stop clears, the ordinary sleep re-checks,
// and its own die-check unwinds for the latch.
void test_rendez_stopped_sleeper_holds_latch(void) {
    g_intr_run_cnt  = 0;
    g_intr_sleep_rc = 0x7fffffff;
    g_intr_exited   = false;
    rendez_init(&g_intr_rendez);

    g_intr_proc = proc_alloc();
    TEST_ASSERT(g_intr_proc != NULL, "proc_alloc failed");
    struct Thread *consumer = thread_create(g_intr_proc, stop_consumer_entry);
    TEST_ASSERT(consumer != NULL, "thread_create(consumer) failed");
    ready(consumer);
    TEST_YIELD_UNTIL_SOFT(g_intr_run_cnt >= 1u &&
                          intr_consumer_parked_on(consumer, &g_intr_rendez));
    bool slept = g_intr_run_cnt == 1u && intr_consumer_parked_on(consumer, &g_intr_rendez);

    intr_job_stop_sleeper(&g_intr_rendez);
    TEST_YIELD_UNTIL_SOFT(g_intr_run_cnt >= 2u ||
                          intr_consumer_parked_on(consumer, &consumer->debug_rendez));
    bool stop_parked = intr_consumer_parked_on(consumer, &consumer->debug_rendez);

    bool latched = notes_post(g_intr_proc, "interrupt", 0u, NULL, true) == 0 &&
                   proc_intr_terminate_pending(g_intr_proc);
    irq_state_t s = proc_table_lock_acquire();
    proc_interrupt_terminate_wake(g_intr_proc);
    proc_table_lock_release(s);
    intr_yield_window();
    bool held = g_intr_run_cnt == 1u &&
                intr_consumer_parked_on(consumer, &consumer->debug_rendez);

    __atomic_store_n(&g_intr_proc->job_stop_req, 0, __ATOMIC_RELEASE);
    (void)wakeup(&consumer->debug_rendez);
    TEST_YIELD_UNTIL_SOFT(g_intr_run_cnt >= 2u);
    u32 run_cleared = g_intr_run_cnt;
    int rc_cleared  = g_intr_sleep_rc;

    intr_consumer_kill_reap(consumer);

    TEST_ASSERT(slept, "premise: the consumer slept in an ordinary sleep");
    TEST_ASSERT(stop_parked, "premise: the stop parked the sleeper on its debug_rendez");
    TEST_ASSERT(latched, "premise: the post armed the terminate latch");
    TEST_ASSERT(held, "the latch left the stopped sleeper parked on its debug_rendez");
    TEST_EXPECT_EQ(run_cleared, 2u,
        "once the stop cleared, the ordinary sleep returned for the latch");
    TEST_EXPECT_EQ(rc_cleared, SLEEP_INTR, "it returned SLEEP_INTR, the latch's unwind");
}

// The skip tests' control thread: a death-only sleep outside any stop park,
// which the walk under test must reach.
static volatile u32  g_skip_run_cnt;
static volatile int  g_skip_sleep_rc;
static struct Rendez g_skip_rendez;
static volatile bool g_skip_exited;

static void skip_control_entry(void) {
    g_skip_run_cnt++;                            // -> 1: pre-sleep run
    g_skip_sleep_rc = sleep_death_only(&g_skip_rendez, death_cond_false, NULL);
    g_skip_run_cnt++;                            // -> 2: post-INTR run
    test_kthread_park_terminal(&g_skip_exited);
}

// The latch's walk, after the post that arms it.
static bool skip_walk_latch(void) {
    bool latched = notes_post(g_intr_proc, "interrupt", 0u, NULL, true) == 0 &&
                   proc_intr_terminate_pending(g_intr_proc);
    irq_state_t s = proc_table_lock_acquire();
    proc_interrupt_terminate_wake(g_intr_proc);
    proc_table_lock_release(s);
    return latched;
}

// A debugger's stop over the job stop already in force, by the real cascade:
// its sleeper walk is the walk under test.
static bool skip_walk_stop(void) {
    irq_state_t s = proc_table_lock_acquire();
    proc_debug_stop_deliver(g_intr_proc);
    proc_table_lock_release(s);
    return __atomic_load_n(&g_intr_proc->debug_stop_req, __ATOMIC_ACQUIRE) != 0;
}

// Two threads of one Proc, one variable apart: the stopped one sleeps in its
// stop park, the control in a death-only sleep outside one. The walk reaches
// the control -- it runs, absorbs the wake, and meets the stop in its own
// detour -- and passes over the stopped thread, which is never switched in: a
// stop park could only absorb the wake, and running it for that would unsettle
// a confirmed stop.
static void skip_stop_park_run(bool (*walk)(void), const char *armed_msg) {
    g_intr_run_cnt  = 0;
    g_intr_sleep_rc = 0x7fffffff;
    g_intr_exited   = false;
    rendez_init(&g_intr_rendez);
    g_skip_run_cnt  = 0;
    g_skip_sleep_rc = 0x7fffffff;
    g_skip_exited   = false;
    rendez_init(&g_skip_rendez);

    g_intr_proc = proc_alloc();
    TEST_ASSERT(g_intr_proc != NULL, "proc_alloc failed");
    struct Thread *stopped = thread_create(g_intr_proc, stop_consumer_entry);
    TEST_ASSERT(stopped != NULL, "thread_create(stopped) failed");
    struct Thread *control = thread_create(g_intr_proc, skip_control_entry);
    TEST_ASSERT(control != NULL, "thread_create(control) failed");
    ready(stopped);
    ready(control);
    TEST_YIELD_UNTIL_SOFT(intr_consumer_parked_on(stopped, &g_intr_rendez) &&
                          intr_consumer_parked_on(control, &g_skip_rendez));
    bool slept = g_intr_run_cnt == 1u && g_skip_run_cnt == 1u &&
                 intr_consumer_parked_on(stopped, &g_intr_rendez) &&
                 intr_consumer_parked_on(control, &g_skip_rendez);

    intr_job_stop_sleeper(&g_intr_rendez);
    TEST_YIELD_UNTIL_SOFT(g_intr_run_cnt >= 2u ||
                          intr_consumer_parked_on(stopped, &stopped->debug_rendez));
    bool stop_parked = intr_consumer_parked_on(stopped, &stopped->debug_rendez);
    u64 stopped_since = intr_stamp(stopped);
    u64 control_since = intr_stamp(control);

    bool armed = walk();
    TEST_YIELD_UNTIL_SOFT(g_skip_run_cnt >= 2u ||
                          (intr_stamp(control) != control_since &&
                           intr_consumer_parked_on(control, &control->debug_rendez)));
    intr_yield_window();
    bool reached = g_skip_run_cnt == 1u && intr_stamp(control) != control_since &&
                   intr_consumer_parked_on(control, &control->debug_rendez);
    bool passed = g_intr_run_cnt == 1u && intr_stamp(stopped) == stopped_since &&
                  intr_consumer_parked_on(stopped, &stopped->debug_rendez);

    intr_publish_death_and_wake(stopped);
    intr_publish_death_and_wake(control);
    TEST_YIELD_UNTIL_SOFT(g_intr_run_cnt >= 2u && g_skip_run_cnt >= 2u);
    int rc_stopped = g_intr_sleep_rc;
    int rc_control = g_skip_sleep_rc;
    test_kthread_join_free(stopped, &g_intr_exited);
    test_kthread_join_free(control, &g_skip_exited);
    g_intr_proc->state = PROC_STATE_ZOMBIE;
    proc_free(g_intr_proc);
    g_intr_proc = NULL;

    TEST_ASSERT(slept, "premise: both threads slept");
    TEST_ASSERT(stop_parked, "premise: the stop parked one thread on its debug_rendez");
    TEST_ASSERT(armed, armed_msg);
    TEST_ASSERT(reached,
        "control: the walk ran the thread outside a stop park, which absorbed "
        "the wake and parked for the stop");
    TEST_ASSERT(passed,
        "the walk passed over the thread in its stop park: never switched in");
    TEST_EXPECT_EQ(rc_stopped, SLEEP_INTR, "group death returned the stopped thread");
    TEST_EXPECT_EQ(rc_control, SLEEP_INTR, "group death returned the control");
}

void test_rendez_latch_wake_skips_stop_park(void) {
    skip_stop_park_run(skip_walk_latch, "premise: the post armed the terminate latch");
}

// A second stop changes nothing a stop park waits on, so the stop cascade's
// sleeper walk passes it over too.
void test_rendez_stop_wake_skips_stop_park(void) {
    skip_stop_park_run(skip_walk_stop, "premise: the debugger's stop was delivered");
}

// ---------------------------------------------------------------------------
// rendez.exit_close_ignores_stop + rendez.exit_close_park_ends_on_death --
// DEBUG-FS-DESIGN 5g, death wins in the exit close. A dying Proc's closer reads
// no death in its sleeps (exit_close_active), and group death clears no stop,
// so the park predicate answers false in a dying group: a closer there never
// parks for a stop, and one that parked while its group lived leaves the park
// when the group dies. Record, reap, then assert, as above.
// ---------------------------------------------------------------------------

static volatile bool g_ec_done;

static int ec_cond(void *arg) {
    (void)arg;
    return __atomic_load_n(&g_ec_done, __ATOMIC_ACQUIRE);
}

// A closer: the ordinary sleep of a close hook, under exit_close_active.
static void exit_close_consumer_entry(void) {
    struct Thread *self = current_thread();
    self->exit_close_active = true;
    g_intr_run_cnt++;                            // -> 1: pre-sleep run
    g_intr_sleep_rc = sleep(&g_intr_rendez, ec_cond, NULL);
    self->exit_close_active = false;
    g_intr_run_cnt++;                            // -> 2: post-sleep run
    test_kthread_park_terminal(&g_intr_exited);
}

// Whatever the run saw, end the closer's sleep and reap it: the close's
// condition turns true, the stop clears, and both rendez are woken.
static void exit_close_consumer_reap(struct Thread *consumer) {
    __atomic_store_n(&g_ec_done, true, __ATOMIC_RELEASE);
    __atomic_store_n(&g_intr_proc->job_stop_req, 0, __ATOMIC_RELEASE);
    (void)wakeup(&consumer->debug_rendez);
    (void)wakeup(&g_intr_rendez);
    TEST_YIELD_UNTIL_SOFT(g_intr_run_cnt >= 2u);
    test_kthread_join_free(consumer, &g_intr_exited);
    g_intr_proc->state = PROC_STATE_ZOMBIE;
    proc_free(g_intr_proc);
    g_intr_proc = NULL;
}

static void exit_close_setup(void) {
    g_intr_run_cnt  = 0;
    g_intr_sleep_rc = 0x7fffffff;
    g_intr_exited   = false;
    g_ec_done       = false;
    rendez_init(&g_intr_rendez);
}

// A closer in a dying group, with a job stop pending, sleeps in its close: the
// stop does not park it, and the close's own wake returns it.
void test_rendez_exit_close_ignores_stop(void) {
    exit_close_setup();
    g_intr_proc = proc_alloc();
    TEST_ASSERT(g_intr_proc != NULL, "proc_alloc failed");
    __atomic_store_n(&g_intr_proc->job_stop_req, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_intr_proc->group_exit_msg, "killed", __ATOMIC_RELEASE);
    struct Thread *consumer = thread_create(g_intr_proc, exit_close_consumer_entry);
    TEST_ASSERT(consumer != NULL, "thread_create(consumer) failed");
    ready(consumer);
    TEST_YIELD_UNTIL_SOFT(g_intr_run_cnt >= 1u &&
                          (intr_consumer_parked_on(consumer, &g_intr_rendez) ||
                           intr_consumer_parked_on(consumer, &consumer->debug_rendez)));
    bool in_close = g_intr_run_cnt == 1u && intr_consumer_parked_on(consumer, &g_intr_rendez);
    bool parked   = intr_consumer_parked_on(consumer, &consumer->debug_rendez);

    u32 run_woken = 0;
    int rc_woken  = 0x7fffffff;
    if (in_close) {
        __atomic_store_n(&g_ec_done, true, __ATOMIC_RELEASE);
        (void)wakeup(&g_intr_rendez);
        TEST_YIELD_UNTIL_SOFT(g_intr_run_cnt >= 2u);
        run_woken = g_intr_run_cnt;
        rc_woken  = g_intr_sleep_rc;
    }

    exit_close_consumer_reap(consumer);

    TEST_ASSERT(in_close || parked, "premise: the closer ran and slept");
    TEST_ASSERT(!parked, "the stop did not park the dying Proc's closer");
    TEST_EXPECT_EQ(run_woken, 2u, "the close's wake returned the closer");
    TEST_EXPECT_EQ(rc_woken, SLEEP_OK, "it returned SLEEP_OK: the close goes on");
}

// A closer whose group still lives honours a stop: it parks. The group then
// dies, and the death cascade's wake ends the park -- the closer sleeps on in
// its close, and the close's own wake returns it.
void test_rendez_exit_close_park_ends_on_death(void) {
    exit_close_setup();
    g_intr_proc = proc_alloc();
    TEST_ASSERT(g_intr_proc != NULL, "proc_alloc failed");
    __atomic_store_n(&g_intr_proc->job_stop_req, 1, __ATOMIC_RELEASE);
    struct Thread *consumer = thread_create(g_intr_proc, exit_close_consumer_entry);
    TEST_ASSERT(consumer != NULL, "thread_create(consumer) failed");
    ready(consumer);
    TEST_YIELD_UNTIL_SOFT(g_intr_run_cnt >= 1u &&
                          intr_consumer_parked_on(consumer, &consumer->debug_rendez));
    bool parked = g_intr_run_cnt == 1u &&
                  intr_consumer_parked_on(consumer, &consumer->debug_rendez);

    bool left = false;
    u32 run_woken = 0;
    int rc_woken  = 0x7fffffff;
    if (parked) {
        u64 since = intr_stamp(consumer);
        intr_publish_death_and_wake(consumer);
        // The woken closer runs, so its stamp moves, and sleeps again: in its
        // close, or back in the park.
        TEST_YIELD_UNTIL_SOFT(g_intr_run_cnt >= 2u ||
                              (intr_stamp(consumer) != since &&
                               (intr_consumer_parked_on(consumer, &g_intr_rendez) ||
                                intr_consumer_parked_on(consumer, &consumer->debug_rendez))));
        left = g_intr_run_cnt == 1u && intr_consumer_parked_on(consumer, &g_intr_rendez);
    }
    if (left) {
        __atomic_store_n(&g_ec_done, true, __ATOMIC_RELEASE);
        (void)wakeup(&g_intr_rendez);
        TEST_YIELD_UNTIL_SOFT(g_intr_run_cnt >= 2u);
        run_woken = g_intr_run_cnt;
        rc_woken  = g_intr_sleep_rc;
    }

    exit_close_consumer_reap(consumer);

    TEST_ASSERT(parked, "premise: the live group's closer parked for the stop");
    TEST_ASSERT(left,
        "the group's death ended the closer's stop park: it sleeps on in its close");
    TEST_EXPECT_EQ(run_woken, 2u, "the close's wake returned the closer");
    TEST_EXPECT_EQ(rc_woken, SLEEP_OK, "it returned SLEEP_OK: the close goes on");
}

// ---------------------------------------------------------------------------
// rendez.reader_frame_predicate -- #90 (ARCH 8.8.1.1): the frame-atomic
// reader-recv guard truth table.
// ---------------------------------------------------------------------------
//
// thread_reader_blocks_death(t) == stop_no_park && !stop_unwinds -- true iff a
// die-check must DEFER (block through) rather than unwind. Pins the full table,
// including the `|| stop_unwinds` disjunct of the die-check guard: a reader AT a
// boundary (stop_unwinds set) must still unwind, so dropping that disjunct
// (block-through even at got==0) would leave a dying reader unable to ever
// unwind. Pure -- drive it on the boot thread's own latches, restoring them.
void test_rendez_reader_frame_predicate(void) {
    struct Thread *t = current_thread();
    bool save_np = t->stop_no_park, save_uw = t->stop_unwinds;

    // Non-reader (stop_no_park clear): NEVER blocks -- the die-check fires
    // immediately for every ordinary sleeper, exactly as before #90.
    t->stop_no_park = false; t->stop_unwinds = false;
    TEST_ASSERT(!thread_reader_blocks_death(t),
        "non-reader (no frame-atomic recv) never blocks death");
    t->stop_no_park = false; t->stop_unwinds = true;
    TEST_ASSERT(!thread_reader_blocks_death(t),
        "non-reader never blocks death even with stop_unwinds set");

    // Reader AT a boundary (stop_unwinds set, got==0): unwinds -- a safe point,
    // no partial frame to discard. This is the `|| stop_unwinds` disjunct.
    t->stop_no_park = true; t->stop_unwinds = true;
    TEST_ASSERT(!thread_reader_blocks_death(t),
        "reader at a frame boundary unwinds (does not block through)");

    // Reader MID-FRAME (stop_no_park set, stop_unwinds clear): BLOCKS THROUGH.
    t->stop_no_park = true; t->stop_unwinds = false;
    TEST_ASSERT(thread_reader_blocks_death(t),
        "reader mid-frame blocks the death through");

    t->stop_no_park = save_np; t->stop_unwinds = save_uw;
}

// ---------------------------------------------------------------------------
// rendez.reader_frame_blocks_death -- #90 (ARCH 8.8.1.1): the frame-atomic
// reader-recv death block-through, end to end.
// ---------------------------------------------------------------------------
//
// The elected 9P reader recv (kernel/9p_client.c::reader_recv_frame) is
// frame-atomic w.r.t. the #811 death-unwind. A dying reader observed
// MID-FRAME (stop_no_park set + stop_unwinds clear -- bytes of the current 9P
// frame already consumed) must NOT unwind at the die-check: an immediate
// unwind discards the partial frame, and the survivor that takes over the
// reader role reads the frame TAIL as a header -> the shared byte stream
// desyncs (task-#50). It BLOCKS THROUGH -- the die-check falls to
// register+sched, the reader finishes the frame (bounded by the trusted
// server, CF-3 B), and unwinds only at the next boundary.
//
// Forces the block-through leg deterministically: a mid-frame reader whose
// Proc is ALREADY group-terminating (group_exit_msg set before it sleeps)
// tsleeps with a short deadline on a never-true cond. The register-then-
// observe die-check sees death pending; with the #90 guard it BLOCKS THROUGH
// (the reader stays SLEEPING, run_cnt == 1, does NOT unwind), and only the
// DEADLINE (via the timer-tick scan) later returns TSLEEP_TIMEDOUT.
// REVERT-PROBE: dropping the `!thread_reader_blocks_death(t)` guard makes the
// die-check return TSLEEP_INTR on the first pass -- the reader never sleeps
// (run_cnt reaches 2 immediately, state != SLEEPING), failing the SLEEPING
// assertions below.

static volatile int  g_rf_run_cnt;
static volatile int  g_rf_sleep_rc;
static volatile bool g_rf_exited;
static struct Rendez g_rf_rendez;
static struct Proc  *g_rf_proc;

static int rf_cond_false(void *arg) { (void)arg; return 0; }

static void rf_reader_consumer_entry(void) {
    struct Thread *self = current_thread();
    // Simulate the elected reader MID-FRAME: in a frame-atomic recv
    // (stop_no_park) with bytes of the frame already consumed (stop_unwinds
    // clear). reader_recv_frame maintains exactly these two latches.
    self->stop_no_park = true;
    self->stop_unwinds = false;
    g_rf_run_cnt++;                              // -> 1: pre-tsleep
    // Short deadline; cond never true; the Proc is already group-terminating.
    // The #90 guard blocks the death through -> the deadline wins (TIMEDOUT).
    g_rf_sleep_rc = tsleep(&g_rf_rendez, rf_cond_false, NULL,
                           timer_now_ns() + 10ull * 1000000ull);   // +10 ms
    // Past the recv -> clear the reader latches before the terminal park.
    self->stop_no_park = false;
    self->stop_unwinds = false;
    g_rf_run_cnt++;                              // -> 2: post-tsleep
    test_kthread_park_terminal(&g_rf_exited);
}

void test_rendez_reader_frame_blocks_death(void) {
    g_rf_run_cnt  = 0;
    g_rf_sleep_rc = 0x7fffffff;
    g_rf_exited   = false;
    rendez_init(&g_rf_rendez);

    g_rf_proc = proc_alloc();
    TEST_ASSERT(g_rf_proc != NULL, "proc_alloc failed");

    // Death is pending BEFORE the reader sleeps: the register-then-observe
    // die-check on the first tsleep pass is the one that must block through.
    __atomic_store_n(&g_rf_proc->group_exit_msg, "killed", __ATOMIC_RELEASE);

    struct Thread *consumer =
        thread_create(g_rf_proc, rf_reader_consumer_entry);
    TEST_ASSERT(consumer != NULL, "thread_create(consumer) failed");
    ready(consumer);
    sched();

    // The revert-probe: with the #90 guard the mid-frame reader BLOCKED
    // THROUGH the pending death and is SLEEPING (run_cnt still 1). Without it,
    // the die-check unwound on the first pass (run_cnt == 2, not SLEEPING).
    TEST_EXPECT_EQ(g_rf_run_cnt, 1u,
        "mid-frame reader blocked the death through -- did not unwind");
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "mid-frame reader is SLEEPING (blocked through, not unwound)");
    TEST_EXPECT_EQ(g_rf_rendez.waiter, consumer,
        "mid-frame reader registered on the rendez (blocked through)");

    // Spin yielding until the timer-tick scan expires the +10 ms deadline and
    // wakes the reader, which -- the timeout check precedes the die-check on
    // resume -- returns TSLEEP_TIMEDOUT. The cap turns a stuck block-through
    // into a clean failure rather than a hang.
    for (u64 i = 0; i < 100000000ull && g_rf_run_cnt < 2u; i++) sched();

    TEST_EXPECT_EQ(g_rf_run_cnt, 2u,
        "reader timed out and ran to completion");
    TEST_EXPECT_EQ(g_rf_sleep_rc, TSLEEP_TIMEDOUT,
        "the death blocked through -> the deadline won (TSLEEP_TIMEDOUT), "
        "not an immediate TSLEEP_INTR unwind (#90)");
    TEST_ASSERT(consumer->rendez_blocked_on == NULL,
        "reader cleared its backref on the timeout resume");

    test_kthread_join_free(consumer, &g_rf_exited);
    g_rf_proc->state = PROC_STATE_ZOMBIE;
    proc_free(g_rf_proc);
    g_rf_proc = NULL;
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree empty after cleanup");
}

// ---------------------------------------------------------------------------
// rendez.reader_frame_blocks_death_sleep -- #90 (ARCH 8.8.1.1): the frame-atomic
// death block-through on the sleep() path (the PRODUCTION path -- #90-audit F1).
// ---------------------------------------------------------------------------
//
// The steady-state elected reader recv is reader_recv_frame(c, 0, NULL) --
// deadline_ns == 0, so srvconn_client_recv's tsleep DEGRADES to sleep(). So the
// production mid-frame block-through actually fires the sleep() die-check guards
// (sched.c register-then-observe + resume-path), NOT the tsleep guards the
// deadline-terminated reader_frame_blocks_death above exercises. This sibling
// revert-probes BOTH sleep() sites, terminated by a REAL producer wakeup() (not
// a deadline): a mid-frame reader whose Proc is group-terminating sleep()s on a
// never-yet-true cond, blocks the pending death through (stays SLEEPING), and
// when woken with cond now true blocks the STILL-pending death through the
// resume-path check and returns SLEEP_OK. REVERT-PROBE: dropping the
// register-then-observe guard unwinds on the first pass (run_cnt==2, not
// SLEEPING); dropping the resume-path guard unwinds on the cond wake
// (SLEEP_INTR, not SLEEP_OK).

static volatile int  g_rfs_run_cnt;
static volatile int  g_rfs_sleep_rc;
static volatile int  g_rfs_cond;
static volatile bool g_rfs_exited;
static struct Rendez g_rfs_rendez;
static struct Proc  *g_rfs_proc;

static int rfs_cond_check(void *arg) { (void)arg; return g_rfs_cond; }

static void rfs_reader_consumer_entry(void) {
    struct Thread *self = current_thread();
    self->stop_no_park = true;
    self->stop_unwinds = false;
    g_rfs_run_cnt++;                             // -> 1: pre-sleep
    g_rfs_sleep_rc = sleep(&g_rfs_rendez, rfs_cond_check, NULL);
    self->stop_no_park = false;
    self->stop_unwinds = false;
    g_rfs_run_cnt++;                             // -> 2: post-sleep
    test_kthread_park_terminal(&g_rfs_exited);
}

void test_rendez_reader_frame_blocks_death_sleep(void) {
    g_rfs_run_cnt  = 0;
    g_rfs_sleep_rc = 0x7fffffff;
    g_rfs_cond     = 0;
    g_rfs_exited   = false;
    rendez_init(&g_rfs_rendez);

    g_rfs_proc = proc_alloc();
    TEST_ASSERT(g_rfs_proc != NULL, "proc_alloc failed");

    // Death pending BEFORE the reader sleeps -> the sleep() register-then-observe
    // die-check must block through.
    __atomic_store_n(&g_rfs_proc->group_exit_msg, "killed", __ATOMIC_RELEASE);

    struct Thread *consumer =
        thread_create(g_rfs_proc, rfs_reader_consumer_entry);
    TEST_ASSERT(consumer != NULL, "thread_create(consumer) failed");
    ready(consumer);
    TEST_YIELD_UNTIL(g_rfs_run_cnt >= 1u && consumer->state == THREAD_SLEEPING);

    // Blocked through the pending death on the register-then-observe check
    // (SLEEPING, run_cnt==1); a bug there would unwind (run_cnt==2, not SLEEPING).
    TEST_EXPECT_EQ(g_rfs_run_cnt, 1u,
        "mid-frame reader blocked the death through on the sleep() path");
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "mid-frame reader is SLEEPING (sleep()-path block-through)");
    TEST_EXPECT_EQ(g_rfs_rendez.waiter, consumer,
        "mid-frame reader registered on the rendez");

    // A REAL producer wake with cond TRUE (not a deadline). With the #90
    // resume-path guard the reader blocks the still-pending death through,
    // re-checks cond (now true), and returns SLEEP_OK. Revert-probe: dropping the
    // resume-path guard unwinds it (SLEEP_INTR) on this wake.
    g_rfs_cond = 1;
    int woke = wakeup(&g_rfs_rendez);
    TEST_EXPECT_EQ(woke, 1, "wakeup reported exactly one waiter");
    TEST_YIELD_UNTIL(g_rfs_run_cnt >= 2u);

    TEST_EXPECT_EQ(g_rfs_run_cnt, 2u, "reader resumed to completion");
    TEST_EXPECT_EQ(g_rfs_sleep_rc, SLEEP_OK,
        "the death blocked through on BOTH sleep() sites -> the cond wake won "
        "(SLEEP_OK), not an immediate SLEEP_INTR unwind (#90 production path)");
    TEST_ASSERT(consumer->rendez_blocked_on == NULL,
        "reader cleared its backref on the cond resume");

    test_kthread_join_free(consumer, &g_rfs_exited);
    g_rfs_proc->state = PROC_STATE_ZOMBIE;
    proc_free(g_rfs_proc);
    g_rfs_proc = NULL;
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree empty after cleanup");
}

// ---------------------------------------------------------------------------
// rendez.caught_wake_* -- item 11 (ARCH 8.8.3): every post that CAUGHT-arms a
// Proc wakes its thread already asleep in an interruptible wait. The sleeper is
// a Linux-phenotype thread in a signal(7)-listed wait (note_interruptible),
// parked BEFORE the post, so its register-then-observe has already run and only
// the post's own wake can reach it. Each path is driven through its production
// entry, with a control one sigtab row away: the same post with nothing caught
// must leave the sleeper asleep. A leg releases and joins its sleeper before
// the test asserts anything, so a RED run leaks no parked thread.
// ---------------------------------------------------------------------------

extern void proc_test_link(struct Proc *p);
extern void proc_test_link_child(struct Proc *parent, struct Proc *p);
extern void proc_test_unlink(struct Proc *p);
extern void proc_test_orphan_rule(struct Proc *dying);

static volatile u32  g_cw_run;
static volatile int  g_cw_rc;
static volatile bool g_cw_release;
static volatile bool g_cw_exited;
static struct Rendez g_cw_rendez;

static int cw_cond(void *arg) { (void)arg; return g_cw_release ? 1 : 0; }

static void cw_sleeper_entry(void) {
    current_thread()->note_interruptible = true;   // a wait on signal(7)'s list
    g_cw_run++;                                    // -> 1: about to sleep
    g_cw_rc = sleep_noteintr(&g_cw_rendez, cw_cond, NULL);
    g_cw_run++;                                    // -> 2: the sleep returned
    test_kthread_park_terminal(&g_cw_exited);
}

struct cw_leg { bool parked; bool woke; bool joined; int rc; };

static struct cw_leg cw_run_leg(struct Proc *p, void (*post)(void *), void *arg) {
    struct cw_leg r = { false, false, false, 0x7fffffff };
    g_cw_run = 0; g_cw_rc = 0x7fffffff; g_cw_release = false; g_cw_exited = false;
    rendez_init(&g_cw_rendez);
    struct Thread *t = thread_create(p, cw_sleeper_entry);
    if (!t) return r;
    ready(t);
    u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
    while (!(g_cw_run >= 1u && t->state == THREAD_SLEEPING) && timer_now_ns() < dl)
        sched();
    r.parked = g_cw_run == 1u && t->state == THREAD_SLEEPING;
    if (r.parked) {
        post(arg);
        r.woke = t->state != THREAD_SLEEPING;   // the post's own wake readied it
    }
    // A sleeper the post did not wake is released through its cond, which it
    // reads before the caught arm -- so a released sleep ends SLEEP_OK and only
    // the note's wake can produce SLEEP_NOTEINTR.
    if (!r.woke) { g_cw_release = true; (void)wakeup(&g_cw_rendez); }
    dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
    while (g_cw_run < 2u && timer_now_ns() < dl) sched();
    if (g_cw_run < 2u) return r;   // wedged: never free a thread that is still asleep
    r.rc = g_cw_rc;
    test_kthread_join_free(t, &g_cw_exited);
    r.joined = true;
    return r;
}

static const struct viv_ksigaction g_cw_hand = { .handler = 0x4000u, .flags = 0,
                                                 .restorer = 0, .mask = 0 };
static const struct viv_ksigaction g_cw_ign  = { .handler = VIV_SIG_IGN, .flags = 0,
                                                 .restorer = 0, .mask = 0 };

// A Linux-phenotype Proc with an all-SIG_DFL sigtab of its own (proc_free
// frees it).
static struct Proc *cw_linux_proc(void) {
    struct Proc *p = proc_alloc();
    if (!p) return NULL;
    p->sigtab = (struct viv_sigtab *)kzalloc(sizeof(struct viv_sigtab), 0);
    if (!p->sigtab) { p->state = PROC_STATE_ZOMBIE; proc_free(p); return NULL; }
    p->state     = PROC_STATE_ALIVE;
    p->phenotype = PHENO_LINUX;
    return p;
}

static void cw_post_child_exit(void *arg) {
    irq_state_t s = proc_table_lock_acquire();
    proc_exit_notify_parent_locked((struct Proc *)arg);
    proc_table_lock_release(s);
}
static void cw_post_susp(void *arg) { (void)proc_job_stop_pgrp(((struct Proc *)arg)->pgid); }
static void cw_post_cont(void *arg) { (void)proc_job_cont_pgrp(((struct Proc *)arg)->pgid); }
static void cw_post_orphan(void *arg) { proc_test_orphan_rule((struct Proc *)arg); }

// A child's exit posts child_exit to its parent (proc_exit_notify_parent_locked,
// the exit path's own code). A SIGCHLD handler must interrupt the parent's
// blocked slow call -- viv-pheno-probe L305 end to end.
void test_rendez_caught_wake_child_exit(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *par = cw_linux_proc();
    TEST_ASSERT(par != NULL, "parent alloc");
    struct Proc *kid = proc_alloc();
    TEST_ASSERT(kid != NULL, "child alloc");
    kid->parent = par;   // the notify reads only parent, pid and exit_status

    struct cw_leg ctl = cw_run_leg(par, cw_post_child_exit, kid);   // SIGCHLD SIG_DFL
    bool set = viv_sigtab_set(par->sigtab, VIV_SIGNOTE_CHILD_EXIT, &g_cw_hand);
    struct cw_leg leg = { false, false, false, 0 };
    if (set) leg = cw_run_leg(par, cw_post_child_exit, kid);

    kid->parent = NULL;
    kid->state  = PROC_STATE_ZOMBIE;
    proc_free(kid);
    par->state  = PROC_STATE_ZOMBIE;
    proc_free(par);

    TEST_ASSERT(ctl.parked && ctl.joined, "control: the sleeper parked and was released");
    TEST_ASSERT(!ctl.woke, "control: an UNCAUGHT child_exit leaves the sleeper asleep");
    TEST_EXPECT_EQ(ctl.rc, SLEEP_OK, "control: released through its cond");
    TEST_ASSERT(set, "the SIGCHLD handler row was written");
    TEST_ASSERT(leg.parked && leg.joined, "the sleeper parked and returned");
    TEST_ASSERT(leg.woke,
        "a CAUGHT child_exit wakes the parent's thread blocked in an interruptible "
        "wait (ARCH 8.8.3) -- not left for an unrelated wake");
    TEST_EXPECT_EQ(leg.rc, SLEEP_NOTEINTR, "the wait unwinds for the handler");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after cleanup");
}

// ^Z's fan (proc_job_stop_pgrp) posts tty:susp to a member that catches it
// instead of stopping it; the handler must run now.
void test_rendez_caught_wake_tty_susp(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *m = cw_linux_proc();
    TEST_ASSERT(m != NULL, "member alloc");
    m->sid  = 0x5F31u;   // a fabricated session no table Proc shares
    m->pgid = (u32)m->pid;
    proc_test_link(m);

    // The control IGNORES SIGTSTP rather than defaulting it: at SIG_DFL the fan
    // STOPS the Proc -- another path -- while an ignored susp takes the same
    // posting branch as a caught one and arms nothing.
    bool ign = viv_sigtab_set(m->sigtab, VIV_SIGNOTE_TTY_SUSP, &g_cw_ign);
    struct cw_leg ctl = { false, false, false, 0 };
    if (ign) ctl = cw_run_leg(m, cw_post_susp, m);
    bool set = viv_sigtab_set(m->sigtab, VIV_SIGNOTE_TTY_SUSP, &g_cw_hand);
    struct cw_leg leg = { false, false, false, 0 };
    if (set) leg = cw_run_leg(m, cw_post_susp, m);
    u32 stopped = __atomic_load_n(&m->job_stop_req, __ATOMIC_ACQUIRE);

    proc_test_unlink(m);
    m->state = PROC_STATE_ZOMBIE;
    proc_free(m);

    TEST_ASSERT(ign && set, "the SIGTSTP rows were written");
    TEST_ASSERT(ctl.parked && ctl.joined, "control: the sleeper parked and was released");
    TEST_ASSERT(!ctl.woke, "control: an IGNORED tty:susp leaves the sleeper asleep");
    TEST_ASSERT(leg.parked && leg.joined, "the sleeper parked and returned");
    TEST_ASSERT(leg.woke, "a CAUGHT tty:susp wakes the member's interruptible sleeper");
    TEST_EXPECT_EQ(leg.rc, SLEEP_NOTEINTR, "the wait unwinds for the handler");
    TEST_EXPECT_EQ(stopped, 0u, "a caught susp posts; it does not stop");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after cleanup");
}

// The tty:cont fan (proc_job_cont_pgrp) to a member with a SIGCONT handler.
void test_rendez_caught_wake_tty_cont(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *m = cw_linux_proc();
    TEST_ASSERT(m != NULL, "member alloc");
    m->sid  = 0x5F32u;
    m->pgid = (u32)m->pid;
    proc_test_link(m);

    struct cw_leg ctl = cw_run_leg(m, cw_post_cont, m);   // SIGCONT SIG_DFL
    bool set = viv_sigtab_set(m->sigtab, VIV_SIGNOTE_TTY_CONT, &g_cw_hand);
    struct cw_leg leg = { false, false, false, 0 };
    if (set) leg = cw_run_leg(m, cw_post_cont, m);

    proc_test_unlink(m);
    m->state = PROC_STATE_ZOMBIE;
    proc_free(m);

    TEST_ASSERT(ctl.parked && ctl.joined, "control: the sleeper parked and was released");
    TEST_ASSERT(!ctl.woke, "control: an UNCAUGHT tty:cont leaves the sleeper asleep");
    TEST_ASSERT(set, "the SIGCONT handler row was written");
    TEST_ASSERT(leg.parked && leg.joined, "the sleeper parked and returned");
    TEST_ASSERT(leg.woke, "a CAUGHT tty:cont wakes the member's interruptible sleeper");
    TEST_EXPECT_EQ(leg.rc, SLEEP_NOTEINTR, "the wait unwinds for the handler");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after cleanup");
}

// The orphaned-pgrp rule: an exit that orphans a group holding a stopped member
// posts tty:hup THEN tty:cont to every member (POSIX). A member that ignores
// SIGHUP but catches SIGCONT is the case a wake between the two posts misses.
// The stopped member is a SECOND, threadless Proc: a sleeper whose own Proc is
// stopped parks in the stop park instead of the wait under test, and the
// resume would wake it whether or not the note did.
void test_rendez_caught_wake_orphan_hup_cont(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *dying = proc_alloc();
    TEST_ASSERT(dying != NULL, "anchor alloc");
    dying->state = PROC_STATE_ALIVE;
    dying->sid   = 0x5F33u;
    dying->pgid  = (u32)dying->pid;
    proc_test_link(dying);
    struct Proc *m = cw_linux_proc();
    TEST_ASSERT(m != NULL, "member alloc");
    m->sid  = dying->sid;          // same session, its own group: dying anchors it
    m->pgid = (u32)m->pid;
    proc_test_link_child(dying, m);
    struct Proc *stopped = proc_alloc();
    TEST_ASSERT(stopped != NULL, "stopped member alloc");
    stopped->state = PROC_STATE_ALIVE;
    stopped->sid   = m->sid;
    stopped->pgid  = m->pgid;
    proc_test_link_child(dying, stopped);

    bool ign = viv_sigtab_set(m->sigtab, VIV_SIGNOTE_TTY_HUP, &g_cw_ign);
    __atomic_store_n(&stopped->job_stop_req, 1u, __ATOMIC_RELEASE);
    struct cw_leg ctl = { false, false, false, 0 };
    if (ign) ctl = cw_run_leg(m, cw_post_orphan, dying);         // SIGCONT SIG_DFL
    u32 still_ctl = __atomic_load_n(&stopped->job_stop_req, __ATOMIC_ACQUIRE);
    bool set = viv_sigtab_set(m->sigtab, VIV_SIGNOTE_TTY_CONT, &g_cw_hand);
    __atomic_store_n(&stopped->job_stop_req, 1u, __ATOMIC_RELEASE);
    struct cw_leg leg = { false, false, false, 0 };
    if (set) leg = cw_run_leg(m, cw_post_orphan, dying);
    u32 still_leg = __atomic_load_n(&stopped->job_stop_req, __ATOMIC_ACQUIRE);

    proc_test_unlink(stopped);
    stopped->state = PROC_STATE_ZOMBIE;
    proc_free(stopped);
    proc_test_unlink(m);
    m->state = PROC_STATE_ZOMBIE;
    proc_free(m);
    proc_test_unlink(dying);
    dying->state = PROC_STATE_ZOMBIE;
    proc_free(dying);

    TEST_ASSERT(ign && set, "the SIGHUP/SIGCONT rows were written");
    TEST_ASSERT(ctl.parked && ctl.joined, "control: the sleeper parked and was released");
    TEST_EXPECT_EQ(still_ctl, 0u, "control: the rule DID fan out (the stopped member resumed)");
    TEST_ASSERT(!ctl.woke, "control: an ignored hup + an uncaught cont leave it asleep");
    TEST_ASSERT(leg.parked && leg.joined, "the sleeper parked and returned");
    TEST_EXPECT_EQ(still_leg, 0u, "the rule fanned out");
    TEST_ASSERT(leg.woke, "a caught tty:cont after an ignored tty:hup wakes the sleeper");
    TEST_EXPECT_EQ(leg.rc, SLEEP_NOTEINTR, "the wait unwinds for the handler");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after cleanup");
}

// rendez.caught_note_one_unwind -- one caught note unwinds ONE of two
// interruptible sleepers in a Proc, as Linux hands a process-directed signal to
// one thread. The wake reaches both; the sleeper that cannot claim the note
// re-reads its cond and re-parks. The control is one note away: a caught note of
// ANOTHER family then unwinds the re-parked sleeper, so its re-park was the
// claim's doing and not a sleeper that could never unwind.
static volatile u32  g_c2_run[2];
static volatile int  g_c2_rc[2];
static volatile u32  g_c2_evals[2];   // cond reads: a woken sleeper re-reads it
static volatile bool g_c2_release[2];
static volatile bool g_c2_exited[2];
static struct Rendez g_c2_rendez[2];

static int c2_cond(void *arg) {
    u32 i = (u32)(uintptr_t)arg;
    g_c2_evals[i]++;
    return g_c2_release[i] ? 1 : 0;
}

static void c2_sleep(u32 i) {
    current_thread()->note_interruptible = true;
    g_c2_run[i]++;
    g_c2_rc[i] = sleep_noteintr(&g_c2_rendez[i], c2_cond, (void *)(uintptr_t)i);
    g_c2_run[i]++;
    test_kthread_park_terminal(&g_c2_exited[i]);
}
static void c2_entry0(void) { c2_sleep(0); }
static void c2_entry1(void) { c2_sleep(1); }

static bool c2_asleep(struct Thread *const t[2], u32 i) {
    return g_c2_run[i] == 1u && t[i]->state == THREAD_SLEEPING;
}
static u32 c2_unwound(void) { return (g_c2_run[0] >= 2u) + (g_c2_run[1] >= 2u); }

void test_rendez_caught_note_one_unwind(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *par = cw_linux_proc();
    TEST_ASSERT(par != NULL, "parent alloc");
    par->sid  = 0x5F34u;   // a fabricated session: the cont fan reaches only par
    par->pgid = (u32)par->pid;
    struct Proc *kid = proc_alloc();
    if (!kid) { par->state = PROC_STATE_ZOMBIE; proc_free(par); }
    TEST_ASSERT(kid != NULL, "child alloc");
    proc_test_link(par);
    kid->parent = par;

    bool set = viv_sigtab_set(par->sigtab, VIV_SIGNOTE_CHILD_EXIT, &g_cw_hand) &&
               viv_sigtab_set(par->sigtab, VIV_SIGNOTE_TTY_CONT, &g_cw_hand);
    struct Thread *t[2] = { NULL, NULL };
    for (u32 i = 0; i < 2u; i++) {
        g_c2_run[i] = 0; g_c2_rc[i] = 0x7fffffff; g_c2_evals[i] = 0;
        g_c2_release[i] = false; g_c2_exited[i] = false;
        rendez_init(&g_c2_rendez[i]);
    }
    if (set) {
        t[0] = thread_create(par, c2_entry0);
        t[1] = t[0] ? thread_create(par, c2_entry1) : NULL;
        if (t[0] && !t[1]) { thread_free(t[0]); t[0] = NULL; }   // never readied
    }

    bool parked = false;
    if (t[0] && t[1]) {
        ready(t[0]);
        ready(t[1]);
        u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
        while (!(parked = c2_asleep(t, 0) && c2_asleep(t, 1)) && timer_now_ns() < dl)
            sched();
    }

    // ONE caught child_exit. Settle until one sleeper has returned and the other
    // has been woken (its cond re-read) and is asleep again -- or both returned.
    u32  unwound  = 0;
    bool reparked = false;
    if (parked) {
        u32 ev[2] = { g_c2_evals[0], g_c2_evals[1] };
        cw_post_child_exit(kid);
        u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
        for (;;) {
            unwound  = c2_unwound();
            reparked = false;
            for (u32 i = 0; i < 2u; i++)
                if (c2_asleep(t, i) && g_c2_evals[i] != ev[i]) reparked = true;
            if ((unwound == 1u && reparked) || unwound == 2u || timer_now_ns() >= dl)
                break;
            sched();
        }
    }
    u32 w        = (g_c2_run[0] >= 2u) ? 0u : 1u;   // the sleeper the note unwound
    int rc_first = g_c2_rc[w];

    // The control: a caught tty:cont -- another family -- for the re-parked one.
    u32 unwound_at_cont = c2_unwound();
    if (parked && unwound == 1u && reparked) {
        (void)proc_job_cont_pgrp(par->pgid);
        u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
        while (g_c2_run[1u - w] < 2u && timer_now_ns() < dl) sched();
    }
    int rc_second = g_c2_rc[1u - w];

    // Release whatever is still asleep, and never free a thread that stayed so.
    bool made   = t[0] && t[1];
    bool joined = made;
    for (u32 i = 0; made && i < 2u; i++) {
        if (g_c2_run[i] < 2u) { g_c2_release[i] = true; (void)wakeup(&g_c2_rendez[i]); }
        u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
        while (g_c2_run[i] < 2u && timer_now_ns() < dl) sched();
        if (g_c2_run[i] < 2u) { joined = false; continue; }
        test_kthread_join_free(t[i], &g_c2_exited[i]);
    }
    kid->parent = NULL;
    kid->state  = PROC_STATE_ZOMBIE;
    proc_free(kid);
    TEST_ASSERT(!made || joined, "both sleepers returned and were joined");
    proc_test_unlink(par);
    par->state = PROC_STATE_ZOMBIE;
    proc_free(par);

    TEST_ASSERT(set, "the SIGCHLD and SIGCONT handler rows were written");
    TEST_ASSERT(made, "both sleepers created");
    TEST_ASSERT(parked, "both sleepers parked in an interruptible wait");
    TEST_EXPECT_EQ(unwound, 1u,
        "ONE caught child_exit unwinds exactly ONE of two interruptible sleepers "
        "(ARCH 8.8.3: one note, one unwind)");
    TEST_EXPECT_EQ(rc_first, SLEEP_NOTEINTR, "the claimant unwinds for the handler");
    TEST_ASSERT(reparked, "the wake reached the peer, which re-read its cond and re-parked");
    TEST_EXPECT_EQ(unwound_at_cont, 1u, "the peer was still asleep when the next note came");
    TEST_EXPECT_EQ(rc_second, SLEEP_NOTEINTR,
        "control: a caught note of ANOTHER family unwinds the re-parked peer");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after cleanup");
}

// ---------------------------------------------------------------------------
// The claim ends at the claimant's EL0-return tail (VIV-EINTR round-2 F1).
// ---------------------------------------------------------------------------

void notes_deliver_at_el0_return(struct exception_context *ctx);

// The EL0-return tail of the call that just returned, run on this kernel
// thread. The sp passes the tail's own checks but sits below a Linux signal
// frame, so a handler is refused before any store and its note stays queued.
static void c3_tail(void) {
    struct exception_context ctx;
    for (size_t k = 0; k < sizeof(ctx); k++) ((u8 *)&ctx)[k] = 0;
    ctx.sp = NOTE_NAME_MAX;
    irq_state_t s = spin_lock_irqsave(NULL);   // the tail runs masked (#713)
    notes_deliver_at_el0_return(&ctx);
    spin_unlock_irqrestore(NULL, s);
}

// rendez.caught_note_tail_discards_and_releases -- the reviewer's chain. A child
// exits under a SIG_DFL SIGCHLD and a resize posts tty:winch under a SIG_DFL
// SIGWINCH (both queued, neither armed), then a caught interrupt lands behind
// them. The sleeper claims the interrupt and unwinds. Its tail must loop past
// BOTH ignored notes to the caught one, as Linux's get_signal does; that note
// cannot be delivered here, and the tail's end must release the claim -- or the
// retried wait refuses the note it claimed and parks, and Ctrl-C is dead.
static volatile u32  g_c3_run;
static volatile int  g_c3_rc1, g_c3_rc2;
static volatile u32  g_c3_left;      // queued notes after the tail
static volatile bool g_c3_release;
static volatile bool g_c3_exited;
static struct Rendez g_c3_rendez;

static int c3_cond(void *arg) { (void)arg; return g_c3_release ? 1 : 0; }

static void c3_entry(void) {
    struct Thread *t = current_thread();
    t->note_interruptible = true;                    // a recv: on signal(7)'s list
    g_c3_run++;                                      // -> 1: the first wait
    g_c3_rc1 = sleep_noteintr(&g_c3_rendez, c3_cond, NULL);
    t->note_interruptible = false;                   // syscall_dispatch's exit
    c3_tail();
    spin_lock(&t->proc->notes->lock);
    g_c3_left = t->proc->notes->count;
    spin_unlock(&t->proc->notes->lock);
    t->note_interruptible = true;
    g_c3_run++;                                      // -> 2: the retry
    g_c3_rc2 = sleep_noteintr(&g_c3_rendez, c3_cond, NULL);
    t->note_interruptible = false;
    g_c3_run++;                                      // -> 3
    test_kthread_park_terminal(&g_c3_exited);
}

void test_rendez_caught_note_tail_discards_and_releases(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *par = cw_linux_proc();
    TEST_ASSERT(par != NULL, "parent alloc");
    bool set = viv_sigtab_set(par->sigtab, VIV_SIGNOTE_INTERRUPT, &g_cw_hand);
    g_c3_run = 0; g_c3_rc1 = g_c3_rc2 = 0x7fffffff; g_c3_left = 0xffffffffu;
    g_c3_release = false; g_c3_exited = false;
    rendez_init(&g_c3_rendez);
    struct Thread *t = set ? thread_create(par, c3_entry) : NULL;

    bool parked = false;
    if (t) {
        ready(t);
        u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
        while (!(parked = g_c3_run == 1u && t->state == THREAD_SLEEPING) &&
               timer_now_ns() < dl)
            sched();
    }
    bool posted = false;
    if (parked) {
        irq_state_t s = proc_table_lock_acquire();
        int p1 = notes_post(par, NOTE_NAME_CHILD_EXIT, 0u, NULL, true);
        int p2 = notes_post(par, NOTE_NAME_TTY_WINCH, 0u, NULL, true);
        int p3 = notes_post(par, NOTE_NAME_INTERRUPT, 0u, NULL, true);
        proc_caught_note_wake(par);
        proc_table_lock_release(s);
        posted = p1 == 0 && p2 == 0 && p3 == 0;
        u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
        while (g_c3_run < 3u && timer_now_ns() < dl) sched();
    }
    int rc1 = g_c3_rc1, rc2 = g_c3_rc2;
    u32 left = g_c3_left;

    // Release a sleeper still parked, and never free a thread that stayed so.
    bool joined = (t == NULL);
    if (t) {
        if (g_c3_run < 3u) { g_c3_release = true; (void)wakeup(&g_c3_rendez); }
        u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
        while (g_c3_run < 3u && timer_now_ns() < dl) sched();
        if (g_c3_run >= 3u) { test_kthread_join_free(t, &g_c3_exited); joined = true; }
    }
    TEST_ASSERT(joined, "the sleeper returned and was joined");
    par->state = PROC_STATE_ZOMBIE;
    proc_free(par);

    TEST_ASSERT(set, "the SIGINT handler row was written");
    TEST_ASSERT(parked, "the sleeper parked in an interruptible wait");
    TEST_ASSERT(posted, "child_exit, tty:winch and interrupt were posted");
    TEST_EXPECT_EQ(rc1, SLEEP_NOTEINTR, "the caught interrupt unwinds the wait");
    TEST_EXPECT_EQ(left, 1u,
        "the tail looped past BOTH ignored notes to the caught one, which stays "
        "queued (no frame can be built on a kernel thread)");
    TEST_EXPECT_EQ(rc2, SLEEP_NOTEINTR,
        "the retried wait unwinds again: the tail released the claim, so the note "
        "it could not deliver is not stranded behind it");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after cleanup");
}

// ---------------------------------------------------------------------------
// The notes leg parks for a stop it applies itself (DEBUG-FS-DESIGN 4.2).
// ---------------------------------------------------------------------------

// rendez.tail_parks_for_the_stop_it_applies -- the tail stops before it
// delivers notes, so the default stop of an uncaught tty:susp, which only the
// notes leg can apply, would leave the thread running EL0 code with its stop
// set unless the leg parks for it. The tail runs on this kernel thread, masked
// as in production: it must park on its own debug_rendez and return only once
// the stop is lifted. The control is one variable away: child_exit, which
// stops nothing, returns at once and leaves the Proc unstopped.
static volatile u32  g_d_run;
static volatile bool g_d_returned;
static volatile bool g_d_exited;

static void d_entry(void) {
    struct exception_context ctx;
    for (size_t k = 0; k < sizeof(ctx); k++) ((u8 *)&ctx)[k] = 0;
    ctx.sp = NOTE_NAME_MAX;
    g_d_run++;
    irq_state_t s = spin_lock_irqsave(NULL);   // the tail runs masked (#713)
    notes_deliver_at_el0_return(&ctx);
    spin_unlock_irqrestore(NULL, s);
    g_d_returned = true;
    test_kthread_park_terminal(&g_d_exited);
}

// One leg on a fresh anchored group (a leader in the same session, another
// group), so the orphan rule cannot discard the stop. Returns whether the tail
// parked on its debug_rendez before it returned; *returned_early says it came
// back before the stop was lifted, *stopped that the Proc took the stop.
static bool d_leg(const char *note, bool *returned_early, bool *stopped, bool *joined) {
    struct Proc *leader = proc_alloc();
    struct Proc *m = leader ? proc_alloc() : NULL;
    *returned_early = false; *stopped = false; *joined = false;
    if (!leader || !m) {
        if (leader) { leader->state = PROC_STATE_ZOMBIE; proc_free(leader); }
        return false;
    }
    proc_test_link(leader);
    m->sid  = (u32)leader->pid;
    m->pgid = (u32)m->pid;
    proc_test_link_child(leader, m);

    g_d_run = 0; g_d_returned = false; g_d_exited = false;
    bool posted = notes_post(m, note, 0u, NULL, true) == 0;
    struct Thread *t = posted ? thread_create(m, d_entry) : NULL;
    bool parked = false;
    if (t) {
        ready(t);
        u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
        while (!g_d_returned && timer_now_ns() < dl) {
            if (g_d_run == 1u && t->state == THREAD_SLEEPING &&
                t->rendez_blocked_on == &t->debug_rendez) {
                parked = true;
                break;
            }
            sched();
        }
        *returned_early = g_d_returned;
        *stopped = __atomic_load_n(&m->job_stop_req, __ATOMIC_ACQUIRE) != 0;
        irq_state_t s = proc_table_lock_acquire();
        proc_job_cont_proc(m);
        proc_table_lock_release(s);
        dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
        while (!g_d_returned && timer_now_ns() < dl) sched();
        if (g_d_returned) { test_kthread_join_free(t, &g_d_exited); *joined = true; }
    }
    // A thread that never came back still lives in m, and proc_free would
    // extinct the suite here, before the asserts could name the failure.
    if (t && !*joined) return parked;
    proc_test_unlink(m);
    m->state = PROC_STATE_ZOMBIE;
    proc_free(m);
    proc_test_unlink(leader);
    leader->state = PROC_STATE_ZOMBIE;
    proc_free(leader);
    return parked;
}

void test_rendez_tail_parks_for_the_stop_it_applies(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    bool early_c, stopped_c, joined_c;
    bool parked_c = d_leg(NOTE_NAME_CHILD_EXIT, &early_c, &stopped_c, &joined_c);
    // The legs share the g_d_* flags: a control thread still alive would write
    // them under the second leg.
    TEST_ASSERT(joined_c, "control: the tail returned and its thread was joined");
    bool early_s, stopped_s, joined_s;
    bool parked_s = d_leg(NOTE_NAME_TTY_SUSP, &early_s, &stopped_s, &joined_s);

    TEST_ASSERT(!parked_c && early_c, "control: child_exit stops nothing, so the tail returns at once");
    TEST_ASSERT(!stopped_c, "control: and the Proc took no stop");
    TEST_ASSERT(joined_s, "the stopped tail returned once the stop was lifted, and was joined");
    TEST_ASSERT(stopped_s, "the uncaught tty:susp applied its default stop in the notes leg");
    TEST_ASSERT(parked_s,
        "the notes leg parked for the stop it applied, on the thread's own debug_rendez");
    TEST_ASSERT(!early_s, "the tail did not return to EL0 while its stop was set");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after cleanup");
}

// rendez.caught_note_release_wakes_peer -- the release's wake. Two sleepers, one
// caught interrupt: one claims it and unwinds, the other re-reads its cond and
// re-parks. Only then does the claimant's tail run; it cannot deliver the note,
// so its release finds the note still queued and must wake the parked peer,
// which unwinds for it. Holding the tail until the peer has re-parked is what
// makes this discriminate: a peer that re-read its cond only after the release
// would unwind with no wake at all.
static volatile u32  g_c4_run[2];
static volatile int  g_c4_rc[2];
static volatile u32  g_c4_evals[2];
static volatile bool g_c4_go;
static volatile bool g_c4_release[2];
static volatile bool g_c4_exited[2];
static struct Rendez g_c4_rendez[2];

static int c4_cond(void *arg) {
    u32 i = (u32)(uintptr_t)arg;
    g_c4_evals[i]++;
    return g_c4_release[i] ? 1 : 0;
}

static void c4_sleep(u32 i) {
    struct Thread *t = current_thread();
    t->note_interruptible = true;
    g_c4_run[i]++;                                   // -> 1: asleep
    g_c4_rc[i] = sleep_noteintr(&g_c4_rendez[i], c4_cond, (void *)(uintptr_t)i);
    t->note_interruptible = false;
    g_c4_run[i]++;                                   // -> 2: the wait returned
    while (!g_c4_go) sched();                        // the test holds the tail
    c3_tail();
    g_c4_run[i]++;                                   // -> 3: the tail ran
    test_kthread_park_terminal(&g_c4_exited[i]);
}
static void c4_entry0(void) { c4_sleep(0); }
static void c4_entry1(void) { c4_sleep(1); }

static bool c4_asleep(struct Thread *const t[2], u32 i) {
    return g_c4_run[i] == 1u && t[i]->state == THREAD_SLEEPING;
}

void test_rendez_caught_note_release_wakes_peer(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *par = cw_linux_proc();
    TEST_ASSERT(par != NULL, "parent alloc");
    bool set = viv_sigtab_set(par->sigtab, VIV_SIGNOTE_INTERRUPT, &g_cw_hand);
    struct Thread *t[2] = { NULL, NULL };
    g_c4_go = false;
    for (u32 i = 0; i < 2u; i++) {
        g_c4_run[i] = 0; g_c4_rc[i] = 0x7fffffff; g_c4_evals[i] = 0;
        g_c4_release[i] = false; g_c4_exited[i] = false;
        rendez_init(&g_c4_rendez[i]);
    }
    if (set) {
        t[0] = thread_create(par, c4_entry0);
        t[1] = t[0] ? thread_create(par, c4_entry1) : NULL;
        if (t[0] && !t[1]) { thread_free(t[0]); t[0] = NULL; }   // never readied
    }
    bool made = t[0] && t[1];

    bool parked = false;
    if (made) {
        ready(t[0]);
        ready(t[1]);
        u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
        while (!(parked = c4_asleep(t, 0) && c4_asleep(t, 1)) && timer_now_ns() < dl)
            sched();
    }

    // One caught interrupt. Settle until one sleeper's wait returned and the
    // other has re-read its cond and is asleep again.
    u32  unwound  = 0;
    bool reparked = false;
    u32  w        = 0;
    if (parked) {
        u32 ev[2] = { g_c4_evals[0], g_c4_evals[1] };
        irq_state_t s = proc_table_lock_acquire();
        (void)notes_post(par, NOTE_NAME_INTERRUPT, 0u, NULL, true);
        proc_caught_note_wake(par);
        proc_table_lock_release(s);
        u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
        for (;;) {
            unwound  = (g_c4_run[0] >= 2u) + (g_c4_run[1] >= 2u);
            w        = (g_c4_run[0] >= 2u) ? 0u : 1u;
            reparked = unwound == 1u && c4_asleep(t, 1u - w) && g_c4_evals[1u - w] != ev[1u - w];
            if (reparked || unwound == 2u || timer_now_ns() >= dl) break;
            sched();
        }
    }
    // Now the claimant's tail: it cannot deliver, so its release must wake the peer.
    g_c4_go = true;
    if (parked) {
        u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
        while ((g_c4_run[0] < 3u || g_c4_run[1] < 3u) && timer_now_ns() < dl) sched();
    }
    int rc_claimant = g_c4_rc[w], rc_peer = g_c4_rc[1u - w];

    bool joined = made;
    for (u32 i = 0; made && i < 2u; i++) {
        if (g_c4_run[i] < 2u) { g_c4_release[i] = true; (void)wakeup(&g_c4_rendez[i]); }
        u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
        while (g_c4_run[i] < 3u && timer_now_ns() < dl) sched();
        if (g_c4_run[i] < 3u) { joined = false; continue; }
        test_kthread_join_free(t[i], &g_c4_exited[i]);
    }
    TEST_ASSERT(!made || joined, "both sleepers returned and were joined");
    par->state = PROC_STATE_ZOMBIE;
    proc_free(par);

    TEST_ASSERT(set, "the SIGINT handler row was written");
    TEST_ASSERT(made, "both sleepers created");
    TEST_ASSERT(parked, "both sleepers parked in an interruptible wait");
    TEST_EXPECT_EQ(unwound, 1u, "one caught note unwinds one sleeper before any tail runs");
    TEST_ASSERT(reparked, "the peer re-read its cond and re-parked while the claim was held");
    TEST_EXPECT_EQ(rc_claimant, SLEEP_NOTEINTR, "the claimant unwound for the note");
    TEST_EXPECT_EQ(rc_peer, SLEEP_NOTEINTR,
        "the claimant's tail could not deliver the note, and its release woke the "
        "parked peer, which unwound for it");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after cleanup");
}

// rendez.caught_note_ends_wait4 -- signal(7)'s list (ARCH 8.8.3). wait4 is a
// listed call. A caught note ends a Linux parent's wait for a live child with
// WAIT_PID_NOTEINTR, nothing reaped; a native parent's wait rides it out and
// ends when its last child is gone (-1, the ECHILD shape).
static struct Proc *g_cwt_par;
static struct Proc *g_cwt_kid;
static bool         g_cwt_unlinked;

static long cwt_wait(void *arg) {
    (void)arg;
    int status = 0;
    return (long)wait_pid_for(-1, 0, &status);
}

static void cwt_release(void *arg) {
    (void)arg;
    proc_test_unlink(g_cwt_kid);           // no child left: the re-scan answers -1
    g_cwt_unlinked = true;
    poll_waiter_list_wake(&g_cwt_par->child_waiters);
}

static bool cwt_kid_linked(void) {
    bool linked = false;
    irq_state_t s = proc_table_lock_acquire();
    for (struct Proc *c = g_cwt_par->children; c; c = c->sibling)
        if (c == g_cwt_kid) linked = true;
    proc_table_lock_release(s);
    return linked;
}

static struct test_caught_leg cwt_leg(bool linux_pheno, bool *kid_kept) {
    g_cwt_par      = test_caught_proc(linux_pheno);
    g_cwt_kid      = g_cwt_par ? proc_alloc() : NULL;
    g_cwt_unlinked = false;
    if (g_cwt_kid) {
        g_cwt_kid->state = PROC_STATE_ALIVE;   // alive, nothing to report: the wait blocks
        proc_test_link_child(g_cwt_par, g_cwt_kid);
    }
    struct test_caught_leg leg = test_caught_run(g_cwt_kid ? g_cwt_par : NULL, cwt_wait,
                                                 NULL, cwt_release, NULL, false);
    *kid_kept = g_cwt_kid && cwt_kid_linked();
    if (leg.stranded) return leg;
    if (g_cwt_kid) {
        if (!g_cwt_unlinked) proc_test_unlink(g_cwt_kid);
        g_cwt_kid->state = PROC_STATE_ZOMBIE;
        proc_free(g_cwt_kid);
    }
    test_caught_proc_free(g_cwt_par, &leg);
    return leg;
}

void test_rendez_caught_note_ends_wait4(void);
void test_rendez_caught_note_ends_wait4(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    bool kept = false, ctl_kept = false;
    struct test_caught_leg leg = cwt_leg(true, &kept);
    struct test_caught_leg ctl = cwt_leg(false, &ctl_kept);

    TEST_ASSERT(leg.parked && leg.posted && leg.joined,
        "the Linux parent waited on its live child, the note posted, the wait returned");
    TEST_ASSERT(leg.on_post, "a caught note ends a Linux parent's wait4 (ARCH 8.8.3)");
    TEST_EXPECT_EQ(leg.rc, (long)WAIT_PID_NOTEINTR,
        "WAIT_PID_NOTEINTR, not the no-child -1 that viv_wait4 reports as ECHILD");
    TEST_ASSERT(kept, "nothing reaped: the child is still the parent's");
    TEST_ASSERT(ctl.parked && ctl.posted && ctl.joined,
        "control: the native parent waited, the note posted, the wait returned");
    TEST_ASSERT(ctl.rode_out, "control: the note woke the native parent and it waited again");
    TEST_EXPECT_EQ(ctl.rc, -1L, "control: the native wait ends when its last child is gone");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after cleanup");
}
