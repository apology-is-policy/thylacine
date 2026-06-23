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
void test_proc_orphan_reparent_smoke(void);
void test_proc_orphan_reparent_to_init(void);
void test_proc_console_attached_smoke(void);
void test_proc_stripes_smoke(void);
void test_proc_legate_scope_teardown(void);
void test_proc_legate_teardown_except_and_zero(void);
void test_proc_legate_teardown_from_zombie_chokepoint(void);

// Test-harness hooks (kept out of proc.h; no production caller). The legate
// teardown walk + the synthetic-Proc table splice let the test verify the walk
// flags every scope member -- the actual death fires at the EL0 die-check,
// which kernel test threads (EL1) never reach, so the flag is the observable.
extern void proc_test_link(struct Proc *p);
extern void proc_test_unlink(struct Proc *p);
extern void proc_test_legate_teardown(u32 scope_id, struct Proc *except);
extern void proc_test_set_init(struct Proc *p);

static volatile u32 g_proc_test_ran;
static volatile u64 g_cpu_run_count[DTB_MAX_CPUS];
static volatile u32 g_grandchild_ran;
static volatile u32 g_grandchild_status_seen;
static volatile u32 g_orphan_grandchild_ran;
static volatile u32 g_console_child_attached;
static volatile u32 g_console_parent_attached;
static volatile u64 g_stripes_child;

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
// proc_reparent_children / wait_pid_for's locked re-scan are all exercised in
// nested configurations.
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

// proc.orphan_reparent_smoke (R6-A F106 close)
//   boot rforks A; A rforks B (grandchild); A exits WITHOUT waiting for B
//   → A's proc_reparent_children moves B from A->children to kproc->children
//   under proc_table_lock; A goes ZOMBIE; B continues running. Boot reaps
//   A (and eventually B too — boot's wait_pid is on kproc, and B was
//   reparented to kproc, so boot can reap B in a subsequent wait_pid).
//
//   Since 2B-F3 this exercises the kproc FALLBACK leg of the adopter
//   selection (g_init_proc is NULL during the in-kernel test phase —
//   joey is created after test_run_all). The init leg is
//   proc.orphan_reparent_to_init below.
//
// This is the test that the cascading_rfork_stress did NOT exercise:
// proc_reparent_children with NON-EMPTY p->children. Without the
// proc_table_lock at this site, the F75 race would fire — A's reparent
// rewriting B->parent concurrently with B's exits reading B->parent for
// its own wakeup. With the lock, the rewrite is serialized; B sees a
// consistent post-rewrite p->parent (= kproc).
//
// Boot drains all kproc children via repeated wait_pid until no children
// remain. At v1.0 this test is non-leaking IF kproc's adopted orphans
// are reapable by boot — which they are because boot IS kproc's thread.
// (Future scenarios where kproc adopts orphans from a Proc whose parent
// is NOT boot would leak; v1.0 doesn't have such scenarios.)

static void orphan_grandchild_entry(void *arg) {
    (void)arg;
    __atomic_fetch_add(&g_orphan_grandchild_ran, 1, __ATOMIC_RELAXED);
    exits("ok");
}

static void parent_with_orphan_entry(void *arg) {
    (void)arg;
    int gc_pid = rfork(RFPROC, orphan_grandchild_entry, NULL);
    if (gc_pid <= 0) extinction("test: orphan_grandchild rfork failed");
    // Do NOT wait for the grandchild. exits() will reparent it to kproc
    // via proc_reparent_children — exercising the F75 lock-protected
    // path with non-empty p->children.
    exits("ok");
}

void test_proc_orphan_reparent_smoke(void) {
    u64 created_before     = proc_total_created();
    u64 destroyed_before   = proc_total_destroyed();
    u64 t_created_before   = thread_total_created();
    u64 t_destroyed_before = thread_total_destroyed();

    g_orphan_grandchild_ran = 0;

    int child_pid = rfork(RFPROC, parent_with_orphan_entry, NULL);
    TEST_ASSERT(child_pid > 0, "outer rfork failed");

    // Drain all kproc children. The F75 race is exercised at the moment
    // the orphan child enters proc_reparent_children with non-empty
    // p->children. Lock discipline ensures B->parent rewrite is atomic
    // with B's eventual exits-side wakeup. We don't deterministically
    // know the order of reaps (A first or B first depends on scheduler
    // interleaving), only that BOTH must complete.
    int reap_count = 0;
    int reaped_a   = 0;
    while (reap_count < 8) {                  // bounded safety; expect 2
        int status = -1;
        int reaped = wait_pid(&status);
        if (reaped <= 0) break;               // no more children
        reap_count++;
        if (reaped == child_pid) reaped_a = 1;
        TEST_EXPECT_EQ(status, 0,
            "orphan-reparent: reaped Proc had non-zero status");
    }

    TEST_ASSERT(reap_count == 2,
        "orphan-reparent: expected exactly 2 reaps (child + reparented "
        "grandchild); got count != 2 — F75 race may have occurred OR "
        "B never ran (work-stealing didn't pick it up)");
    TEST_ASSERT(reaped_a,
        "orphan-reparent: child (A) was not among the reaps");
    TEST_EXPECT_EQ(g_orphan_grandchild_ran, 1u,
        "orphan-reparent: grandchild did not run (couldn't have exits'd)");

    TEST_ASSERT(proc_total_created()     - created_before     == 2,
        "orphan-reparent: proc_total_created mismatch");
    TEST_ASSERT(proc_total_destroyed()   - destroyed_before   == 2,
        "orphan-reparent: proc_total_destroyed mismatch (LEAK?)");
    TEST_ASSERT(thread_total_created()   - t_created_before   == 2,
        "orphan-reparent: thread_total_created mismatch");
    TEST_ASSERT(thread_total_destroyed() - t_destroyed_before == 2,
        "orphan-reparent: thread_total_destroyed mismatch (Thread leak?)");
}

