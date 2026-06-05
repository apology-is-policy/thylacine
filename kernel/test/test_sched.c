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

// scheduler.runnable_count_excludes_idle (#857 regression)
//   sched_runnable_count() reports runnable WORK and MUST exclude
//   SCHED_BAND_IDLE. The per-CPU idle threads live in run_tree[BAND_IDLE] on
//   secondaries ("participate in the run tree like any other thread"), so a
//   count that included them reported a phantom backlog and made the
//   sched_runnable_count()==0 quiescence assertions race a secondary idle
//   thread that, under host load, sat in-tree at the check instant -- the smp8
//   "cons.* flake" (which was never console_mgr, never a kernel fault). This
//   pins the fix deterministically at smp1: a band-IDLE thread in the tree is
//   invisible to the work count yet visible to the per-band query, and a
//   band-WORK thread is still counted (so a real strand is never masked).
void test_sched_runnable_count_excludes_idle(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "work count 0 at entry");

    // SMP redesign (ARCH 8.4.2): cpu0's bootcpu_idle is a permanent in-tree
    // BAND_IDLE thread, so the IDLE band is no longer empty at baseline. This
    // test runs on cpu0/kthread with secondaries parked (no stealing during
    // tests), so the IDLE-band count is stable; assert DELTAS against the
    // captured baseline rather than absolute counts.
    unsigned base_idle = sched_runnable_count_band(SCHED_BAND_IDLE);

    struct Thread *idle_band = thread_create(kproc(), sched_test_thread_a);
    TEST_ASSERT(idle_band != NULL, "thread_create failed");
    idle_band->band = SCHED_BAND_IDLE;          // mimic a per-CPU idle thread
    ready(idle_band);

    // Pre-#857 this returned 1 (idle miscounted as work) -> the smp8 flake.
    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "a band-IDLE thread is NOT counted as runnable work");
    TEST_EXPECT_EQ(sched_runnable_count_band(SCHED_BAND_IDLE), base_idle + 1u,
        "the band-IDLE thread IS present in the IDLE band (baseline + 1)");

    // A real WORK thread alongside it IS counted (no masking of real strands).
    struct Thread *work = thread_create(kproc(), sched_test_thread_b);
    TEST_ASSERT(work != NULL, "thread_create(work) failed");   // default band NORMAL
    ready(work);
    TEST_EXPECT_EQ(sched_runnable_count(), 1u,
        "a band-NORMAL work thread IS counted (idle excluded, work included)");

    thread_free(work);
    thread_free(idle_band);
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "work count 0 after free");
    TEST_EXPECT_EQ(sched_runnable_count_band(SCHED_BAND_IDLE), base_idle,
        "IDLE band back to baseline after free");
}


// =============================================================================
// P3-G: WFI-aware work-stealing tests.
// =============================================================================

