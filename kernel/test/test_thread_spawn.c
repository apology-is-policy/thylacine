// P6-pouch-threads (sub-chunk 9): SYS_THREAD_SPAWN + SYS_THREAD_EXIT
// + multi-thread-Proc reap tests.
//
// Coverage:
//
//   thread.create_user_ctx_layout         — thread_create_user lays out
//                                            ctx for the thread_user_-
//                                            trampoline path with the
//                                            four user-mode args in the
//                                            right callee-saved slots.
//   thread.exit_self_marks_exiting        — thread_exit_self on a peer
//                                            Thread of a non-kproc Proc
//                                            transitions state to
//                                            THREAD_EXITING; Proc stays
//                                            ALIVE if peers remain.
//   thread.exit_self_last_thread_zombies  — last live Thread calling
//                                            thread_exit_self transitions
//                                            the Proc to ZOMBIE with
//                                            exit_status = 0.
//   proc.multi_thread_reap                — rfork a child Proc, spawn
//                                            peer Threads in it, each
//                                            calls thread_exit_self,
//                                            main calls exits → every
//                                            Thread freed by wait_pid's
//                                            return.
//   proc.thread_reap_churn                — XT-3b: 1200 spawn+exit
//                                            cycles in one non-exempt
//                                            Proc; the cap counts live
//                                            threads, exited ones are
//                                            freed while the Proc lives.
//   proc.thread_reap_inflight             — XT-3b: a retired Thread
//                                            still switching away
//                                            survives the reap and holds
//                                            up exec's drain (task #19).
//   proc.thread_reap_concurrent           — XT-3b: three peers spawn,
//                                            reap and exit at once; every
//                                            Thread freed exactly once.
//
// The kernel test harness runs at EL1 so we can't drive a full eret-to-
// EL0 from here; the user-mode SVC dispatch + the userland_enter eret
// dance are exercised by the joey-side smoke + the /thread-probe binary
// (sub-chunk 9c, end-to-end). Here we focus on the kernel-internal
// state machine.

#include "test.h"

#include "../../arch/arm64/mmu.h"   // mmu_kernel_ttbr0_pa (RW-1 B-F1 ctx.ttbr0)

#include <thylacine/context.h>
#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

void test_thread_create_user_ctx_layout(void);
void test_thread_exit_self_marks_exiting(void);
void test_thread_exit_self_last_thread_zombies(void);
void test_proc_multi_thread_reap(void);
void test_proc_thread_reap_churn(void);
void test_proc_thread_reap_inflight(void);
void test_proc_thread_reap_concurrent(void);

// ---------------------------------------------------------------------------
// thread.create_user_ctx_layout
// ---------------------------------------------------------------------------