// =============================================================================
// 2B-F3: orphan reparent targets init (g_init_proc) when published.
//
// proc.orphan_reparent_to_init
//   boot rforks I (a stand-in init that spins until released, then reaps
//   its adopted orphan via wait_pid and exits); boot points g_init_proc
//   at I via proc_test_set_init. boot rforks A; A rforks B (grandchild,
//   spins until released) and exits WITHOUT waiting for B → B reparents
//   to I (NOT kproc — the pre-2B-F3 target; this assert fails on the
//   reparent-to-kproc code). boot releases B (it exits → ZOMBIE child of
//   I) and releases I, which reaps B — proving an adopted orphan is
//   reaped by init rather than leaking as a permanent zombie. Counters
//   balance 3/3 (no Proc-table leak).
//
//   While g_init_proc points at I, ANY orphaning exit in the system
//   would reparent there — safe because the test phase runs serially
//   (no concurrent orphan producers; console_mgr is a kproc Thread, not
//   a Proc). I's own death auto-clears g_init_proc at the
//   proc_become_zombie_locked chokepoint; the explicit
//   proc_test_set_init(NULL) restore is belt.
//
//   boot's reaps use wait_pid_for(pid) so they never steal I's adoptee —
//   the same by-pid discipline joey's supervisor loop uses against its
//   tracked children.
// =============================================================================

static volatile u32 g_oti_init_go;        // boot sets → init_sim may reap
static volatile u32 g_oti_init_reaped;    // pid init_sim reaped (0 = none yet)
static volatile u32 g_oti_gc_pid;         // grandchild pid, published by A
static volatile u32 g_oti_gc_release;     // boot sets → grandchild exits

static void oti_init_sim_entry(void *arg) {
    (void)arg;
    while (__atomic_load_n(&g_oti_init_go, __ATOMIC_ACQUIRE) == 0u)
        sched();
    // Sole child by now is the adopted orphan (zombie or about to be:
    // its exits wakes our child_waiters since we became its parent).
    int st = -1;
    int r = wait_pid(&st);
    __atomic_store_n(&g_oti_init_reaped, (u32)(r > 0 ? r : 0),
                     __ATOMIC_RELEASE);
    exits("ok");
}

static void oti_grandchild_entry(void *arg) {
    (void)arg;
    while (__atomic_load_n(&g_oti_gc_release, __ATOMIC_ACQUIRE) == 0u)
        sched();
    exits("ok");
}

static void oti_parent_entry(void *arg) {
    (void)arg;
    int gc_pid = rfork(RFPROC, oti_grandchild_entry, NULL);
    if (gc_pid <= 0) extinction("test: orphan-to-init grandchild rfork failed");
    __atomic_store_n(&g_oti_gc_pid, (u32)gc_pid, __ATOMIC_RELEASE);
    // Exit with B alive → proc_reparent_children with non-empty children,
    // targeting the published init.
    exits("ok");
}

// OTI_CHECK (audit F2): both 2B-F3 tests publish a stand-in init via
// proc_test_set_init; a bare TEST_ASSERT early-return after that point would
// strand g_init_proc pointing at a never-reaping spinner, so later tests'
// orphans would adopt into it and fail spuriously. OTI_CHECK routes the
// post-publish checks through the fail: epilogue, which restores
// g_init_proc = NULL and releases the gate flags. (A failed run extincts at
// the test-phase gate regardless; this only protects the diagnostic quality of
// the remaining tests in that run.)
#define OTI_CHECK(cond, msg) \
    do { if (!(cond)) { test_fail(msg); goto fail; } } while (0)

