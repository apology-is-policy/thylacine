// rfork / exits / wait_pid lifecycle tests (P2-D / P2-Db).
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
//   proc.rfork_stress_1000
//     1000 iterations of rfork → exits → wait_pid in batches of 8.
//     Verifies proc_total_destroyed advances by 1000 and slub baseline
//     returns to within tolerance — primary leak check at the multi-
//     process surface (P2-A trip-hazards #80, #81 + the lifecycle gates
//     in proc_free + thread_free). Per-CPU run counters track which
//     CPU(s) executed children; sum must equal ITERS.
//
// At v1.0 P2-Db the children run in kernel mode (no userspace yet);
// rfork takes a kernel function pointer as entry. The full userspace
// rfork-from-syscall split lands at P2-G with the ELF loader.
//
// Cross-CPU placement note: at this phase secondaries do not run their
// own timer IRQs (per-CPU timer init lands later) and rfork doesn't
// send IPI_RESCHED to peers. Children placed by ready() into the local
// run tree are typically picked by the same CPU before secondaries
// have a chance to steal — so per-CPU work distribution is opportunistic
// rather than guaranteed at this phase. A dedicated cross-CPU test
// lands once either secondary timers OR rfork-driven IPI_RESCHED is
// wired (post-P2-Db; tracked as an open follow-up).

#include "test.h"

#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/smp.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

void test_proc_rfork_basic_smoke(void);
void test_proc_rfork_exits_status(void);
void test_proc_rfork_stress_1000(void);

static volatile u32 g_proc_test_ran;
static volatile u64 g_cpu_run_count[DTB_MAX_CPUS];

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

static void proc_test_child_record_cpu(void *arg) {
    (void)arg;
    unsigned cpu = smp_cpu_idx_self();
    if (cpu < DTB_MAX_CPUS) {
        __atomic_fetch_add(&g_cpu_run_count[cpu], 1, __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&g_proc_test_ran, 1, __ATOMIC_RELAXED);
    exits("ok");
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

// Stress: 1000 iterations of rfork → exits → wait_pid in batches of 8.
// Verifies no leak in the descriptor caches + alloc_pages baseline.
// Per-CPU run counters track which CPU(s) ran the children; sum must
// equal ITERS.
void test_proc_rfork_stress_1000(void) {
    enum { ITERS = 1000 };
    enum { BATCH = 8 };       // rfork BATCH children before reaping any

    u64 created_before     = proc_total_created();
    u64 destroyed_before   = proc_total_destroyed();
    u64 t_created_before   = thread_total_created();
    u64 t_destroyed_before = thread_total_destroyed();

    g_proc_test_ran = 0;
    for (unsigned i = 0; i < DTB_MAX_CPUS; i++) g_cpu_run_count[i] = 0;

    int done = 0;
    while (done < ITERS) {
        int batch = (ITERS - done < BATCH) ? (ITERS - done) : BATCH;

        // Fan out `batch` children into the local run tree.
        for (int j = 0; j < batch; j++) {
            int pid = rfork(RFPROC, proc_test_child_record_cpu, NULL);
            TEST_ASSERT(pid > 0, "rfork failed mid-stress");
        }

        // Reap them all.
        for (int j = 0; j < batch; j++) {
            int status = -1;
            int reaped_pid = wait_pid(&status);
            TEST_ASSERT(reaped_pid > 0, "wait_pid failed mid-stress");
            TEST_EXPECT_EQ(status, 0, "stress child wrong status");
        }

        done += batch;
    }

    u32 ran = __atomic_load_n(&g_proc_test_ran, __ATOMIC_RELAXED);
    TEST_EXPECT_EQ(ran, (u32)ITERS, "child ran count mismatch");
    TEST_ASSERT(proc_total_created() - created_before == ITERS,
        "proc_total_created didn't track ITERS");
    TEST_ASSERT(proc_total_destroyed() - destroyed_before == ITERS,
        "proc_total_destroyed didn't track ITERS");
    TEST_ASSERT(thread_total_created() - t_created_before == ITERS,
        "thread_total_created didn't track ITERS");
    TEST_ASSERT(thread_total_destroyed() - t_destroyed_before == ITERS,
        "thread_total_destroyed didn't track ITERS");

    u64 sum = 0;
    for (unsigned i = 0; i < DTB_MAX_CPUS; i++) {
        sum += g_cpu_run_count[i];
    }
    TEST_ASSERT(sum == (u64)ITERS,
        "per-CPU run counters' sum mismatch with ITERS");
}
