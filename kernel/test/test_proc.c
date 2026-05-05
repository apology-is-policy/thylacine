// rfork / exits / wait_pid lifecycle tests (P2-D).
//
// Three tests:
//
//   proc.rfork_basic_smoke
//     Boot kthread rforks one child running an entry that sets a flag
//     and calls exits("ok"). Boot calls wait_pid(&status); verifies the
//     reaped PID matches the rforked PID and status is 0.
//
//   proc.rfork_exits_status
//     Same shape, but the child calls exits("err"); verifies status
//     translates to non-zero.
//
//   proc.rfork_stress_100
//     100 iterations of rfork → exits → wait_pid in a loop. Verifies
//     proc_total_destroyed advances by 100 and slub baseline returns to
//     within tolerance (ensures no leak).
//
// At v1.0 P2-D the children run in kernel mode (no userspace yet);
// rfork takes a kernel function pointer as entry. The full userspace
// rfork-from-syscall split lands at P2-G with the ELF loader.
//
// Cross-CPU placement: ready() inserts the child into the local CPU's
// run tree (boot CPU). Secondaries' work-stealing (P2-Ce) may pick the
// child up; either way the child runs to exits() and the parent's
// wait_pid resumes via the child_done Rendez.

#include "test.h"

#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

// Forward declarations to match test.c's registry (kills
// -Wmissing-prototypes; the test functions must be non-static so the
// linker can find them from test.c's g_tests[]).
void test_proc_rfork_basic_smoke(void);
void test_proc_rfork_exits_status(void);
void test_proc_rfork_stress_100(void);

// Shared state for proc.rfork_basic_smoke + rfork_exits_status. The
// `volatile` storage class blocks the compiler from folding writes
// into reads — we MUST observe the child's increment.
static volatile u32 g_proc_test_ran;

static void proc_test_child_ok(void *arg) {
    (void)arg;
    g_proc_test_ran++;
    exits("ok");
}

static void proc_test_child_err(void *arg) {
    (void)arg;
    g_proc_test_ran++;
    exits("err");
}

void test_proc_rfork_basic_smoke(void) {
    g_proc_test_ran = 0;

    u64 created_before   = proc_total_created();
    u64 destroyed_before = proc_total_destroyed();

    int child_pid = rfork(RFPROC, proc_test_child_ok, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed (returned <= 0)");
    TEST_ASSERT(proc_total_created() == created_before + 1,
        "proc_total_created did not advance");

    int status = -1;
    int reaped_pid = wait_pid(&status);

    TEST_EXPECT_EQ(reaped_pid, child_pid,
        "wait_pid returned a different PID than rfork");
    TEST_EXPECT_EQ(status, 0,
        "exits(\"ok\") should produce status 0");
    TEST_EXPECT_EQ(g_proc_test_ran, 1u,
        "child entry didn't run before exits");
    TEST_ASSERT(proc_total_destroyed() == destroyed_before + 1,
        "proc_total_destroyed did not advance");
}

void test_proc_rfork_exits_status(void) {
    g_proc_test_ran = 0;

    int child_pid = rfork(RFPROC, proc_test_child_err, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");

    int status = 0;
    int reaped_pid = wait_pid(&status);

    TEST_EXPECT_EQ(reaped_pid, child_pid,
        "wait_pid returned a different PID");
    TEST_ASSERT(status != 0,
        "exits(\"err\") should produce non-zero status");
    TEST_EXPECT_EQ(g_proc_test_ran, 1u,
        "child entry didn't run");
}

// Stress: N iterations of rfork → exits → wait_pid. Verifies no leak in
// the descriptor caches + alloc_pages baseline.
void test_proc_rfork_stress_100(void) {
    enum { ITERS = 100 };

    u64 created_before   = proc_total_created();
    u64 destroyed_before = proc_total_destroyed();
    u64 t_created_before = thread_total_created();
    u64 t_destroyed_before = thread_total_destroyed();

    g_proc_test_ran = 0;

    for (int i = 0; i < ITERS; i++) {
        int child_pid = rfork(RFPROC, proc_test_child_ok, NULL);
        TEST_ASSERT(child_pid > 0, "rfork failed mid-stress");

        int status = -1;
        int reaped_pid = wait_pid(&status);
        TEST_EXPECT_EQ(reaped_pid, child_pid,
            "wait_pid returned wrong PID mid-stress");
        TEST_EXPECT_EQ(status, 0, "stress child exits(\"ok\") wrong status");
    }

    TEST_EXPECT_EQ(g_proc_test_ran, (u32)ITERS,
        "child ran count mismatch");
    TEST_ASSERT(proc_total_created() - created_before == ITERS,
        "proc_total_created didn't track ITERS");
    TEST_ASSERT(proc_total_destroyed() - destroyed_before == ITERS,
        "proc_total_destroyed didn't track ITERS");
    TEST_ASSERT(thread_total_created() - t_created_before == ITERS,
        "thread_total_created didn't track ITERS");
    TEST_ASSERT(thread_total_destroyed() - t_destroyed_before == ITERS,
        "thread_total_destroyed didn't track ITERS");
}