void test_proc_orphan_reparent_to_init(void) {
    u64 created_before   = proc_total_created();
    u64 destroyed_before = proc_total_destroyed();

    g_oti_init_go     = 0;
    g_oti_init_reaped = 0;
    g_oti_gc_pid      = 0;
    g_oti_gc_release  = 0;

    int init_pid = rfork(RFPROC, oti_init_sim_entry, NULL);
    TEST_ASSERT(init_pid > 0, "rfork init_sim failed");
    struct Proc *init_sim = proc_find_by_pid(init_pid);
    TEST_ASSERT(init_sim != NULL, "init_sim Proc not found by pid");
    proc_test_set_init(init_sim);   // every check past here is OTI_CHECK

    int parent_pid = rfork(RFPROC, oti_parent_entry, NULL);
    OTI_CHECK(parent_pid > 0, "rfork orphaning parent failed");

    int st = -1;
    int r = wait_pid_for(parent_pid, 0, &st);
    OTI_CHECK(r == parent_pid, "reap the orphaning parent by pid");
    OTI_CHECK(st == 0, "orphaning parent exit status");

    // A's ZOMBIE was observed through the proc_table_lock-held reap, so
    // its reparent (same critical section) is visible. B is still ALIVE
    // (spinning on g_oti_gc_release), so the pointer is stable: nothing
    // reaps it until init_sim does, and init_sim is gated on g_oti_init_go.
    u32 gc_pid = __atomic_load_n(&g_oti_gc_pid, __ATOMIC_ACQUIRE);
    OTI_CHECK(gc_pid != 0, "grandchild pid was not published");
    struct Proc *gc = proc_find_by_pid((int)gc_pid);
    OTI_CHECK(gc != NULL, "orphaned grandchild not found post-reparent");
    OTI_CHECK(gc->parent != kproc(),
        "orphan went to kproc despite a published init (pre-2B-F3 target)");
    OTI_CHECK(gc->parent == init_sim,
        "orphan reparented to g_init_proc (ARCH 7.9 step 6)");

    // Release B (exits → ZOMBIE child of init_sim), then let init_sim reap.
    __atomic_store_n(&g_oti_gc_release, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_oti_init_go, 1u, __ATOMIC_RELEASE);

    int ist = -1;
    int ir = wait_pid_for(init_pid, 0, &ist);
    OTI_CHECK(ir == init_pid, "reap init_sim by pid");
    OTI_CHECK(ist == 0, "init_sim exit status");
    OTI_CHECK(__atomic_load_n(&g_oti_init_reaped, __ATOMIC_ACQUIRE) == gc_pid,
        "init_sim reaped the adopted orphan (init reaps adoptees)");

    // init_sim's death auto-cleared g_init_proc (proc_become_zombie_locked
    // chokepoint); assert that, then restore explicitly as belt.
    OTI_CHECK(proc_init_proc() == NULL,
        "g_init_proc not auto-cleared at init death");
    proc_test_set_init(NULL);

    // 3 created (init_sim + A + B); 3 destroyed (A by boot, B by init_sim,
    // init_sim by boot) — adopted orphans do not leak the Proc table.
    TEST_ASSERT(proc_total_created()   - created_before   == 3,
        "orphan-to-init: proc_total_created mismatch");
    TEST_ASSERT(proc_total_destroyed() - destroyed_before == 3,
        "orphan-to-init: proc_total_destroyed mismatch (adopted-orphan LEAK?)");
    return;

fail:
    // Restore the adopter pointer (the load-bearing cleanup: later tests'
    // orphans must fall back to kproc, not a stranded stand-in) and release
    // the gates so the helpers exit rather than spin. Helper zombies may leak
    // until the failed run extincts at the test-phase gate.
    proc_test_set_init(NULL);
    __atomic_store_n(&g_oti_gc_release, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_oti_init_go, 1u, __ATOMIC_RELEASE);
}

// =============================================================================
// proc.orphan_reparent_zombie_to_init
//   The adopted-ALREADY-ZOMBIE leg of proc_reparent_children (the
//   `adopted_zombie` branch + its child_waiters wake), which
//   orphan_reparent_to_init does NOT exercise -- that test adopts the
//   grandchild while ALIVE, so the branch is never taken. Here B exits and
//   ZOMBIES *before* its parent A exits, so A's reparent moves a ZOMBIE to
//   init: the exact path that sets adopted_zombie and fires the adopted-zombie
//   wakeup, and that init must still be able to reap. boot makes the ordering
//   deterministic by polling B to ZOMBIE before releasing A -- no timing
//   dependence.
//
//   Coverage boundary: this does NOT place init BLOCKED in wait_pid at the
//   instant of adoption (where the wakeup is what releases it) -- that needs
//   init to already hold an alive child. A lost wake there surfaces as a
//   reap-hang caught by the boot timeout; the wake is the same child_waiters
//   primitive every exits() uses (no-lost-wake argument at the g_proc_table_-
//   lock header). Left as a reasoned residual.
// =============================================================================

static volatile u32 g_otz_go;          // boot → init_sim may reap + exit
static volatile u32 g_otz_a_go;        // boot → A may exit (after B has zombied)
static volatile u32 g_otz_b_pid;       // grandchild pid, published by A
static volatile u32 g_otz_init_reaped; // pid init_sim reaped (0 = none)

static void otz_init_sim_entry(void *arg) {
    (void)arg;
    while (__atomic_load_n(&g_otz_go, __ATOMIC_ACQUIRE) == 0u)
        sched();
    int st = -1;
    int r = wait_pid(&st);   // walks children, finds the adopted ZOMBIE, reaps
    __atomic_store_n(&g_otz_init_reaped, (u32)(r > 0 ? r : 0),
                     __ATOMIC_RELEASE);
    exits("ok");
}

static void otz_grandchild_entry(void *arg) {
    (void)arg;
    exits("ok");             // zombie immediately; child of A, unreaped
}

static void otz_parent_entry(void *arg) {
    (void)arg;
    int b = rfork(RFPROC, otz_grandchild_entry, NULL);
    if (b <= 0) extinction("test: orphan-zombie-to-init grandchild rfork failed");
    __atomic_store_n(&g_otz_b_pid, (u32)b, __ATOMIC_RELEASE);
    // Block until boot has confirmed B is ZOMBIE, then exit → A reparents a
    // ZOMBIE child (adopted_zombie == true, deterministically).
    while (__atomic_load_n(&g_otz_a_go, __ATOMIC_ACQUIRE) == 0u)
        sched();
    exits("ok");
}

