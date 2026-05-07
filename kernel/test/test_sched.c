// scheduler tests (P2-Ba dispatch + P2-Bc preemption).
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
//
// scheduler.preemption_smoke (P2-Bc)
//   Two CPU-bound threads (busy loops, no cooperative sched()) each
//   ready'd; boot waits N timer ticks. Without preemption the threads
//   would be deadlocked (no thread voluntarily yields). With
//   preemption: the timer IRQ → sched_tick → need_resched → preempt_check_irq
//   → sched() chain rotates through ta / tb / boot, each consuming
//   their slice. After the wait window, boot signals exit, both
//   threads exit cooperatively, boot asserts both counters > 0 and
//   roughly equal (latency bound: each thread is reachable within
//   slice × N ticks of becoming runnable).

#include "test.h"

#include "../../arch/arm64/timer.h"

#include <thylacine/dtb.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/smp.h>
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

// ---------------------------------------------------------------------------
// scheduler.preemption_smoke (P2-Bc)
// ---------------------------------------------------------------------------
//
// Two CPU-bound threads + boot kthread share the CPU under timer-IRQ-
// driven preemption. Each tick decrements current's slice; on slice
// expiry, sched() switches to the next runnable thread. The threads
// busy-loop incrementing per-thread counters; boot waits a fixed
// window then signals exit.
//
// What this proves:
//   - timer_irq_handler → sched_tick is wired (slices decrement).
//   - preempt_check_irq fires from the IRQ-return path (vectors.S).
//   - sched() picks the right next thread under preemption (ta and tb
//     each get scheduled).
//   - The latency bound holds informally: with slice = 6 ticks and
//     N = 3 (boot, ta, tb), each thread reaches running within
//     ~18 ticks of becoming runnable.
//
// The wait window is generous (40 ticks ≈ 40 ms at 1000 Hz) — enough
// for each thread to get multiple slices, so both counters are
// observably non-zero. Fairness is checked loosely (10× tolerance);
// the test isn't a benchmark.

static volatile bool g_preempt_test_running;
static volatile u64  g_preempt_test_counter[2];

static void preempt_test_thread_a(void) {
    while (g_preempt_test_running) {
        g_preempt_test_counter[0]++;
    }
    // Exit signaled. Yield back to the scheduler; boot will reap.
    sched();
    // Unreachable — boot doesn't switch back.
}

static void preempt_test_thread_b(void) {
    while (g_preempt_test_running) {
        g_preempt_test_counter[1]++;
    }
    sched();
}

