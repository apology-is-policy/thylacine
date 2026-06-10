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
//                                            main calls exits → parent
//                                            wait_pid drains ALL
//                                            Threads (the multi-thread
//                                            reap path in wait_pid).
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

    // Yield until every peer Thread reaches THREAD_EXITING. The state
    // field is monotonic (peers transition only via thread_exit_self,
    // which writes EXITING under g_proc_table_lock; a stale plain read
    // here can see RUNNABLE/RUNNING/EXITING but never go backward), so
    // a yield-and-recheck loop converges.
    for (;;) {
        bool all_exiting = true;
        for (struct Thread *peer = p->threads; peer; peer = peer->next_in_proc) {
            if (peer == current_thread()) continue;
            if (peer->state != THREAD_EXITING) { all_exiting = false; break; }
        }
        if (all_exiting) break;
        sched();
    }

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

    // 3 threads created in the child Proc (main + 2 peers); all 3
    // freed by wait_pid's reap loop.
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
// Larger reap — 5 peers; verifies wait_pid's multi-thread drain loop
// (walks p->threads, on_cpu-spin each, thread_free each, then proc_free).
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

    for (;;) {
        bool all_exiting = true;
        for (struct Thread *peer = p->threads; peer; peer = peer->next_in_proc) {
            if (peer == current_thread()) continue;
            if (peer->state != THREAD_EXITING) { all_exiting = false; break; }
        }
        if (all_exiting) break;
        sched();
    }

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
// proc.wait_pid_concurrent_waiter_refused  (RW-2 2B-F1/F2)
//
// At most ONE Thread of a Proc may be inside wait_pid_for at a time (p->child_-
// done is a single-waiter Rendez -> F1 extinction; wait_pid_cond's lockless
// p->children walk races a peer's reap+free -> F2 UAF). The per-Proc wait_active
// flag refuses a genuinely-CONCURRENT 2nd caller with -1 (a SOLE waiter, even in
// a multi-thread Proc, is correctly allowed -- this is why kproc's boot Thread
// keeps reaping).
//
// Two worker Threads of one Proc race into wait_pid_for with a live (spinning)
// CHILD present. Exactly one claims wait_active + sleeps on child_done; the
// other hits the guard -> -1. The child then exits, waking the claimer to reap
// it. Asserts: exactly ONE worker was refused, and the OTHER reaped the child.
// FAILS PRE-FIX: pre-fix BOTH workers sleep on child_done -> the 2nd trips the
// single-waiter assert -> kernel extinction (deterministic on -smp 1: the
// claimer sleeps, then the peer's sleep() hits r->waiter != NULL).
// ---------------------------------------------------------------------------

static volatile u32 g_wpcw_refused;     // count of guard-refused workers (rc==-1)
static volatile int g_wpcw_reaped_pid;  // SET only by the worker that reaped (its rc)
static volatile int g_wpcw_expect_pid;  // the child's pid (the expected reaped value)
static volatile u32 g_wpcw_done;        // workers finished

static void wpcw_child_entry(void *arg) {
    (void)arg;
    // Stay alive (a non-zombie child) until a worker has been guard-refused, so
    // the claiming worker is provably SLEEPING (holding wait_active) when the
    // other worker hits the guard. Then exit to wake the claimer to reap us.
    while (__atomic_load_n(&g_wpcw_refused, __ATOMIC_ACQUIRE) == 0u) sched();
    exits("ok");
}

static void wpcw_worker_entry(void *arg) {
    (void)arg;
    int status = -123;
    int rc = wait_pid_for(-1, 0, &status);
    if (rc == -1) {
        __atomic_fetch_add(&g_wpcw_refused, 1u, __ATOMIC_ACQ_REL);
    } else {
        __atomic_store_n(&g_wpcw_reaped_pid, rc, __ATOMIC_RELEASE);
    }
    __atomic_fetch_add(&g_wpcw_done, 1u, __ATOMIC_ACQ_REL);
    thread_exit_self();
}

static void wpcw_parent_entry(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;

    int child_pid = rfork(RFPROC, wpcw_child_entry, NULL);
    if (child_pid <= 0) extinction("wpcw: rfork child failed");
    __atomic_store_n(&g_wpcw_expect_pid, child_pid, __ATOMIC_RELEASE); // expected

    struct Thread *w0 = thread_create_with_arg(p, wpcw_worker_entry, NULL);
    struct Thread *w1 = thread_create_with_arg(p, wpcw_worker_entry, NULL);
    if (!w0 || !w1) extinction("wpcw: thread_create_with_arg failed");
    ready(w0);
    ready(w1);

    while (__atomic_load_n(&g_wpcw_done, __ATOMIC_ACQUIRE) < 2u) sched();
    exits("ok");
}

void test_proc_wait_pid_concurrent_waiter_refused(void) {
    __atomic_store_n(&g_wpcw_refused, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wpcw_done, 0u, __ATOMIC_RELEASE);
    // RW-2 R2-F2: reaped_pid is a SENTINEL (0) -- it is witnessed ONLY when the
    // claiming worker actually reaps the child (storing the child's pid). The
    // parent stores the expected child pid into expect_pid separately, so the
    // final equality is NON-tautological (a never-reaped child leaves 0 != pid).
    __atomic_store_n(&g_wpcw_reaped_pid, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wpcw_expect_pid, 0, __ATOMIC_RELEASE);

    int pid = rfork(RFPROC, wpcw_parent_entry, NULL);
    TEST_ASSERT(pid > 0, "rfork parent failed");

    int status = -1;
    int reaped = wait_pid(&status);   // boot reaps the multi-thread test Proc
    TEST_EXPECT_EQ(reaped, pid, "reaped the multi-thread test Proc");

    TEST_EXPECT_EQ(__atomic_load_n(&g_wpcw_refused, __ATOMIC_ACQUIRE), 1u,
        "exactly ONE of two concurrent same-Proc waiters is guard-refused "
        "(pre-fix: the 2nd trips the single-waiter assert -> extinction)");
    int got = __atomic_load_n(&g_wpcw_reaped_pid, __ATOMIC_ACQUIRE);
    int exp = __atomic_load_n(&g_wpcw_expect_pid, __ATOMIC_ACQUIRE);
    TEST_ASSERT(exp > 0, "child pid recorded");
    TEST_EXPECT_EQ(got, exp,
        "the OTHER (sole) waiter actually reaped the child (its pid) -- a "
        "multi-thread Proc with one waiter is correctly ALLOWED (kproc reaps)");
}