void test_proc_orphan_reparent_zombie_to_init(void) {
    u64 created_before   = proc_total_created();
    u64 destroyed_before = proc_total_destroyed();

    g_otz_go = 0; g_otz_a_go = 0; g_otz_b_pid = 0; g_otz_init_reaped = 0;

    int init_pid = rfork(RFPROC, otz_init_sim_entry, NULL);
    TEST_ASSERT(init_pid > 0, "rfork init_sim failed");
    struct Proc *init_sim = proc_find_by_pid(init_pid);
    TEST_ASSERT(init_sim != NULL, "init_sim Proc not found by pid");
    proc_test_set_init(init_sim);   // every check past here is OTI_CHECK

    int parent_pid = rfork(RFPROC, otz_parent_entry, NULL);
    OTI_CHECK(parent_pid > 0, "rfork orphaning parent failed");

    // Wait for A to publish B, then poll B to ZOMBIE so A's exit adopts a
    // ZOMBIE (the branch under test). B is a child of A and unreaped here, so
    // the pointer is stable until init_sim reaps it at the very end.
    u32 b_pid = 0;
    while ((b_pid = __atomic_load_n(&g_otz_b_pid, __ATOMIC_ACQUIRE)) == 0u)
        sched();
    struct Proc *b = proc_find_by_pid((int)b_pid);
    OTI_CHECK(b != NULL, "grandchild B not found");
    while (__atomic_load_n(&b->state, __ATOMIC_ACQUIRE) != PROC_STATE_ZOMBIE)
        sched();

    // B is ZOMBIE; release A. A exits → proc_reparent_children moves the
    // ZOMBIE B to init_sim (the adopted_zombie path).
    __atomic_store_n(&g_otz_a_go, 1u, __ATOMIC_RELEASE);

    int st = -1;
    int r = wait_pid_for(parent_pid, 0, &st);
    OTI_CHECK(r == parent_pid, "reap the orphaning parent by pid");

    // B is now a ZOMBIE child of init_sim, still unreaped (init_sim gated).
    OTI_CHECK(b->parent == init_sim,
        "already-zombie orphan reparented to g_init_proc (adopted_zombie leg)");
    OTI_CHECK((int)__atomic_load_n(&b->state, __ATOMIC_ACQUIRE)
        == (int)PROC_STATE_ZOMBIE, "adopted orphan still ZOMBIE pre-init-reap");

    // Release init_sim; it reaps the adopted zombie and exits.
    __atomic_store_n(&g_otz_go, 1u, __ATOMIC_RELEASE);

    int ist = -1;
    int ir = wait_pid_for(init_pid, 0, &ist);
    OTI_CHECK(ir == init_pid, "reap init_sim by pid");
    OTI_CHECK(__atomic_load_n(&g_otz_init_reaped, __ATOMIC_ACQUIRE) == b_pid,
        "init_sim reaped the adopted ZOMBIE orphan");

    OTI_CHECK(proc_init_proc() == NULL,
        "g_init_proc not auto-cleared at init death");
    proc_test_set_init(NULL);

    // 3 created (init_sim + A + B); 3 destroyed (A by boot, B by init_sim,
    // init_sim by boot) — adopted ZOMBIE orphans do not leak the Proc table.
    TEST_ASSERT(proc_total_created()   - created_before   == 3,
        "orphan-zombie-to-init: proc_total_created mismatch");
    TEST_ASSERT(proc_total_destroyed() - destroyed_before == 3,
        "orphan-zombie-to-init: proc_total_destroyed mismatch (LEAK?)");
    return;

fail:
    // Same cleanup as the alive-adoption test: restore the adopter pointer and
    // release the gates so the helpers exit rather than spin.
    proc_test_set_init(NULL);
    __atomic_store_n(&g_otz_a_go, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_otz_go, 1u, __ATOMIC_RELEASE);
}

#undef OTI_CHECK

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

// =============================================================================
// P5-hostowner-a: console attachment.
// =============================================================================

// rfork'd child entry: record whether this child inherited console-
// attachment. It must NOT — PROC_FLAG_CONSOLE_ATTACHED is conferred
// only by an explicit proc_mark_console_attached, never by rfork
// (specs/corvus.tla: console_attached grows solely via MarkConsole-
// Attached).
static void console_attach_child_entry(void *arg) {
    (void)arg;
    struct Thread *t = current_thread();
    struct Proc *self = t ? t->proc : NULL;
    g_console_child_attached = proc_is_console_attached(self) ? 1u : 0u;
    exits("ok");
}

// rfork'd intermediate entry: mark SELF console-attached, then rfork a
// grandchild. The grandchild (console_attach_child_entry) must NOT
// inherit the bit — proving rfork never propagates console-attachment
// even from a parent that genuinely holds it. A buggy rfork_internal
// that copied parent->proc_flags would fail this case (the kproc-rooted
// case alone would not: kproc is un-attached, so a buggy copy of 0
// still yields 0).
static void console_attach_parent_entry(void *arg) {
    (void)arg;
    struct Thread *t = current_thread();
    struct Proc *self = t ? t->proc : NULL;
    if (!self) extinction("test: console parent has no proc");
    proc_mark_console_attached(self);
    g_console_parent_attached = proc_is_console_attached(self) ? 1u : 0u;

    int gc_pid = rfork(RFPROC, console_attach_child_entry, NULL);
    if (gc_pid <= 0) extinction("test: console grandchild rfork failed");
    int gc_status = -1;
    int reaped = wait_pid(&gc_status);
    if (reaped != gc_pid)
        extinction("test: console parent wait_pid returned wrong pid");
    exits("ok");
}

