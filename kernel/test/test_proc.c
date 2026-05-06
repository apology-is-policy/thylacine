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

#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/smp.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

void test_proc_rfork_basic_smoke(void);
void test_proc_rfork_exits_status(void);
void test_proc_rfork_stress_1000(void);
void test_proc_cascading_rfork_wait_smoke(void);
void test_proc_cascading_rfork_stress(void);

static volatile u32 g_proc_test_ran;
static volatile u64 g_cpu_run_count[DTB_MAX_CPUS];
static volatile u32 g_grandchild_ran;
static volatile u32 g_grandchild_status_seen;

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

// =============================================================================
// P3-A (R5-H F75 close): cascading-lineage tests.
//
// Exercises three-level lineage (boot → child → grandchild) so the
// proc-table-lock paths in proc_link_child / proc_unlink_child /
// proc_reparent_children / wait_pid_cond are all exercised in nested
// configurations.
//
// At v1.0 P3-A this is also a regression-protector against future
// changes that bypass the locking discipline. The tests don't
// deterministically trigger the F75 race (timing-dependent on SMP
// interleaving), but they DO exercise every locked path. Combined with
// work-stealing's opportunistic placement, the stress variant has a
// real chance of triggering the race window and would surface a panic
// or a counter mismatch under regression.
// =============================================================================

static void grandchild_entry_ok(void *arg) {
    (void)arg;
    __atomic_fetch_add(&g_grandchild_ran, 1, __ATOMIC_RELAXED);
    exits("ok");
}

static void child_entry_with_grandchild_wait(void *arg) {
    (void)arg;

    int gc_pid = rfork(RFPROC, grandchild_entry_ok, NULL);
    if (gc_pid <= 0) extinction("test: grandchild rfork failed");

    int gc_status = -1;
    int reaped = wait_pid(&gc_status);
    if (reaped != gc_pid)
        extinction("test: child wait_pid returned wrong pid");
    if (gc_status == 0)
        __atomic_fetch_add(&g_grandchild_status_seen, 1, __ATOMIC_RELAXED);

    exits("ok");
}

// proc.cascading_rfork_wait_smoke
//   boot rforks A; A rforks B; B exits; A waits for B (reaps); A exits;
//   boot waits for A (reaps).
//
// Exercises:
//   * Multi-level rfork (boot's rfork → A's rfork from inside its kthread)
//   * Nested wait_pid (A waits for B; boot waits for A)
//   * Nested exits (B exits → wakes A; A exits → wakes boot)
//   * The proc-table-lock paths under nested concurrency on SMP
//
// At v1.0 P3-A: with work-stealing, A may run on a secondary CPU and
// B may run on yet another CPU — exercising the cross-CPU lineage path.
// All Procs are reaped; no leaks.
void test_proc_cascading_rfork_wait_smoke(void) {
    u64 created_before     = proc_total_created();
    u64 destroyed_before   = proc_total_destroyed();
    u64 t_created_before   = thread_total_created();
    u64 t_destroyed_before = thread_total_destroyed();

    g_grandchild_ran          = 0;
    g_grandchild_status_seen  = 0;

    int child_pid = rfork(RFPROC, child_entry_with_grandchild_wait, NULL);
    TEST_ASSERT(child_pid > 0, "outer rfork failed");

    int child_status = -1;
    int reaped = wait_pid(&child_status);
    TEST_EXPECT_EQ(reaped, child_pid,
        "boot wait_pid returned wrong pid");
    TEST_EXPECT_EQ(child_status, 0,
        "child status not 0 (cascading exits chain broken)");

    u32 gc_ran    = __atomic_load_n(&g_grandchild_ran, __ATOMIC_RELAXED);
    u32 gc_status = __atomic_load_n(&g_grandchild_status_seen, __ATOMIC_RELAXED);
    TEST_EXPECT_EQ(gc_ran, 1u,
        "grandchild did not run");
    TEST_EXPECT_EQ(gc_status, 1u,
        "grandchild status not observed by child wait_pid");

    // 2 procs created (child + grandchild); 2 destroyed (child reaped by
    // boot; grandchild reaped by child). 2 threads similarly.
    TEST_ASSERT(proc_total_created()   - created_before     == 2,
        "cascading: proc_total_created mismatch");
    TEST_ASSERT(proc_total_destroyed() - destroyed_before   == 2,
        "cascading: proc_total_destroyed mismatch (leaked Proc?)");
    TEST_ASSERT(thread_total_created()   - t_created_before == 2,
        "cascading: thread_total_created mismatch");
    TEST_ASSERT(thread_total_destroyed() - t_destroyed_before == 2,
        "cascading: thread_total_destroyed mismatch (leaked Thread?)");
}

// proc.cascading_rfork_stress
//   100 iterations of the cascading_rfork_wait_smoke pattern.
//
// At 100 iterations × 3 levels of lineage × 4 vCPUs work-stealing, the
// odds of hitting the F75 race window (parent in proc_reparent_children
// while child reads p->parent) are non-zero across the run. Without
// the proc-table lock, this test would historically have produced
// occasional UAF panics ("wakeup: corrupted waiter" or "extinction"
// from a freed Rendez). With the lock landed at P3-A, the stress
// completes with all counters balanced — a regression check that any
// future change that omits the lock from a lineage mutator will fail.
//
// Bounded at 100 (not 1000) to keep boot time under the 500 ms budget;
// the cascading path is expensive (3-level wait_pid chains).
void test_proc_cascading_rfork_stress(void) {
    enum { ITERS = 100 };

    u64 created_before     = proc_total_created();
    u64 destroyed_before   = proc_total_destroyed();
    u64 t_created_before   = thread_total_created();
    u64 t_destroyed_before = thread_total_destroyed();

    g_grandchild_ran         = 0;
    g_grandchild_status_seen = 0;

    for (int i = 0; i < ITERS; i++) {
        int child_pid = rfork(RFPROC, child_entry_with_grandchild_wait, NULL);
        TEST_ASSERT(child_pid > 0, "rfork failed mid-stress");
        int child_status = -1;
        int reaped = wait_pid(&child_status);
        TEST_ASSERT(reaped == child_pid,
            "wait_pid returned wrong pid mid-stress");
        TEST_EXPECT_EQ(child_status, 0,
            "child status not 0 mid-stress");
    }

    u32 gc_ran    = __atomic_load_n(&g_grandchild_ran, __ATOMIC_RELAXED);
    u32 gc_status = __atomic_load_n(&g_grandchild_status_seen, __ATOMIC_RELAXED);
    TEST_EXPECT_EQ(gc_ran, (u32)ITERS,
        "grandchild run count mismatch (some iteration's grandchild didn't run)");
    TEST_EXPECT_EQ(gc_status, (u32)ITERS,
        "grandchild status observation count mismatch");

    // Per iteration: 2 procs + 2 threads created/destroyed.
    TEST_ASSERT(proc_total_created()     - created_before     == 2 * ITERS,
        "cascading-stress: proc_total_created mismatch");
    TEST_ASSERT(proc_total_destroyed()   - destroyed_before   == 2 * ITERS,
        "cascading-stress: proc_total_destroyed mismatch (leak?)");
    TEST_ASSERT(thread_total_created()   - t_created_before   == 2 * ITERS,
        "cascading-stress: thread_total_created mismatch");
    TEST_ASSERT(thread_total_destroyed() - t_destroyed_before == 2 * ITERS,
        "cascading-stress: thread_total_destroyed mismatch (leak?)");
}