// scheduler.idle_in_wfi_observability — verify the per-CPU idle_in_wfi
// flag is observable from boot. Out-of-range queries return false (not
// extinction); boot CPU stays FALSE until the first deadlock-path
// fallback fires (R12-sched); after secondaries settle into per_cpu_main's
// wfi loop, at least one reports idle_in_wfi=true.
//
// Closes part of R5-H F78 verification — the "is this peer in WFI?"
// signal that ready/wakeup consult to choose a wake target.
void test_sched_idle_in_wfi_observability(void) {
    // Out-of-range — no extinction, returns false defensively.
    TEST_EXPECT_EQ(sched_idle_in_wfi(DTB_MAX_CPUS), false,
        "out-of-range cpu_idx returns false");
    TEST_EXPECT_EQ(sched_idle_in_wfi(DTB_MAX_CPUS + 100), false,
        "way-out-of-range cpu_idx returns false");

    // SMP redesign (ARCH 8.4.2/8.4.5): idle_in_wfi(0) means "cpu0's current is
    // its idle (bootcpu_idle)." This test runs ON kthread, so cpu0's current is
    // kthread -- the last switch into kthread set the flag = (kthread==idle) =
    // FALSE. cpu0's idle runs only when kthread (or any cpu0 thread) blocks with
    // no other runnable work (e.g. virtio_blk_probe), and switching back to
    // kthread re-clears the flag. So it is FALSE whenever a non-idle thread runs
    // here.
    TEST_EXPECT_EQ(sched_idle_in_wfi(0), false,
        "boot CPU idle_in_wfi=FALSE while kthread (a non-idle thread) is running");

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

// =============================================================================
// HMP foundation (#864, ARCH §8.4.4): capacity + capacity-aware placement.
//
// The placement LOGIC is verified here against a SYNTHETIC asymmetric DTB --
// declared capacity-dmips-mhz asymmetry expressed directly as the normalized
// capacity arrays the DTB parse would produce. Deterministic; no real perf
// asymmetry needed (the "verification boundary", ARCH §8.4.4). The EMPIRICAL
// EAS tuning is deferred to real heterogeneous hardware. The SAFETY of any
// placement (under arbitrary target CPU) is the composition result proved by
// specs/sched_alpha.tla; these tests cover the heuristic the safety proof is
// agnostic to.
// =============================================================================

// scheduler.capacity_normalize_synthetic_dtb
//   sched_capacity_normalize maps raw capacity-dmips-mhz (0 == not declared)
//   to normalized [0, SCHED_CAPACITY_SCALE] capacities + the hetero verdict.
void test_sched_capacity_normalize_synthetic_dtb(void) {
    u32 out[DTB_MAX_CPUS];

    // All cores declare the same dmips -> homogeneous, all full scale.
    {
        u32 raw[4] = { 1024, 1024, 1024, 1024 };
        bool h = sched_capacity_normalize(raw, 4, out);
        TEST_EXPECT_EQ(h, false, "equal dmips -> not heterogeneous");
        for (unsigned i = 0; i < 4; i++)
            TEST_EXPECT_EQ(out[i], SCHED_CAPACITY_SCALE, "equal -> full scale");
    }

    // No core declares capacity-dmips-mhz (QEMU virt) -> homogeneous default.
    {
        u32 raw[4] = { 0, 0, 0, 0 };
        bool h = sched_capacity_normalize(raw, 4, out);
        TEST_EXPECT_EQ(h, false, "all-absent -> not heterogeneous");
        for (unsigned i = 0; i < 4; i++)
            TEST_EXPECT_EQ(out[i], SCHED_CAPACITY_SCALE, "absent -> full scale");
    }

    // big.LITTLE: two big cores (1024) + two little (512) -> normalized to
    // 1024 / 512 (max maps to SCALE); heterogeneous.
    {
        u32 raw[4] = { 1024, 1024, 512, 512 };
        bool h = sched_capacity_normalize(raw, 4, out);
        TEST_EXPECT_EQ(h, true, "big.LITTLE -> heterogeneous");
        TEST_EXPECT_EQ(out[0], 1024u, "big core -> 1024");
        TEST_EXPECT_EQ(out[1], 1024u, "big core -> 1024");
        TEST_EXPECT_EQ(out[2], 512u,  "little core -> 512");
        TEST_EXPECT_EQ(out[3], 512u,  "little core -> 512");
    }

    // Arbitrary magnitudes normalize against the max (2000 -> 1024, 1000 -> 512).
    {
        u32 raw[2] = { 2000, 1000 };
        bool h = sched_capacity_normalize(raw, 2, out);
        TEST_EXPECT_EQ(h, true, "2:1 dmips -> heterogeneous");
        TEST_EXPECT_EQ(out[0], 1024u, "max dmips -> SCALE");
        TEST_EXPECT_EQ(out[1], 512u,  "half dmips -> SCALE/2");
    }

    // A core that does not declare it, on a board where another does, is
    // assumed full capacity (never wrongly demoted).
    {
        u32 raw[2] = { 2000, 0 };
        bool h = sched_capacity_normalize(raw, 2, out);
        TEST_EXPECT_EQ(out[0], 1024u, "declared max -> SCALE");
        TEST_EXPECT_EQ(out[1], SCHED_CAPACITY_SCALE,
            "undeclared on a mixed board -> assumed full capacity");
        TEST_EXPECT_EQ(h, false, "1024 vs assumed-1024 -> not distinguishable");
    }
}

// scheduler.place_by_capacity_synthetic_dtb
//   The core question: "does the policy route a heavy task to the high-capacity
//   CPU, leave a light task put, and not migrate a heavy task already on the
//   biggest core?" util values are chosen far from the misfit boundary so the
//   test is robust to the v1.0 placeholder threshold value.
void test_sched_place_by_capacity_synthetic_dtb(void) {
    // cpu0 = big (1024), cpu1 = little (512).
    u32 caps2[2] = { 1024, 512 };

    // Homogeneous verdict -> identity placement regardless of util.
    TEST_EXPECT_EQ(sched_place_by_capacity(1000u, 1, caps2, 2, false), 1u,
        "homogeneous -> keep prev (heavy task)");
    TEST_EXPECT_EQ(sched_place_by_capacity(50u, 0, caps2, 2, false), 0u,
        "homogeneous -> keep prev (light task)");

    // Heterogeneous: a heavy task on the little core is a misfit -> big core.
    TEST_EXPECT_EQ(sched_place_by_capacity(1000u, 1, caps2, 2, true), 0u,
        "heavy task on little core -> routed to big core");

    // A light task on the little core fits -> stays (no needless migration).
    TEST_EXPECT_EQ(sched_place_by_capacity(50u, 1, caps2, 2, true), 1u,
        "light task on little core -> stays put");

    // A heavy task already on the biggest core stays (nothing bigger to find).
    TEST_EXPECT_EQ(sched_place_by_capacity(1000u, 0, caps2, 2, true), 0u,
        "heavy task on big core -> stays (already the highest capacity)");

    // 4-CPU big.LITTLE: heavy task on a little core -> the first big core.
    {
        u32 caps4[4] = { 1024, 1024, 512, 512 };
        TEST_EXPECT_EQ(sched_place_by_capacity(1000u, 2, caps4, 4, true), 0u,
            "heavy task on little core -> highest-capacity (first big) core");
        TEST_EXPECT_EQ(sched_place_by_capacity(50u, 3, caps4, 4, true), 3u,
            "light task on little core -> stays put");
    }

    // Defensive: prev_cpu out of range -> return prev unchanged.
    TEST_EXPECT_EQ(sched_place_by_capacity(1000u, 5, caps2, 2, true), 5u,
        "out-of-range prev -> returned unchanged");
}

// scheduler.select_target_cpu_homogeneous_is_prev
//   On the REAL (uniform) boot topology, the placement wrapper is the identity
//   for EVERY CPU and EVERY task -- this is the v1.0 behavior-preservation
//   guarantee that ready() == ready_on(self) == the pre-#864 placement. Also
//   pins that a CPU-pinned thread never migrates.
void test_sched_select_target_cpu_homogeneous_is_prev(void) {
    // QEMU virt / RPi declare no capacity-dmips-mhz -> homogeneous.
    TEST_EXPECT_EQ(sched_topology_hetero(), false,
        "boot topology is homogeneous (no capacity-dmips-mhz in the DTB)");
    TEST_EXPECT_EQ(sched_cpu_capacity(0), SCHED_CAPACITY_SCALE,
        "cpu0 capacity is the full scale on a uniform topology");

    struct Thread *t = thread_create(kproc(), sched_test_thread_a);
    TEST_ASSERT(t != NULL, "thread_create failed");
    t->util = 1000;                      // a "heavy" task...

    // ...still stays on its prev CPU on every CPU, because the topology is
    // homogeneous (the policy short-circuits to prev).
    unsigned n = smp_cpu_count();
    if (n > DTB_MAX_CPUS) n = DTB_MAX_CPUS;
    for (unsigned c = 0; c < n; c++) {
        TEST_EXPECT_EQ(select_target_cpu(t, c), c,
            "homogeneous topology -> select_target_cpu is the identity");
    }

    // A CPU-pinned thread never migrates regardless of topology/util.
    t->cpu_pinned = true;
    TEST_EXPECT_EQ(select_target_cpu(t, 0), 0u,
        "a cpu_pinned thread stays on its prev CPU");

    thread_free(t);
}

// scheduler.ready_on_cross_cpu_enqueue
//   The enqueue MECHANISM (ready_on) places a thread on an EXPLICIT target
//   CPU's run tree -- the load-bearing capability the placement policy + a
//   future misfit-push both consume. Notify is disabled during tests, so a
//   placed-on-a-parked-secondary thread sits still (deterministic) until
//   removed. At -smp 1 the self-target path is exercised instead.
void test_sched_ready_on_cross_cpu_enqueue(void) {
    struct Thread *t = thread_create(kproc(), sched_test_thread_a);
    TEST_ASSERT(t != NULL, "thread_create failed");
    TEST_EXPECT_EQ(t->state, THREAD_RUNNABLE, "fresh thread is RUNNABLE");

    unsigned self = smp_cpu_idx_self();

    if (smp_cpu_count() >= 2) {
        // Target a peer CPU explicitly. With notify disabled the secondary
        // stays parked (no per-CPU timer during tests, #810), so t sits in
        // cpu1's tree deterministically.
        unsigned peer = (self == 1) ? 0u : 1u;
        ready_on(peer, t);
        TEST_EXPECT_EQ(sched_in_cpu_tree(peer, t), true,
            "ready_on placed the thread on the target peer's run tree");
        TEST_EXPECT_EQ(sched_in_cpu_tree(self, t), false,
            "the thread is NOT on the caller's own tree (placed cross-CPU)");
        // #866 F1: cross-CPU placement set the TARGET's need_resched so it
        // reschedules + considers the placed thread at its next preempt point
        // (not after a full slice). need_resched_set is NOT notify-gated, so the
        // parked peer's flag is observably set here even with notify disabled.
        // The flag self-heals: the peer clears it at its first sched() entry
        // post-test (production transition), so it leaves no lasting state.
        TEST_EXPECT_EQ(sched_need_resched_pending(peer), true,
            "cross-CPU placement requested a reschedule on the target");
    } else {
        // -smp 1: only the self target exists; ready_on(self) == local enqueue.
        ready_on(self, t);
        TEST_EXPECT_EQ(sched_in_cpu_tree(self, t), true,
            "ready_on(self) placed the thread on the caller's run tree");
    }

    // Remove from whichever tree it landed in, then free.
    sched_remove_if_runnable(t);
    TEST_EXPECT_EQ(sched_in_cpu_tree(self, t), false, "removed from self tree");
    if (smp_cpu_count() >= 2) {
        unsigned peer = (self == 1) ? 0u : 1u;
        TEST_EXPECT_EQ(sched_in_cpu_tree(peer, t), false, "removed from peer tree");
    }
    thread_free(t);
}
