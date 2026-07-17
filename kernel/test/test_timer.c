// timer leaf-API smoke. Verifies the tick counter advances after a
// brief WFI loop. Tests the IRQ delivery path end-to-end: the GIC's
// distributor/redist/CPU-interface init, the IRQ vector wiring in
// vectors.S, exception_irq_curr_el's ack/dispatch/EOI, and the
// timer's CNTP_TVAL_EL0 reload. If any step is broken, ticks never
// arrive and the WFI loop wedges forever.
//
// The test runs AFTER timer_busy_wait_ticks(5) in boot_main, so by
// the time this test executes the kernel has already proven the
// IRQ path is live. We re-check from the test for the audit trail
// (and to catch any regression where the boot-path tick observation
// passes but the harness later fails).

#include "test.h"

#include "../../arch/arm64/timer.h"
#include <thylacine/types.h>

void test_timer_tick_increments(void) {
    TEST_ASSERT(timer_get_freq() > 0,
        "timer_get_freq is zero (timer_init didn't run?)");

    u64 t0 = timer_get_ticks();
    timer_busy_wait_ticks(2);
    u64 t1 = timer_get_ticks();

    TEST_ASSERT(t1 > t0,
        "tick counter did not advance after busy_wait_ticks(2)");
    TEST_ASSERT((t1 - t0) >= 2,
        "tick counter advanced fewer than 2 (lost IRQ?)");
}

// Tickless idle one-shot (NO_HZ_IDLE; docs/TICKLESS-IDLE.md TI-1). The clamp
// helper is the testable core of timer_arm_oneshot_cnt -- the CNTV_TVAL reload
// (target - now) bounded to [TIMER_MIN_RELOAD, TIMER_MAX_RELOAD]. Pure, no live
// timer touched: an arbitrary counter base drives every clamp boundary.
void test_timer_oneshot_tval_clamps(void) {
    u64 now = 1000000;

    // In-range: a comfortable delta ahead returns that delta exactly.
    TEST_EXPECT_EQ(timer_oneshot_tval(now + 50000, now), 50000u,
        "in-range one-shot tval must equal the delta");

    // target == now -> delta 0 -> clamp UP to MIN (never a 0/negative reload).
    TEST_EXPECT_EQ(timer_oneshot_tval(now, now), TIMER_MIN_RELOAD,
        "target==now must clamp up to TIMER_MIN_RELOAD");

    // target < now (overdue) -> delta 0 -> clamp UP to MIN (fire ASAP).
    TEST_EXPECT_EQ(timer_oneshot_tval(now - 999, now), TIMER_MIN_RELOAD,
        "overdue target must clamp up to TIMER_MIN_RELOAD");

    // A delta just below MIN clamps up to MIN.
    TEST_EXPECT_EQ(timer_oneshot_tval(now + (TIMER_MIN_RELOAD - 1), now),
        TIMER_MIN_RELOAD,
        "sub-MIN delta must clamp up to TIMER_MIN_RELOAD");

    // A delta past the 32-bit horizon clamps DOWN to MAX.
    TEST_EXPECT_EQ(timer_oneshot_tval(now + ((u64)TIMER_MAX_RELOAD + 1000), now),
        TIMER_MAX_RELOAD,
        "over-MAX delta must clamp down to TIMER_MAX_RELOAD");

    // Exactly MAX stays MAX.
    TEST_EXPECT_EQ(timer_oneshot_tval(now + TIMER_MAX_RELOAD, now),
        TIMER_MAX_RELOAD,
        "delta == MAX must stay MAX");
}

// timer_arm_oneshot_cnt is a thin CNTV write (read CNTVCT, write the clamped
// TVAL, ENABLE). Prove it does not wedge the per-CPU timer: arm a far one-shot,
// restore periodic via timer_arm_this_cpu, then confirm 1 kHz ticks resume. The
// one-shot is overwritten before it could fire, so the suite's ticking is
// undisturbed (NO behavior change at TI-1 -- no production path arms a one-shot
// yet; this drives the primitive directly). Runs on cpu0 (the test CPU), where
// g_ticks advances.
void test_timer_arm_oneshot_restores(void) {
    u64 now = timer_get_counter();
    timer_arm_oneshot_cnt(now + (u64)timer_get_freq());   // ~1 s out
    timer_arm_this_cpu();                                 // restore periodic

    u64 t0 = timer_get_ticks();
    timer_busy_wait_ticks(2);
    TEST_ASSERT(timer_get_ticks() > t0,
        "periodic ticks must resume after a one-shot arm + restore");
}