// proc.console_attached_smoke
//   Verifies the P5-hostowner-a console-attachment mechanism:
//     * proc_is_console_attached(NULL) is false (fail-closed).
//     * A fresh Proc is not console-attached.
//     * proc_mark_console_attached sets the bit; it is idempotent.
//     * A rfork'd child does NOT inherit the bit.
void test_proc_console_attached_smoke(void) {
    // Fail-closed: NULL reads as not-attached.
    TEST_ASSERT(!proc_is_console_attached(NULL),
        "proc_is_console_attached(NULL) must be false (fail-closed)");

    // A fresh Proc is unattached; the mark sets it; the mark is a
    // one-way set — idempotent, never cleared.
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_ASSERT(!proc_is_console_attached(p),
        "a fresh Proc must not be console-attached");
    proc_mark_console_attached(p);
    TEST_ASSERT(proc_is_console_attached(p),
        "proc_mark_console_attached must set the console bit");
    proc_mark_console_attached(p);
    TEST_ASSERT(proc_is_console_attached(p),
        "proc_mark_console_attached must be idempotent (one-way set)");
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);

    // rfork must NOT propagate the console bit. The child of the
    // current (kproc-context) thread starts un-attached — pinning the
    // spec rule that console_attached grows only via an explicit
    // MarkConsoleAttached, so a future remote-login chain cannot
    // inherit the local-console trust anchor.
    g_console_child_attached = 0xFFu;
    int child_pid = rfork(RFPROC, console_attach_child_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, child_pid, "wait_pid returned wrong pid");
    TEST_EXPECT_EQ(status, 0, "console-attach child should exit clean");
    TEST_EXPECT_EQ(g_console_child_attached, 0u,
        "a rfork'd child must NOT inherit console-attachment");

    // Non-propagation from a CONSOLE-ATTACHED parent. An intermediate
    // Proc marks ITSELF console-attached, then rforks a grandchild; the
    // grandchild must start un-attached. This is the case that fails
    // against a buggy rfork_internal copying parent->proc_flags — the
    // kproc-rooted case above would not (kproc is un-attached).
    g_console_parent_attached = 0xFFu;
    g_console_child_attached  = 0xFFu;
    int parent_pid = rfork(RFPROC, console_attach_parent_entry, NULL);
    TEST_ASSERT(parent_pid > 0, "rfork (console-attached parent) failed");
    int pstatus = -1;
    int preaped = wait_pid(&pstatus);
    TEST_EXPECT_EQ(preaped, parent_pid,
        "wait_pid returned wrong pid (console parent)");
    TEST_EXPECT_EQ(pstatus, 0, "console-attached parent should exit clean");
    TEST_EXPECT_EQ(g_console_parent_attached, 1u,
        "the intermediate parent must be console-attached after self-mark");
    TEST_EXPECT_EQ(g_console_child_attached, 0u,
        "a grandchild of a console-attached parent must NOT inherit the bit");
}

// =============================================================================
// SYS_EXIT_GROUP / cross-thread shootdown (ARCH §7.9.1, invariant I-24).
// =============================================================================