void test_thread_create_user_ctx_layout(void) {
    // Allocate a bare Proc — we only check ctx layout, no scheduling, no
    // user-VA dereference. The Proc carries a real pgtable_root + asid so
    // ctx.ttbr0 picks up the proper (asid<<48 | pgtable_root) value, but
    // we don't actually swap into TTBR0.
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    u64 entry = 0x100000ull;
    u64 sp    = 0x000000007ffff000ull;       // inside EXEC_USER_STACK range, 16-aligned
    u64 arg   = 0xdeadbeefcafef00dull;
    u64 tls   = 0x12345000ull;

    struct Thread *t = thread_create_user(p, entry, sp, arg, tls);
    TEST_ASSERT(t != NULL, "thread_create_user failed");

    TEST_EXPECT_EQ(t->ctx.x19, sp,    "ctx.x19 = user_sp_va");
    TEST_EXPECT_EQ(t->ctx.x20, arg,   "ctx.x20 = user_arg");
    TEST_EXPECT_EQ(t->ctx.x21, entry, "ctx.x21 = user_entry_va");
    TEST_EXPECT_EQ(t->ctx.x22, tls,   "ctx.x22 = user_tls_va");
    TEST_EXPECT_EQ(t->ctx.lr,
                   (u64)(uintptr_t)thread_user_trampoline,
                   "ctx.lr = thread_user_trampoline");
    TEST_ASSERT(t->state == THREAD_RUNNABLE,
                "fresh thread is RUNNABLE");
    TEST_ASSERT(t->proc == p, "Thread's proc pointer");
    TEST_ASSERT(t->kstack_base != NULL, "Thread has kstack");
    // RW-1 B-F1: ctx.ttbr0 is baked to the kernel TTBR0 at create; the rolling
    // ASID user value (asid << 48 | pgtable_root) is installed by the context-
    // switch pre-hook before the first switch, not at create.
    TEST_EXPECT_EQ(t->ctx.ttbr0, (u64)mmu_kernel_ttbr0_pa(),
                   "ctx.ttbr0 = kernel TTBR0 at create (rolling ASID set at switch)");
    TEST_EXPECT_EQ(t->clear_child_tid, 0ull,
                   "clear_child_tid defaults to 0 (unset)");

    // Cleanup — the Thread is RUNNABLE but never ready()-inserted, so
    // sched_remove_if_runnable is a no-op. thread_free handles the rest.
    thread_free(t);

    // Proc has no live threads / no children / state ALIVE — proc_free
    // requires state == ZOMBIE; transition manually for the test cleanup.
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// thread.exit_self_marks_exiting
//
// rfork a child Proc. In the child's entry, spawn 2 peer Threads (kernel-
// mode entries — we test the state-machine, not eret-to-EL0). Each peer
// calls thread_exit_self. Main yields until both peers reach EXITING,
// then calls exits("ok"). Parent wait_pid reaps the whole Proc.
// ---------------------------------------------------------------------------

static volatile u32 g_workers_ran;

static void mte_worker_entry(void *arg) {
    (void)arg;
    __atomic_fetch_add(&g_workers_ran, 1u, __ATOMIC_RELEASE);
    thread_exit_self();
}

static void mte_parent_entry(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;

    for (int i = 0; i < 2; i++) {
        struct Thread *peer = thread_create_with_arg(p, mte_worker_entry, NULL);
        if (!peer) extinction("mte_parent_entry: thread_create_with_arg failed");
        ready(peer);
    }

    // Yield until every peer Thread has exited. A peer's EXITING commit
    // retires it off p->threads in the same g_proc_table_lock hold (XT-3b),
    // so the live count falling back to 1 (this thread) says exactly that.
    // Not a walk of p->threads: lock-free, a peer's reap may free the node
    // the walk stands on.
    TEST_YIELD_UNTIL_PROC(__atomic_load_n(&p->thread_count, __ATOMIC_ACQUIRE) == 1);

    // Self-check: Proc is still ALIVE — neither peer's thread_exit_self
    // counted as last live thread (main is still alive).
    if (p->state != PROC_STATE_ALIVE) extinction("Proc state != ALIVE after peer exits");

    exits("ok");
}

void test_thread_exit_self_marks_exiting(void) {
    __atomic_store_n(&g_workers_ran, 0u, __ATOMIC_RELEASE);

    u64 t_created_before   = thread_total_created();
    u64 t_destroyed_before = thread_total_destroyed();

    int child_pid = rfork(RFPROC, mte_parent_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");

    int status = -1;
    int reaped_pid = wait_pid(&status);
    TEST_EXPECT_EQ(reaped_pid, child_pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0, "child exited cleanly");
    TEST_EXPECT_EQ(__atomic_load_n(&g_workers_ran, __ATOMIC_ACQUIRE), 2u,
                   "both workers should have run");

    // 3 threads created in the child Proc (main + 2 peers); all 3 freed by
    // the time wait_pid returns (by a peer's reap or by wait_pid itself).
    u64 t_created_delta   = thread_total_created()   - t_created_before;
    u64 t_destroyed_delta = thread_total_destroyed() - t_destroyed_before;
    TEST_EXPECT_EQ(t_created_delta, 3u,
                   "3 Threads created in the child Proc");
    TEST_EXPECT_EQ(t_destroyed_delta, 3u,
                   "wait_pid drained all 3 Threads");
}

// ---------------------------------------------------------------------------
// thread.exit_self_last_thread_zombies
//
// A non-rfork'd child where the FIRST thread to call thread_exit_self
// is the main thread itself. This exercises the "this is the last live
// Thread" path in thread_exit_self — Proc transitions to ZOMBIE with
// exit_status = 0 (mirrors exits("ok")).
// ---------------------------------------------------------------------------

static void ltz_entry(void *arg) {
    (void)arg;
    // No peers spawned — the calling Thread IS the only one. Call
    // thread_exit_self directly. The kernel detects "last live" and
    // ZOMBIEs the Proc with status 0.
    thread_exit_self();
}

void test_thread_exit_self_last_thread_zombies(void) {
    int child_pid = rfork(RFPROC, ltz_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");

    int status = -1;
    int reaped_pid = wait_pid(&status);
    TEST_EXPECT_EQ(reaped_pid, child_pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0,
                   "thread_exit_self last-Thread path uses exit_status=0");
}

// ---------------------------------------------------------------------------
// proc.multi_thread_reap
//
// Larger reap — 5 peers; every Thread is freed by the time wait_pid returns,
// whichever freer got it (a peer's reap at its exit, or wait_pid's drain of
// the zombie's live and retired lists -- XT-3b).
// ---------------------------------------------------------------------------

#define MTR_N_WORKERS 5

static volatile u32 g_mtr_workers_ran;

static void mtr_worker_entry(void *arg) {
    (void)arg;
    __atomic_fetch_add(&g_mtr_workers_ran, 1u, __ATOMIC_RELEASE);
    thread_exit_self();
}

static void mtr_parent_entry(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;
    for (int i = 0; i < MTR_N_WORKERS; i++) {
        struct Thread *peer = thread_create_with_arg(p, mtr_worker_entry, NULL);
        if (!peer) extinction("mtr_parent_entry: thread_create_with_arg failed");
        ready(peer);
    }

    // As in mte_parent_entry: the live count, never a lock-free list walk.
    TEST_YIELD_UNTIL_PROC(__atomic_load_n(&p->thread_count, __ATOMIC_ACQUIRE) == 1);

    exits("ok");
}

void test_proc_multi_thread_reap(void) {
    __atomic_store_n(&g_mtr_workers_ran, 0u, __ATOMIC_RELEASE);

    u64 t_created_before   = thread_total_created();
    u64 t_destroyed_before = thread_total_destroyed();

    int child_pid = rfork(RFPROC, mtr_parent_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");

    int status = -1;
    int reaped_pid = wait_pid(&status);
    TEST_EXPECT_EQ(reaped_pid, child_pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0, "child exited cleanly");
    TEST_EXPECT_EQ(__atomic_load_n(&g_mtr_workers_ran, __ATOMIC_ACQUIRE),
                   (u32)MTR_N_WORKERS,
                   "all workers ran");

    u64 t_created_delta   = thread_total_created()   - t_created_before;
    u64 t_destroyed_delta = thread_total_destroyed() - t_destroyed_before;
    TEST_EXPECT_EQ(t_created_delta, (u64)(MTR_N_WORKERS + 1),
                   "N+1 Threads created");
    TEST_EXPECT_EQ(t_destroyed_delta, (u64)(MTR_N_WORKERS + 1),
                   "wait_pid drained all N+1 Threads");
}

// ---------------------------------------------------------------------------
// proc.wait_pid_concurrent_waiters_both_reap  (#344, the multi-waiter lift)
//
// #344 retired the RW-2 2B-F1/F2 `wait_active` guard that REFUSED a 2nd
// concurrent same-Proc waiter with -1. Now ANY number of a Proc's Threads may
// be inside wait_pid_for at once: each registers its OWN poll_waiter on the
// Proc's `child_waiters` list and parks on its OWN private rendez; a child's
// ZOMBIE transition wakes ALL registered waiters; exactly one reaps each zombie.
//
// Two worker Threads of one Proc each call wait_pid_for(-1) once, with TWO live
// (spinning) children present. Both workers MUST park concurrently -- the very
// thing the old single-waiter `child_done` Rendez could not survive (a 2nd
// sleeper tripped sleep's single-waiter assert, which is the whole reason the
// guard existed). The children then exit; each worker reaps a DISTINCT child.
//
// FAILS PRE-FIX: with the wait_active guard, exactly one worker reaps and the
// 2nd is refused (-1) -> reaped_count == 1, neg == 1. POST-FIX both reap ->
// reaped_count == 2, neg == 0, and the two reaped pids are the two children.
// (Pre-fix WITHOUT the guard would instead extinct on the 2nd concurrent
// sleeper; "both park + both reap" is precisely the property #344 makes safe.)
// ---------------------------------------------------------------------------

#define WPCW_NCHILD 2
static volatile u32 g_wpcw_go;             // children spin until set
static volatile u32 g_wpcw_entered;        // workers that ENTERED wait_pid_for
static volatile u32 g_wpcw_reaped_count;   // workers that reaped (rc > 0)
static volatile u32 g_wpcw_neg;            // workers that returned <= 0
static volatile int g_wpcw_pid[WPCW_NCHILD];   // pids the workers reaped
static volatile u32 g_wpcw_pid_n;          // next free slot in g_wpcw_pid
static volatile u32 g_wpcw_done;           // workers finished

static void wpcw_child_entry(void *arg) {
    (void)arg;
    // Stay a live (non-zombie) child until BOTH workers have parked, so the
    // concurrent-park path -- the #344 property -- is exercised, not dodged.
    TEST_YIELD_UNTIL_PROC(__atomic_load_n(&g_wpcw_go, __ATOMIC_ACQUIRE) != 0u);
    exits("ok");
}

static void wpcw_worker_entry(void *arg) {
    (void)arg;
    __atomic_fetch_add(&g_wpcw_entered, 1u, __ATOMIC_ACQ_REL);
    int status = -123;
    int rc = wait_pid_for(-1, 0, &status);
    if (rc > 0) {
        u32 slot = __atomic_fetch_add(&g_wpcw_pid_n, 1u, __ATOMIC_ACQ_REL);
        if (slot < (u32)WPCW_NCHILD)
            __atomic_store_n(&g_wpcw_pid[slot], rc, __ATOMIC_RELEASE);
        __atomic_fetch_add(&g_wpcw_reaped_count, 1u, __ATOMIC_ACQ_REL);
    } else {
        __atomic_fetch_add(&g_wpcw_neg, 1u, __ATOMIC_ACQ_REL);
    }
    __atomic_fetch_add(&g_wpcw_done, 1u, __ATOMIC_ACQ_REL);
    thread_exit_self();
}

static void wpcw_parent_entry(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;

    int c0 = rfork(RFPROC, wpcw_child_entry, NULL);
    int c1 = rfork(RFPROC, wpcw_child_entry, NULL);
    if (c0 <= 0 || c1 <= 0) extinction("wpcw: rfork child failed");

    struct Thread *w0 = thread_create_with_arg(p, wpcw_worker_entry, NULL);
    struct Thread *w1 = thread_create_with_arg(p, wpcw_worker_entry, NULL);
    if (!w0 || !w1) extinction("wpcw: thread_create_with_arg failed");
    ready(w0);
    ready(w1);

    // Wait until BOTH workers have ENTERED wait_pid_for, then spin so they both
    // reach the park (the children are still alive -> no zombie -> both MUST
    // sleep concurrently, the #344 property). Only THEN release the children.
    TEST_YIELD_UNTIL_PROC(__atomic_load_n(&g_wpcw_entered,
                                          __ATOMIC_ACQUIRE) >= 2u);
    for (int i = 0; i < 1000; i++) sched();

    __atomic_store_n(&g_wpcw_go, 1u, __ATOMIC_RELEASE);   // both children exit

    TEST_YIELD_UNTIL_PROC(__atomic_load_n(&g_wpcw_done,
                                          __ATOMIC_ACQUIRE) >= 2u);
    exits("ok");
}

void test_proc_wait_pid_concurrent_waiters_both_reap(void) {
    __atomic_store_n(&g_wpcw_go, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wpcw_entered, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wpcw_reaped_count, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wpcw_neg, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wpcw_pid_n, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wpcw_done, 0u, __ATOMIC_RELEASE);
    for (int i = 0; i < WPCW_NCHILD; i++)
        __atomic_store_n(&g_wpcw_pid[i], 0, __ATOMIC_RELEASE);

    int pid = rfork(RFPROC, wpcw_parent_entry, NULL);
    TEST_ASSERT(pid > 0, "rfork parent failed");

    int status = -1;
    int reaped = wait_pid(&status);   // boot reaps the multi-thread test Proc
    TEST_EXPECT_EQ(reaped, pid, "reaped the multi-thread test Proc");

    // #344: BOTH concurrent same-Proc waiters reaped a child. Pre-fix the
    // wait_active guard refused the 2nd -> reaped_count would be 1, neg 1.
    TEST_EXPECT_EQ(__atomic_load_n(&g_wpcw_reaped_count, __ATOMIC_ACQUIRE), 2u,
        "both concurrent same-Proc waiters reaped a child (#344 multi-waiter; "
        "pre-fix the wait_active guard refused the 2nd -> only 1 reaped)");
    TEST_EXPECT_EQ(__atomic_load_n(&g_wpcw_neg, __ATOMIC_ACQUIRE), 0u,
        "neither concurrent waiter was refused / returned <= 0 (#344)");
    int p0 = __atomic_load_n(&g_wpcw_pid[0], __ATOMIC_ACQUIRE);
    int p1 = __atomic_load_n(&g_wpcw_pid[1], __ATOMIC_ACQUIRE);
    TEST_ASSERT(p0 > 0 && p1 > 0, "both reaped pids recorded");
    TEST_ASSERT(p0 != p1, "the two waiters reaped DISTINCT children");
}

// ===========================================================================
// XT-3b: per-thread reaping (specs/thread_reap.tla). A Thread that exits while
// a peer lives on RETIRES off p->threads, and a live peer frees it at its next
// spawn or exit once its switch away has settled.
// ===========================================================================

void proc_retire_for_test(struct Proc *p, struct Thread *t);
unsigned proc_retired_count_for_test(struct Proc *p);

// A non-exempt principal (test_resource.c's A_REAL_USER): the TCB is exempt
// from PROC_THREAD_MAX, and a Proc rforked by the runner inherits the TCB's.
#define REAP_TEST_USER 1000u

// ---------------------------------------------------------------------------
// proc.thread_reap_churn
//
// One Proc spawns and retires a worker TRC_SPAWNS times -- more than four
// times PROC_THREAD_MAX -- running the spawn handler's two steps, in its
// order, before each spawn: proc_reap_retired, then proc_thread_cap_ok.
//
// FAILS PRE-FIX in either half. Without the retire, every exited worker stays
// on p->threads and the cap refuses spawn 256: the lifetime cap (study F3).
// Without the reap, thread_count stays at 1 but nothing is freed while the
// Proc lives: the retired list grows without bound and no free lands early.
// ---------------------------------------------------------------------------

#define TRC_SPAWNS 1200u

static volatile u32 g_trc_ran;
static volatile u32 g_trc_refused_at;    // 1 + the spawn the cap refused; 0 = none
static volatile u64 g_trc_freed_alive;   // Threads freed while the Proc lived
static volatile u32 g_trc_retired_max;   // the longest the retired list got
static volatile u32 g_trc_cpu_dipped;    // proc_cpu_ns went backwards across a reap
static volatile u32 g_trc_focus_kept;    // a debug focus outlived its retired Thread

static void trc_worker_entry(void *arg) {
    (void)arg;
    __atomic_fetch_add(&g_trc_ran, 1u, __ATOMIC_RELEASE);
    thread_exit_self();
}

static void trc_parent_entry(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;
    __atomic_store_n(&p->principal_id, REAP_TEST_USER, __ATOMIC_RELEASE);

    u64 freed0  = thread_total_destroyed();
    u64 cpu_was = 0;
    for (u32 i = 0; i < TRC_SPAWNS; i++) {
        proc_reap_retired(p);
        if (!proc_thread_cap_ok(p)) {
            __atomic_store_n(&g_trc_refused_at, i + 1u, __ATOMIC_RELEASE);
            break;
        }
        struct Thread *w = thread_create_with_arg(p, trc_worker_entry, NULL);
        if (!w) extinction("trc: thread_create_with_arg failed");
        if (i == 0u) __atomic_store_n(&p->debug_focus_thread, w, __ATOMIC_RELEASE);
        ready(w);
        // This is the Proc's only other live Thread, so nothing but its own
        // next proc_reap_retired can free w: reading w until then is safe.
        TEST_YIELD_UNTIL_PROC(__atomic_load_n(&w->state, __ATOMIC_ACQUIRE) == THREAD_EXITING);
        if (i == 0u && __atomic_load_n(&p->debug_focus_thread, __ATOMIC_ACQUIRE) == w)
            __atomic_store_n(&g_trc_focus_kept, 1u, __ATOMIC_RELEASE);

        u32 retired = proc_retired_count_for_test(p);
        if (retired > g_trc_retired_max) g_trc_retired_max = retired;
        irq_state_t s = proc_table_lock_acquire();
        u64 cpu = proc_cpu_ns(p);
        proc_table_lock_release(s);
        if (cpu < cpu_was) __atomic_store_n(&g_trc_cpu_dipped, 1u, __ATOMIC_RELEASE);
        cpu_was = cpu;
    }
    __atomic_store_n(&g_trc_freed_alive, thread_total_destroyed() - freed0,
                     __ATOMIC_RELEASE);
    exits("ok");
}

void test_proc_thread_reap_churn(void) {
    __atomic_store_n(&g_trc_ran, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_trc_refused_at, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_trc_freed_alive, 0ull, __ATOMIC_RELEASE);
    __atomic_store_n(&g_trc_retired_max, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_trc_cpu_dipped, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_trc_focus_kept, 0u, __ATOMIC_RELEASE);

    u64 created0   = thread_total_created();
    u64 destroyed0 = thread_total_destroyed();

    int pid = rfork(RFPROC, trc_parent_entry, NULL);
    TEST_ASSERT(pid > 0, "rfork failed");
    int status = -1;
    TEST_EXPECT_EQ(wait_pid(&status), pid, "wait_pid reaps the churning Proc");
    TEST_EXPECT_EQ(status, 0, "the churning Proc exited cleanly");

    TEST_EXPECT_EQ(__atomic_load_n(&g_trc_refused_at, __ATOMIC_ACQUIRE), 0u,
        "no spawn refused: PROC_THREAD_MAX counts live threads, not the "
        "Proc's history (study F3 -- pre-fix the cap refuses spawn 256)");
    TEST_EXPECT_EQ(__atomic_load_n(&g_trc_ran, __ATOMIC_ACQUIRE), TRC_SPAWNS,
        "every worker ran");
    TEST_ASSERT(__atomic_load_n(&g_trc_freed_alive, __ATOMIC_ACQUIRE) + 2u >= TRC_SPAWNS,
        "exited workers were freed while their Proc lived (the reap points)");
    TEST_ASSERT(__atomic_load_n(&g_trc_retired_max, __ATOMIC_ACQUIRE) <= 4u,
        "the retired list stays bounded (I-32: retired-but-allocated per Proc)");
    TEST_EXPECT_EQ(__atomic_load_n(&g_trc_cpu_dipped, __ATOMIC_ACQUIRE), 0u,
        "proc_cpu_ns never dips across a reap (the commit folds and unlinks in one hold)");
    TEST_EXPECT_EQ(__atomic_load_n(&g_trc_focus_kept, __ATOMIC_ACQUIRE), 0u,
        "a debug focus on a Thread is cleared when it retires (no recycled-slot match)");
    TEST_EXPECT_EQ(thread_total_created() - created0, (u64)TRC_SPAWNS + 1u,
        "TRC_SPAWNS workers + the main thread created");
    TEST_EXPECT_EQ(thread_total_destroyed() - destroyed0, (u64)TRC_SPAWNS + 1u,
        "every one of them freed by the time wait_pid returns");
}

// ---------------------------------------------------------------------------
// proc.thread_reap_inflight
//
// A retired Thread whose switch away has not completed must survive the reap
// and must hold up exec's drain. The test retires a Thread it built and never
// ran, holding its on_cpu set to stand in for that switch; a helper Proc
// clears it, as the destination CPU would, once the drain is under way.
//
// FAILS PRE-FIX: a reap that ignored on_cpu takes the Thread, and its free then
// spins until the helper's fallback release -- the test sees the free land
// (thread_reap.tla BUGGY_REAP_IGNORES_ONCPU). A drain that returns with the
// Thread still in flight is exec freeing the old address space under a tail
// that still stores into it (task #19, BUGGY_EXEC_NO_DRAIN).
// ---------------------------------------------------------------------------

static volatile u32 g_tri_draining;     // the Proc is about to drain
static volatile u32 g_tri_released;     // the helper cleared on_cpu
static volatile u32 g_tri_reap_took;    // the reap freed the in-flight Thread
static volatile u32 g_tri_drain_early;  // the drain returned before the release
static volatile u32 g_tri_drain_left;   // the drain left a retired Thread behind

static void tri_never_runs(void *arg) {
    (void)arg;
    extinction("tri_never_runs: a never-readied Thread ran");
}

static void tri_helper_entry(void *arg) {
    struct Thread *f = (struct Thread *)arg;
    // Released ~20 ms after the drain starts. The 100 ms fallback is for a
    // buggy reap that spins on f before the drain is ever reached: it must
    // end in a failed assert, never a hung boot.
    u64 fallback = timer_now_ns() + 100ull * 1000ull * 1000ull;
    while (__atomic_load_n(&g_tri_draining, __ATOMIC_ACQUIRE) == 0u &&
           timer_now_ns() < fallback)
        sched();
    u64 until = timer_now_ns() + 20ull * 1000ull * 1000ull;
    while (timer_now_ns() < until) sched();
    __atomic_store_n(&g_tri_released, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&f->on_cpu, false, __ATOMIC_RELEASE);   // the last touch of f
    exits("ok");
}

static void tri_entry(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;
    struct Thread *f = thread_create_with_arg(p, tri_never_runs, NULL);
    if (!f) extinction("tri: thread_create_with_arg failed");
    proc_retire_for_test(p, f);
    __atomic_store_n(&f->on_cpu, true, __ATOMIC_RELEASE);   // "still switching away"

    int hpid = rfork(RFPROC, tri_helper_entry, f);
    if (hpid <= 0) extinction("tri: rfork helper failed");

    u64 d0 = thread_total_destroyed();
    proc_reap_retired(p);                          // must leave f: it is in flight
    if (thread_total_destroyed() != d0 || proc_retired_count_for_test(p) != 1u)
        __atomic_store_n(&g_tri_reap_took, 1u, __ATOMIC_RELEASE);

    __atomic_store_n(&g_tri_draining, 1u, __ATOMIC_RELEASE);
    proc_drain_retired(p);                         // must wait f out
    if (__atomic_load_n(&g_tri_released, __ATOMIC_ACQUIRE) == 0u)
        __atomic_store_n(&g_tri_drain_early, 1u, __ATOMIC_RELEASE);
    if (proc_retired_count_for_test(p) != 0u)
        __atomic_store_n(&g_tri_drain_left, 1u, __ATOMIC_RELEASE);

    int st = -1;
    (void)wait_pid(&st);                           // the helper
    exits("ok");
}

void test_proc_thread_reap_inflight(void) {
    __atomic_store_n(&g_tri_draining, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_tri_released, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_tri_reap_took, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_tri_drain_early, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_tri_drain_left, 0u, __ATOMIC_RELEASE);

    u64 created0   = thread_total_created();
    u64 destroyed0 = thread_total_destroyed();

    int pid = rfork(RFPROC, tri_entry, NULL);
    TEST_ASSERT(pid > 0, "rfork failed");
    int status = -1;
    TEST_EXPECT_EQ(wait_pid(&status), pid, "wait_pid reaps the test Proc");
    TEST_EXPECT_EQ(status, 0, "the test Proc exited cleanly");

    TEST_EXPECT_EQ(__atomic_load_n(&g_tri_reap_took, __ATOMIC_ACQUIRE), 0u,
        "the reap left a retired Thread whose switch away was in flight "
        "(thread_reap.tla NoFreeInFlight)");
    TEST_EXPECT_EQ(__atomic_load_n(&g_tri_drain_early, __ATOMIC_ACQUIRE), 0u,
        "the exec drain waited out the in-flight switch (task #19)");
    TEST_EXPECT_EQ(__atomic_load_n(&g_tri_drain_left, __ATOMIC_ACQUIRE), 0u,
        "the drain left no retired Thread behind");
    TEST_EXPECT_EQ(thread_total_created() - created0, 3ull,
        "the test Proc's thread, the stand-in and the helper created");
    TEST_EXPECT_EQ(thread_total_destroyed() - destroyed0, 3ull,
        "all three freed by the time wait_pid returns");
}

// ---------------------------------------------------------------------------
// proc.thread_reap_gauges
//
// A claimed Thread is still counted until the commit folds it: proc_cpu_ns and
// proc_kstack_peak never read less after a step of a reap than before it
// (audit F1). The test retires a settled Thread whose run time and stack depth
// it sets -- the deepest stack in the Proc -- and reads both gauges before the
// claim, between the claim and the commit, and after the commit.
//
// FAILS PRE-FIX: the old reap took the Thread off p->exited in the claiming
// hold and folded its stack depth in a later one, so the reading between the
// two lost it.
// ---------------------------------------------------------------------------

unsigned proc_reap_claim_for_test(struct Proc *p);
void     proc_reap_commit_for_test(struct Proc *p);

#define TRG_RUN_NS 1000000000ull   // a second: more than the test Proc runs

static volatile u32 g_trg_claimed;
static volatile u32 g_trg_freed;
static volatile u64 g_trg_cpu[3];
static volatile u32 g_trg_peak[3];

static void trg_never_runs(void *arg) {
    (void)arg;
    extinction("trg_never_runs: a never-readied Thread ran");
}

static void trg_read(struct Proc *p, unsigned at) {
    irq_state_t s = proc_table_lock_acquire();
    g_trg_cpu[at]  = proc_cpu_ns(p);
    g_trg_peak[at] = proc_kstack_peak(p, NULL, NULL);
    proc_table_lock_release(s);
}

static void trg_entry(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;
    struct Thread *f = thread_create_with_arg(p, trg_never_runs, NULL);
    if (!f) extinction("trg: thread_create_with_arg failed");
    // The deepest a stack can read: its first usable word written, which no
    // running thread reaches without faulting on the guard below it.
    *(volatile u64 *)((char *)f->kstack_base + THREAD_KSTACK_GUARD_SIZE) = 0;
    __atomic_store_n(&f->run_ns, TRG_RUN_NS, __ATOMIC_RELAXED);
    proc_retire_for_test(p, f);           // never ran, so on_cpu is clear: settled

    u64 d0 = thread_total_destroyed();
    trg_read(p, 0);
    __atomic_store_n(&g_trg_claimed, proc_reap_claim_for_test(p), __ATOMIC_RELEASE);
    trg_read(p, 1);
    proc_reap_commit_for_test(p);
    trg_read(p, 2);
    __atomic_store_n(&g_trg_freed, (u32)(thread_total_destroyed() - d0), __ATOMIC_RELEASE);
    exits("ok");
}

void test_proc_thread_reap_gauges(void) {
    __atomic_store_n(&g_trg_claimed, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_trg_freed, 0u, __ATOMIC_RELEASE);

    int pid = rfork(RFPROC, trg_entry, NULL);
    TEST_ASSERT(pid > 0, "rfork failed");
    int status = -1;
    TEST_EXPECT_EQ(wait_pid(&status), pid, "wait_pid reaps the test Proc");
    TEST_EXPECT_EQ(status, 0, "the test Proc exited cleanly");

    TEST_EXPECT_EQ(__atomic_load_n(&g_trg_claimed, __ATOMIC_ACQUIRE), 1u,
        "the claim took the one settled retired Thread");
    TEST_EXPECT_EQ(__atomic_load_n(&g_trg_freed, __ATOMIC_ACQUIRE), 1u,
        "the commit freed it");
    TEST_EXPECT_EQ(g_trg_peak[0], (u32)THREAD_KSTACK_SIZE,
        "the retired Thread holds the Proc's deepest stack before the reap");
    TEST_EXPECT_EQ(g_trg_peak[1], (u32)THREAD_KSTACK_SIZE,
        "proc_kstack_peak still counts a claimed Thread (audit F1)");
    TEST_EXPECT_EQ(g_trg_peak[2], (u32)THREAD_KSTACK_SIZE,
        "the commit folded its depth into the reaped peak");
    TEST_ASSERT(g_trg_cpu[0] >= TRG_RUN_NS,
        "the retired Thread's run time is in proc_cpu_ns before the reap");
    TEST_ASSERT(g_trg_cpu[1] >= g_trg_cpu[0],
        "proc_cpu_ns does not dip between the claim and the commit");
    TEST_ASSERT(g_trg_cpu[2] >= g_trg_cpu[1],
        "proc_cpu_ns does not dip across the commit");
}

// ---------------------------------------------------------------------------
// proc.thread_reap_concurrent
//
// Three peers of one Proc each spawn and retire TRX_PER workers at the same
// time, so reapers race each other and the exits they reap. Every worker runs
// once and every Thread is freed exactly once: the created and destroyed
// deltas match, and a second free of one Thread would trip
// thread_free_retired's magic check (thread_reap.tla OneFreerPerThread).
// ---------------------------------------------------------------------------

#define TRX_SPAWNERS 3u
#define TRX_PER      200u

static volatile u32 g_trx_ran[TRX_SPAWNERS];
static volatile u32 g_trx_refused;
static volatile u32 g_trx_done;

static void trx_worker_entry(void *arg) {
    u32 who = (u32)(uintptr_t)arg;
    __atomic_fetch_add(&g_trx_ran[who], 1u, __ATOMIC_RELEASE);
    thread_exit_self();
}

static void trx_spawner_entry(void *arg) {
    u32 who = (u32)(uintptr_t)arg;
    struct Proc *p = current_thread()->proc;
    for (u32 i = 0; i < TRX_PER; i++) {
        proc_reap_retired(p);
        if (!proc_thread_cap_ok(p)) {
            __atomic_fetch_add(&g_trx_refused, 1u, __ATOMIC_ACQ_REL);
            break;
        }
        struct Thread *w = thread_create_with_arg(p, trx_worker_entry,
                                                  (void *)(uintptr_t)who);
        if (!w) extinction("trx: thread_create_with_arg failed");
        ready(w);   // a peer may free w from here on: it is not touched again
        TEST_YIELD_UNTIL_PROC(__atomic_load_n(&g_trx_ran[who], __ATOMIC_ACQUIRE) == i + 1u);
    }
    __atomic_fetch_add(&g_trx_done, 1u, __ATOMIC_ACQ_REL);
    thread_exit_self();
}

static void trx_parent_entry(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;
    __atomic_store_n(&p->principal_id, REAP_TEST_USER, __ATOMIC_RELEASE);
    for (u32 k = 0; k < TRX_SPAWNERS; k++) {
        struct Thread *s = thread_create_with_arg(p, trx_spawner_entry,
                                                  (void *)(uintptr_t)k);
        if (!s) extinction("trx: spawner create failed");
        ready(s);
    }
    TEST_YIELD_UNTIL_PROC(__atomic_load_n(&g_trx_done, __ATOMIC_ACQUIRE) == TRX_SPAWNERS);
    exits("ok");
}

void test_proc_thread_reap_concurrent(void) {
    for (u32 k = 0; k < TRX_SPAWNERS; k++)
        __atomic_store_n(&g_trx_ran[k], 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_trx_refused, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_trx_done, 0u, __ATOMIC_RELEASE);

    u64 created0   = thread_total_created();
    u64 destroyed0 = thread_total_destroyed();

    int pid = rfork(RFPROC, trx_parent_entry, NULL);
    TEST_ASSERT(pid > 0, "rfork failed");
    int status = -1;
    TEST_EXPECT_EQ(wait_pid(&status), pid, "wait_pid reaps the racing Proc");
    TEST_EXPECT_EQ(status, 0, "the racing Proc exited cleanly");

    for (u32 k = 0; k < TRX_SPAWNERS; k++)
        TEST_EXPECT_EQ(__atomic_load_n(&g_trx_ran[k], __ATOMIC_ACQUIRE), TRX_PER,
            "every spawner's workers all ran");
    TEST_EXPECT_EQ(__atomic_load_n(&g_trx_refused, __ATOMIC_ACQUIRE), 0u,
        "no spawn refused under concurrent churn");
    u64 total = 1u + TRX_SPAWNERS + (u64)TRX_SPAWNERS * TRX_PER;
    TEST_EXPECT_EQ(thread_total_created() - created0, total,
        "main + spawners + every worker created");
    TEST_EXPECT_EQ(thread_total_destroyed() - destroyed0, total,
        "each freed exactly once by the time wait_pid returns");
}