void test_sched_preemption_smoke(void) {
    // Pre-conditions.
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree must be empty at test entry");

    g_preempt_test_running    = true;
    g_preempt_test_counter[0] = 0;
    g_preempt_test_counter[1] = 0;

    struct Thread *ta = thread_create(kproc(), preempt_test_thread_a);
    struct Thread *tb = thread_create(kproc(), preempt_test_thread_b);
    TEST_ASSERT(ta != NULL, "thread_create(ta) failed");
    TEST_ASSERT(tb != NULL, "thread_create(tb) failed");

    ready(ta);
    ready(tb);
    TEST_EXPECT_EQ(sched_runnable_count(), 2u,
        "two threads ready'd into the run tree");

    // Wait window. boot is current; sched_tick decrements boot's slice;
    // when expired, preempt_check_irq → sched() picks ta. ta busy-
    // loops; its slice decrements; preempt; tb runs. Eventually rotate
    // back to boot which is in WFI inside timer_busy_wait_ticks.
    //
    // 40 ticks: with 6-tick slice and 3 active threads, each thread
    // gets ~3 slices (≈ 18 increments of its counter — but counter
    // increments at busy-loop rate which is millions per ms, so the
    // counter value is huge, just non-zero is the bar).
    timer_busy_wait_ticks(40);

    // Signal exit. Threads check on their next iteration and break.
    g_preempt_test_running = false;

    // Allow the threads to finish their loops + reach sched(). Each
    // exits at most one busy-loop iteration after observing the flag,
    // then yields. 5 ticks is generous (each thread needs ≤ 1 slice
    // to drain).
    timer_busy_wait_ticks(5);

    // Both threads must have run.
    TEST_ASSERT(g_preempt_test_counter[0] > 0,
        "ta did not run (preempt_check_irq not firing?)");
    TEST_ASSERT(g_preempt_test_counter[1] > 0,
        "tb did not run (preempt_check_irq not firing?)");

    // Loose fairness: 10× tolerance. Each thread should get roughly
    // a third of the CPU time over the wait window. With slice
    // quantization, ratios within 10× indicate the rotation is
    // working — without preemption one thread would have ALL the CPU
    // and the other would have ZERO (deadlock from busy loop).
    u64 a = g_preempt_test_counter[0];
    u64 b = g_preempt_test_counter[1];
    TEST_ASSERT(a < (u64)10 * b && b < (u64)10 * a,
        "preemption is severely unfair (>10× imbalance)");

    // Cleanup. Both threads have called sched() after exiting their
    // loops; both should be RUNNABLE in the tree (suspended inside
    // their own sched()). thread_free removes them.
    thread_free(ta);
    thread_free(tb);
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree empty after thread_free");
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


// =============================================================================
// P3-G: WFI-aware work-stealing tests.
// =============================================================================

// scheduler.idle_in_wfi_observability — verify the per-CPU idle_in_wfi
// flag is observable from boot. Out-of-range queries return false (not
// extinction); boot CPU stays FALSE forever (no wfi loop hook); after
// secondaries settle into per_cpu_main's wfi loop, at least one reports
// idle_in_wfi=true.
//
// Closes part of R5-H F78 verification — the "is this peer in WFI?"
// signal that ready/wakeup consult to choose a wake target.
void test_sched_idle_in_wfi_observability(void) {
    // Out-of-range — no extinction, returns false defensively.
    TEST_EXPECT_EQ(sched_idle_in_wfi(DTB_MAX_CPUS), false,
        "out-of-range cpu_idx returns false");
    TEST_EXPECT_EQ(sched_idle_in_wfi(DTB_MAX_CPUS + 100), false,
        "way-out-of-range cpu_idx returns false");

    // Boot CPU never enters per_cpu_main's wfi loop — stays FALSE.
    TEST_EXPECT_EQ(sched_idle_in_wfi(0), false,
        "boot CPU is never in wfi (post-init runs in _hang's asm loop)");

    // If running multi-CPU, secondaries should be in WFI by now.
    // Allow brief settle window for any in-flight ipi handling.
    if (smp_cpu_count() < 2) {
        return;     // UP — nothing to assert.
    }
    timer_busy_wait_ticks(2);

    bool any_in_wfi = false;
    for (unsigned i = 1; i < smp_cpu_count(); i++) {
        if (sched_idle_in_wfi(i)) {
            any_in_wfi = true;
            break;
        }
    }
    TEST_ASSERT(any_in_wfi,
        "at least one secondary should be in WFI after settle window");
}

static volatile u32 g_notify_test_ran;

static void notify_test_thread(void) {
    g_notify_test_ran++;
    sched();
    // Unreachable — boot doesn't switch back here.
}

// scheduler.notify_idle_peer_smoke — verify that with notify enabled,
// ready() of a thread on boot wakes an idle secondary via IPI_RESCHED.
// The IPI hits the secondary, the secondary's wfi exits, the secondary's
// post-wfi sched picks try_steal, the secondary steals our thread.
//
// Observable: g_ipi_resched_count[secondary] increments by ≥ 1 across
// the ready call.
//
// Closes the load-bearing piece of R5-H F78. Without sched_set_notify_
// enabled(true), ready() doesn't IPI; with it, ready() does.
void test_sched_notify_idle_peer_smoke(void) {
    if (smp_cpu_count() < 2) {
        return;     // UP — no peer to notify.
    }

    // Settle window: ensure secondaries are in WFI before we start.
    timer_busy_wait_ticks(2);

    // Snapshot per-secondary IPI counts before.
    u64 ipi_before[DTB_MAX_CPUS];
    for (unsigned i = 0; i < DTB_MAX_CPUS; i++) {
        ipi_before[i] = g_ipi_resched_count[i];
    }

    // Toggle notify on for this test only — boot enables it permanently
    // before /init, but the test framework runs with it OFF.
    sched_set_notify_enabled(true);

    // Create + ready a thread. ready() will fire notify_idle_peer
    // and hit one secondary with IPI_RESCHED.
    g_notify_test_ran = 0;
    struct Thread *t = thread_create(kproc(), notify_test_thread);
    TEST_ASSERT(t != NULL, "thread_create failed");
    ready(t);

    // Allow the IPI to deliver + secondary to process. 5 ticks ~5 ms.
    timer_busy_wait_ticks(5);

    // Restore notify state for subsequent tests (UP-like assumption).
    sched_set_notify_enabled(false);

    // At least one secondary's IPI count should have increased.
    bool any_secondary_received = false;
    for (unsigned i = 1; i < DTB_MAX_CPUS; i++) {
        if (g_ipi_resched_count[i] > ipi_before[i]) {
            any_secondary_received = true;
            break;
        }
    }
    TEST_ASSERT(any_secondary_received,
        "at least one secondary received IPI_RESCHED via notify_idle_peer");

    // Cleanup. Either secondary stole + ran the thread (g_notify_test_ran
    // > 0), or the thread is still RUNNABLE in some tree (boot's or
    // a secondary's). thread_free is idempotent across both.
    thread_free(t);
}

// scheduler.notify_disabled_no_ipi — when notify is disabled (default
// during in-kernel tests), ready() does NOT send IPI to peers. Tests
// that assume UP-like consumer-runs-on-readier semantics keep working.
void test_sched_notify_disabled_no_ipi(void) {
    if (smp_cpu_count() < 2) {
        return;
    }

    // Ensure disabled (the default state during tests).
    sched_set_notify_enabled(false);

    timer_busy_wait_ticks(2);

    u64 ipi_before[DTB_MAX_CPUS];
    for (unsigned i = 0; i < DTB_MAX_CPUS; i++) {
        ipi_before[i] = g_ipi_resched_count[i];
    }

    // ready a thread; with notify disabled, no IPI should fire.
    g_notify_test_ran = 0;
    struct Thread *t = thread_create(kproc(), notify_test_thread);
    TEST_ASSERT(t != NULL, "thread_create failed");
    ready(t);

    // Settle. No IPI means secondaries stay in WFI.
    timer_busy_wait_ticks(3);

    // Aggregate IPI counts across all secondaries; none should have
    // changed (timer-driven IRQs don't trigger IPI; only explicit
    // gic_send_ipi does, which only happens via notify_idle_peer
    // when enabled, or in tests that explicitly send).
    u64 total_delta = 0;
    for (unsigned i = 1; i < DTB_MAX_CPUS; i++) {
        if (g_ipi_resched_count[i] > ipi_before[i]) {
            total_delta += g_ipi_resched_count[i] - ipi_before[i];
        }
    }
    TEST_EXPECT_EQ(total_delta, 0u,
        "no secondary IPIs fired with notify disabled");

    // Cleanup. Thread is still in boot's tree (no steal possible
    // without IPI to wake secondaries). thread_free removes.
    thread_free(t);
}