// proc.group_terminate_smoke
//   Kernel-side contract for proc_group_terminate + el0_return_die_check. The
//   FULL cascade (every peer Thread self-exits at its EL0-return die-check; the
//   last out reaps the Proc) is a userspace-EL0 behavior proven by the
//   stratumd-shutdown E2E (the test.sh de-flake) -- a kernel rfork child runs
//   in the kernel and never hits an EL0-return tail, so it can't exercise that
//   path here. This pins what IS unit-testable:
//     * a NULL Proc is a safe no-op (fail-safe);
//     * group_exit_msg is set-once (CAS; the first caller's msg wins, so the
//       last-Thread-out ZOMBIE status is the first-declared one);
//     * el0_return_die_check no-ops (RETURNS) on a Proc not group-terminating;
//     * terminating one Proc does not flag another (isolation).
void test_proc_group_terminate_smoke(void) {
    // Fail-safe: a NULL Proc is a quiet no-op -- it returns at the magic check
    // BEFORE the p->threads walk, so it needs no g_proc_table_lock.
    proc_group_terminate(NULL, "fail");

    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_ASSERT(p->group_exit_msg == NULL,
        "a fresh Proc must not be group-terminating (KP_ZERO -> NULL)");

    // First group-terminate publishes the msg. Also exercises the empty-bucket
    // torpor_wake_all_for_proc + the empty p->threads cascade walk +
    // smp_resched_others -- all safe no-ops here (no torpor waiters, no peers).
    // #811: proc_group_terminate's contract requires g_proc_table_lock held
    // (the cascade walks p->threads, which races thread_free). p has no threads
    // so the walk is empty, but hold the lock so the test models the contract.
    {
        irq_state_t s = proc_table_lock_acquire();
        proc_group_terminate(p, "fail");
        proc_table_lock_release(s);
    }
    TEST_ASSERT(p->group_exit_msg != NULL,
        "proc_group_terminate must publish group_exit_msg");
    TEST_EXPECT_EQ((int)p->group_exit_msg[0], (int)'f',
        "group_exit_msg must be the first caller's msg (\"fail\")");

    // Set-once (CAS): a racing second group-terminate does NOT overwrite the
    // first msg -- the last-Thread-out ZOMBIE status stays the first-declared.
    const char *first = p->group_exit_msg;
    {
        irq_state_t s = proc_table_lock_acquire();
        proc_group_terminate(p, "ok");
        proc_table_lock_release(s);
    }
    TEST_ASSERT(p->group_exit_msg == first,
        "proc_group_terminate is set-once (first msg wins; CAS idempotent)");

    // Isolation + el0_return_die_check no-op: the test thread's own Proc is
    // NOT group-terminating, so el0_return_die_check must RETURN here (it must
    // not self-exit the test thread). Asserting the Proc is un-flagged first
    // guards the call from accidentally terminating the harness.
    struct Thread *t = current_thread();
    struct Proc *self = t ? t->proc : NULL;
    TEST_ASSERT(self != NULL && self->group_exit_msg == NULL,
        "terminating p must not flag the test thread's Proc (isolation)");
    el0_return_die_check();   // must return -- self is not group-terminating

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// =============================================================================
// P5-corvus-srv: per-Proc identity tag (`stripes`).
// =============================================================================

// rfork'd child entry: record this child's own `stripes` tag. It must
// be non-zero AND differ from the parent's — proc_alloc mints a fresh
// tag for every Proc, so an rfork child's stripes is never the
// parent's (CORVUS-DESIGN.md §6.3; specs/corvus.tla: a Proc's identity
// is per-Proc distinct, never inherited).
static void stripes_child_entry(void *arg) {
    (void)arg;
    struct Thread *t = current_thread();
    struct Proc *self = t ? t->proc : NULL;
    __atomic_store_n(&g_stripes_child, proc_stripes(self), __ATOMIC_RELAXED);
    exits("ok");
}

// proc.stripes_smoke
//   Verifies the P5-corvus-srv per-Proc identity tag:
//     * proc_stripes(NULL) is 0 — the fail-closed sentinel.
//     * kproc carries a non-zero tag.
//     * proc_alloc mints a fresh, non-zero, mutually-distinct tag per
//       Proc, none aliasing kproc's.
//     * an rfork'd child's tag is non-zero and differs from its
//       parent's — the tag is minted, never inherited.
void test_proc_stripes_smoke(void) {
    // Fail-closed: a NULL Proc reads as stripes 0.
    TEST_EXPECT_EQ(proc_stripes(NULL), (u64)0,
        "proc_stripes(NULL) must be 0 (fail-closed sentinel)");

    // kproc carries a real, non-zero tag (0 is reserved).
    u64 kp = proc_stripes(kproc());
    TEST_ASSERT(kp != 0, "kproc must carry a non-zero stripes tag");

    // proc_alloc mints fresh, non-zero, mutually-distinct tags.
    struct Proc *a = proc_alloc();
    TEST_ASSERT(a != NULL, "proc_alloc(a) returned NULL");
    struct Proc *b = proc_alloc();
    TEST_ASSERT(b != NULL, "proc_alloc(b) returned NULL");
    u64 sa = proc_stripes(a);
    u64 sb = proc_stripes(b);
    TEST_ASSERT(sa != 0, "Proc a must carry a non-zero stripes tag");
    TEST_ASSERT(sb != 0, "Proc b must carry a non-zero stripes tag");
    TEST_ASSERT(sa != sb, "distinct Procs must carry distinct stripes tags");
    TEST_ASSERT(sa != kp && sb != kp,
        "a proc_alloc'd Proc must not alias kproc's stripes tag");
    a->state = PROC_STATE_ZOMBIE;
    proc_free(a);
    b->state = PROC_STATE_ZOMBIE;
    proc_free(b);

    // An rfork'd child gets a minted tag, NOT the parent's. The parent
    // is the current (kproc-context) thread; the child's recorded tag
    // must differ from kproc's. The counter is monotonic and the child
    // is created after kproc, so a buggy rfork that copied
    // parent->stripes would yield exactly kp — caught here.
    __atomic_store_n(&g_stripes_child, 0, __ATOMIC_RELAXED);
    int child_pid = rfork(RFPROC, stripes_child_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, child_pid, "wait_pid returned wrong pid");
    TEST_EXPECT_EQ(status, 0, "stripes child should exit clean");
    u64 child_stripes = __atomic_load_n(&g_stripes_child, __ATOMIC_RELAXED);
    TEST_ASSERT(child_stripes != 0,
        "an rfork'd child must carry a non-zero stripes tag");
    TEST_ASSERT(child_stripes != kp,
        "an rfork'd child's stripes must differ from the parent's "
        "(minted fresh, never inherited)");
}

// =============================================================================
// A-4a legate scope teardown (I-25). Synthetic Procs spliced into the table
// with a chosen legate_scope_id; the teardown walk must group-terminate (set
// group_exit_msg) exactly the matching members, sparing non-members, the
// `except` Proc (the legate root on its own exit), and kproc. Synthetic Procs
// have no running thread, so flagging them is inert -- the flag is the
// unit-testable observable (the death step is the already-tested #809/#811
// EL0-die-check path).
// =============================================================================

static struct Proc *legate_make_linked(u32 scope) {
    struct Proc *p = proc_alloc();
    if (!p) return NULL;
    p->legate_scope_id = scope;
    proc_test_link(p);
    return p;
}

static void legate_drop_linked(struct Proc *p) {
    if (!p) return;
    proc_test_unlink(p);
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

void test_proc_legate_scope_teardown(void) {
    // A,B share scope 0xA; C is in a different scope 0xB (the control).
    struct Proc *a = legate_make_linked(0xAu);
    struct Proc *b = legate_make_linked(0xAu);
    struct Proc *c = legate_make_linked(0xBu);
    TEST_ASSERT(a && b && c, "alloc + link synthetic Procs");

    proc_test_legate_teardown(0xAu, NULL);

    TEST_ASSERT(a->group_exit_msg != NULL, "scope-0xA member A flagged");
    TEST_ASSERT(b->group_exit_msg != NULL, "scope-0xA member B flagged");
    TEST_EXPECT_EQ(c->group_exit_msg, (const char *)NULL,
        "scope-0xB member C NOT flagged (different scope)");
    TEST_EXPECT_EQ(kproc()->group_exit_msg, (const char *)NULL,
        "kproc NEVER flagged by a legate teardown");

    legate_drop_linked(a);
    legate_drop_linked(b);
    legate_drop_linked(c);
}

void test_proc_legate_teardown_except_and_zero(void) {
    // `except`: the legate root (trigger 1) is spared; the other member dies.
    struct Proc *root = legate_make_linked(0xCu);
    struct Proc *mem  = legate_make_linked(0xCu);
    TEST_ASSERT(root && mem, "alloc + link");

    proc_test_legate_teardown(0xCu, root);   // except = root
    TEST_EXPECT_EQ(root->group_exit_msg, (const char *)NULL,
        "the except (root) Proc is NOT flagged");
    TEST_ASSERT(mem->group_exit_msg != NULL, "the other scope member IS flagged");

    legate_drop_linked(root);
    legate_drop_linked(mem);

    // scope_id == 0 is the not-a-legate sentinel: a teardown for scope 0 (or
    // against a normal scope-0 Proc) must be a TOTAL no-op -- never nuke every
    // ordinary Proc. This guards the catastrophic mis-call.
    struct Proc *normal = legate_make_linked(0u);   // scope_id 0 == normal Proc
    TEST_ASSERT(normal != NULL, "alloc + link normal Proc");

    proc_test_legate_teardown(0u, NULL);            // scope 0 -> no-op
    TEST_EXPECT_EQ(normal->group_exit_msg, (const char *)NULL,
        "scope-0 teardown does NOT flag a normal Proc");
    TEST_EXPECT_EQ(kproc()->group_exit_msg, (const char *)NULL,
        "scope-0 teardown does NOT flag kproc");

    proc_test_legate_teardown(0xDEADu, NULL);       // no member has this scope
    TEST_EXPECT_EQ(normal->group_exit_msg, (const char *)NULL,
        "teardown of an unmatched scope flags nobody");

    legate_drop_linked(normal);
}

// A-4a audit F1 regression: the legate-root scope teardown fires from the SHARED
// zombie chokepoint (proc_become_zombie_locked -> proc_legate_teardown_if_root),
// so a root that dies via a kill / group-terminate path -- not only via exits()
// -- still sweeps its scope. And the GUARD: only a Proc carrying
// PROC_FLAG_LEGATE_ROOT triggers the sweep, so a dying scope MEMBER (scope_id
// set, no ROOT flag) must NOT tear down the whole group. proc_legate_teardown_-
// if_root requires the caller hold g_proc_table_lock (it uses the LOCKED walk),
// mirroring its production call site inside proc_become_zombie_locked.
void test_proc_legate_teardown_from_zombie_chokepoint(void) {
    struct Proc *root = legate_make_linked(0x5Eu);
    struct Proc *mem  = legate_make_linked(0x5Eu);
    TEST_ASSERT(root && mem, "alloc + link root + member (scope 0x5E)");

    // (a) A non-ROOT Proc at the chokepoint is a no-op: a dying MEMBER does not
    //     sweep its scope (pre-F1, the teardown lived only in exits(); the guard
    //     here is what keeps a member death from nuking the group).
    irq_state_t s = proc_table_lock_acquire();
    proc_legate_teardown_if_root(mem);            // mem carries scope but no ROOT flag
    proc_table_lock_release(s);
    TEST_EXPECT_EQ(root->group_exit_msg, (const char *)NULL,
        "a non-root member at the chokepoint does NOT flag the scope");
    TEST_EXPECT_EQ(mem->group_exit_msg, (const char *)NULL,
        "a non-root member at the chokepoint does NOT flag itself");

    // (b) The ROOT at the chokepoint sweeps the scope (member flagged), sparing
    //     itself (except = the dying root, which the zombie transition handles).
    __atomic_fetch_or(&root->proc_flags, PROC_FLAG_LEGATE_ROOT, __ATOMIC_RELEASE);
    s = proc_table_lock_acquire();
    proc_legate_teardown_if_root(root);
    proc_table_lock_release(s);
    TEST_ASSERT(mem->group_exit_msg != NULL,
        "the legate root at the chokepoint flags the scope member");
    TEST_EXPECT_EQ(root->group_exit_msg, (const char *)NULL,
        "the legate root spares itself (except = p)");

    legate_drop_linked(root);
    legate_drop_linked(mem);
}

// =============================================================================
// U-7-pre: SYS_WAIT_PID v2 — wait_pid_for (pid filter + WAIT_WNOHANG)
// =============================================================================
//
// wait_pid_for generalizes wait_pid: select a specific child (want_pid > 0)
// and/or poll without blocking (WAIT_WNOHANG). These tests prosecute the
// selection logic + the WNOHANG sentinel; the teardown path is shared with
// wait_pid and exercised by the rfork/exits tests above.

static volatile u32 g_wpf_release;   // parent sets → the spinner child exits

static void wpf_spinner_thunk(void *arg) {
    (void)arg;
    // Stay alive until the parent releases us, yielding cooperatively so we
    // never starve a peer on an oversubscribed CPU set. This keeps the child
    // reliably ALIVE (not a zombie) at the parent's WNOHANG poll.
    while (__atomic_load_n(&g_wpf_release, __ATOMIC_ACQUIRE) == 0u)
        sched();
    exits("ok");
}

static void wpf_exiter_thunk(void *arg) {
    (void)arg;
    exits("ok");                     // exits immediately → becomes a zombie
}

// A want_pid that is not a child returns -1 immediately under BOTH blocking
// and WNOHANG modes — it must NOT block waiting for a child that can never
// appear (the `!any_match → -1` path).
void test_proc_wait_pid_for_no_match(void) {
    int st = -42;
    int r1 = wait_pid_for(0x7ffffff0, 0, &st);
    TEST_EXPECT_EQ(r1, -1, "wait_pid_for(absent pid, blocking) returns -1 (no block)");
    int r2 = wait_pid_for(0x7ffffff0, WAIT_WNOHANG, &st);
    TEST_EXPECT_EQ(r2, -1, "wait_pid_for(absent pid, WNOHANG) returns -1");
}

// WAIT_WNOHANG reports a live-but-not-yet-zombie child as 0 (not ready),
// then a blocking wait_pid_for reaps it once released.
void test_proc_wait_pid_for_wnohang_alive_then_reap(void) {
    __atomic_store_n(&g_wpf_release, 0u, __ATOMIC_RELEASE);
    int pid = rfork(RFPROC, wpf_spinner_thunk, NULL);
    TEST_ASSERT(pid > 0, "rfork spinner failed");

    int st = -42;
    int r = wait_pid_for(pid, WAIT_WNOHANG, &st);
    TEST_EXPECT_EQ(r, 0, "WNOHANG on a live child returns 0 (not ready)");

    __atomic_store_n(&g_wpf_release, 1u, __ATOMIC_RELEASE);   // let it exit
    int reaped = wait_pid_for(pid, 0, &st);
    TEST_EXPECT_EQ(reaped, pid, "blocking wait_pid_for reaps the released child");
    TEST_EXPECT_EQ(st, 0, "spinner exit status");
}

// The pid filter selects the named child even when ANOTHER child is a ready
// zombie: WNOHANG on the live spinner returns 0 (it IGNORES the exited
// child's zombie), then each child is reaped by its own pid. A reap-any bug
// would return exit_pid from the WNOHANG poll once the exiter zombies — the
// SMP gate amplifies that window.
void test_proc_wait_pid_for_selects_target(void) {
    __atomic_store_n(&g_wpf_release, 0u, __ATOMIC_RELEASE);
    int spin_pid = rfork(RFPROC, wpf_spinner_thunk, NULL);
    TEST_ASSERT(spin_pid > 0, "rfork spinner failed");
    int exit_pid = rfork(RFPROC, wpf_exiter_thunk, NULL);
    TEST_ASSERT(exit_pid > 0, "rfork exiter failed");

    // Drive until the exiter is a ready zombie (its own WNOHANG poll reaps it).
    // Each iteration polls the SPINNER with WNOHANG *first* and asserts 0 —
    // with NO sched() between the two polls. So on the final iteration (the one
    // where the exiter poll reaps it, proving it was a present zombie) the
    // spinner poll ran with that zombie present: a reap-any-WNOHANG bug would
    // return exit_pid there; a correct pid filter returns 0. This makes the
    // "ignore a present zombie" case DETERMINISTIC (audit F4), not best-effort.
    int st = -42;
    int reaped_exit;
    for (;;) {
        int r_spin = wait_pid_for(spin_pid, WAIT_WNOHANG, &st);
        TEST_EXPECT_EQ(r_spin, 0, "WNOHANG selects the live spinner, never the exiter zombie");
        reaped_exit = wait_pid_for(exit_pid, WAIT_WNOHANG, &st);
        if (reaped_exit == exit_pid) break;     // exiter was a zombie this iter → reaped
        TEST_EXPECT_EQ(reaped_exit, 0, "exiter WNOHANG is 0 (alive) or its pid (reaped)");
        sched();                                 // let the exiter make progress to exits()
    }
    TEST_EXPECT_EQ(st, 0, "exiter status");

    __atomic_store_n(&g_wpf_release, 1u, __ATOMIC_RELEASE);
    int reaped_spin = wait_pid_for(spin_pid, 0, &st);
    TEST_EXPECT_EQ(reaped_spin, spin_pid, "reap the spinner by its own pid");
    TEST_EXPECT_EQ(st, 0, "spinner status");
}
