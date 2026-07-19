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
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../../arch/arm64/timer.h"   // LS-5c: far tsleep deadline (timer_now_ns)

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
    sched();

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

    TEST_EXPECT_EQ(consumer->state, THREAD_RUNNABLE,
        "sleeper readied by the terminate wake");
    sched();

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
    sched();

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
    sched();

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
    sched();

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
    sched();

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
    sched();

    TEST_EXPECT_EQ(g_intr_run_cnt, 1u, "consumer ran once before tsleeping");
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer parked on the far-deadline tsleep");

    TEST_EXPECT_EQ(notes_post(g_intr_proc, "interrupt", 0u, NULL, true), 0,
        "interrupt post accepted");
    irq_state_t s = proc_table_lock_acquire();
    proc_interrupt_terminate_wake(g_intr_proc);
    proc_table_lock_release(s);
    sched();

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
