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
