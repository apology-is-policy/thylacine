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
//   proc.{exits,fault}_in_dying_group_keeps_group_status
//                                          -- a last thread leaving a group
//                                            whose termination was already
//                                            claimed reaps with the claimed
//                                            status (I-24).
//   proc.reap_exited_threads_frees_peers  -- proc_reap_exited_threads frees
//                                            every exited, off-CPU peer while
//                                            the Proc lives, and the Proc keeps
//                                            their run time + kstack watermark;
//                                            a debug focus naming one clears.
//   proc.reap_keeps_thread_cap_open       -- 300 workers started and finished
//                                            one after another under a
//                                            non-exempt principal: the cap
//                                            never refuses (it did at 256).
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
#include <thylacine/handle.h>
#include <thylacine/loom.h>
#include <thylacine/notes.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

void test_thread_create_user_ctx_layout(void);
void test_thread_exit_self_marks_exiting(void);
void test_thread_exit_self_last_thread_zombies(void);
void test_proc_reap_exited_threads_frees_peers(void);
void test_proc_reap_keeps_thread_cap_open(void);
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

// ---------------------------------------------------------------------------
// proc.exits_in_dying_group_keeps_group_status
// proc.fault_in_dying_group_keeps_group_status
//
// I-24: a group's termination status is decided once; the first claim wins.
// Main claims it with code 7 (an exit_group(7)) and leaves as a peer does.
// The worker, now the last live Thread, then exits with another status: by
// exits(), or by proc_fault_terminate, the path a file page-in takes when the
// group's own death abandons its 9P read (snare:bus). The Proc must reap
// with 7, not with the late thread's status.
// ---------------------------------------------------------------------------

static struct Thread *volatile g_dgs_main;
static volatile bool           g_dgs_fault;

static void dgs_worker_entry(void *arg) {
    (void)arg;
    while (g_dgs_main->state != THREAD_EXITING)
        sched();
    if (g_dgs_fault)
        proc_fault_terminate(NOTE_NAME_SNARE_BUS, (uintptr_t)0xdead000ul);
    exits("snare:bus");
}

static void dgs_main_entry(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;
    g_dgs_main = current_thread();
    struct Thread *w = thread_create_with_arg(p, dgs_worker_entry, NULL);
    if (!w) extinction("dgs_main_entry: thread_create_with_arg failed");
    ready(w);
    irq_state_t s = proc_table_lock_acquire();
    proc_group_terminate_code(p, 7, "seven");
    proc_table_lock_release(s);
    thread_exit_self();
}

static void dgs_note_worker_entry(void *arg) {
    (void)arg;
    while (g_dgs_main->state != THREAD_EXITING)
        sched();
    exits("ok");
}

static int dgs_run_entry(void (*entry)(void *), bool fault, int flags) {
    g_dgs_main  = NULL;
    g_dgs_fault = fault;
    int child_pid = rfork(RFPROC, entry, NULL);
    if (child_pid <= 0) return -2;
    int status = -1;
    int reaped = wait_pid_for(child_pid, flags, &status);
    return reaped == child_pid ? status : -3;
}

static int dgs_run(bool fault) {
    return dgs_run_entry(dgs_main_entry, fault, 0);
}

void test_proc_exits_in_dying_group_keeps_group_status(void) {
    int status = dgs_run(false);
    TEST_EXPECT_NE(status, 1, "the late exits() status (1) replaced the group's");
    TEST_EXPECT_EQ(status, 7,
        "a last thread's exits() in a dying group reaps with the group's status");
}

void test_proc_fault_in_dying_group_keeps_group_status(void) {
    int status = dgs_run(true);
    TEST_EXPECT_NE(status, 1, "the late fault's snare:bus status (1) replaced the group's");
    TEST_EXPECT_EQ(status, 7,
        "a fault in a dying group reaps with the group's status, not snare:bus");
}

// gcpark-b: the claim decides the signal too. The packed word of the same run
// must read EXITED(7) (0x700); SIGNALED(SIGBUS) would read 7, the raw value
// the test above accepts, so only the packed wait can tell.
void test_proc_fault_in_dying_group_reaps_exited_packed(void) {
    int status = dgs_run_entry(dgs_main_entry, true, WAIT_PACKED);
    TEST_EXPECT_NE(status, WAIT_STATUS_SIGNALED(NOTE_SIG_BUS),
        "the late fault's SIGBUS replaced the group's exit");
    TEST_EXPECT_EQ(status, WAIT_STATUS_EXITED(7),
        "a fault in a dying group reaps packed as the group's exit (0x700)");
}

// Main dies of a fault while a worker still runs: the death claims the group
// with its signal, and the worker's later exits("ok") leaves as a peer does.
static void dgs_note_main_entry(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;
    g_dgs_main = current_thread();
    struct Thread *w = thread_create_with_arg(p, dgs_note_worker_entry, NULL);
    if (!w) extinction("dgs_note_main_entry: thread_create_with_arg failed");
    ready(w);
    exits_note(NOTE_NAME_SNARE_SEGV);
}

void test_proc_note_death_with_live_peer_reaps_signalled(void) {
    int packed = dgs_run_entry(dgs_note_main_entry, false, WAIT_PACKED);
    int raw    = dgs_run_entry(dgs_note_main_entry, false, 0);
    TEST_EXPECT_EQ(packed, WAIT_STATUS_SIGNALED(NOTE_SIG_SEGV),
        "a fault with a live peer reaps packed as SIGSEGV, not the peer's exit");
    TEST_EXPECT_EQ(raw, 1, "the same death reads 1 raw");
}

// ---------------------------------------------------------------------------
// proc.reap_exited_threads_frees_peers
// proc.reap_keeps_thread_cap_open
// proc.reap_before_sqpoll_charge
//
// An exited peer used to stay on p->threads until wait_pid reaped the whole
// Proc, so a program that started and joined workers hit PROC_THREAD_MAX with a
// handful of live threads (OpenRCT2 froze at 256; libxml2's runtest failed).
// The Loom SQPOLL charge shares that budget, so it frees them first too.
// The child Proc below is the only reaper of its own threads (wait_pid cannot
// act while it runs), so holding a worker pointer until the child's own
// proc_reap_exited_threads call is safe.
// ---------------------------------------------------------------------------

#define RET_N_WORKERS 4
#define RET_CHURN     300

static volatile u32 g_ret_spin;

static void ret_worker_entry(void *arg) {
    (void)arg;
    for (u32 i = 0; i < 20000u; i++)            // some run time to account for
        __atomic_fetch_add(&g_ret_spin, 1u, __ATOMIC_RELAXED);
    thread_exit_self();
}

static bool ret_dead(struct Thread *w) {
    return __atomic_load_n(&w->state, __ATOMIC_ACQUIRE) == THREAD_EXITING &&
           !__atomic_load_n(&w->on_cpu, __ATOMIC_ACQUIRE);
}

static volatile int  g_ret_count_before, g_ret_count_after;
static volatile u64  g_ret_run_sum, g_ret_reaped_run, g_ret_cpu_after;
static volatile u32  g_ret_used_max, g_ret_reaped_peak, g_ret_peak_after;
static volatile u64  g_ret_destroyed;
static volatile bool g_ret_alone, g_ret_focus_cleared, g_ret_focus_kept;

static void ret_frees_entry(void *arg) {
    (void)arg;
    struct Proc   *p    = current_thread()->proc;
    struct Thread *self = current_thread();
    struct Thread *w[RET_N_WORKERS];
    for (int i = 0; i < RET_N_WORKERS; i++) {
        w[i] = thread_create_with_arg(p, ret_worker_entry, NULL);
        if (!w[i]) extinction("ret_frees_entry: thread_create_with_arg failed");
        ready(w[i]);
    }
    for (int i = 0; i < RET_N_WORKERS; i++)
        TEST_YIELD_UNTIL_PROC(ret_dead(w[i]));

    irq_state_t s = proc_table_lock_acquire();
    g_ret_count_before = p->thread_count;
    u64 run = 0;
    u32 used_max = 0;
    for (int i = 0; i < RET_N_WORKERS; i++) {
        run += w[i]->run_ns;
        u32 used = thread_kstack_used(w[i], NULL);
        if (used > used_max) used_max = used;
    }
    __atomic_store_n(&p->debug_focus_thread, w[0], __ATOMIC_RELEASE);
    proc_table_lock_release(s);
    g_ret_run_sum  = run;
    g_ret_used_max = used_max;

    u64 destroyed_before = thread_total_destroyed();
    proc_reap_exited_threads();
    g_ret_destroyed = thread_total_destroyed() - destroyed_before;

    s = proc_table_lock_acquire();
    g_ret_count_after   = p->thread_count;
    g_ret_alone         = p->threads == self && !self->next_in_proc;
    g_ret_reaped_run    = p->reaped_run_ns;
    g_ret_reaped_peak   = p->reaped_kstack_peak;
    g_ret_cpu_after     = proc_cpu_ns(p);
    g_ret_peak_after    = proc_kstack_peak(p, NULL, NULL);
    g_ret_focus_cleared = __atomic_load_n(&p->debug_focus_thread, __ATOMIC_ACQUIRE) == NULL;
    __atomic_store_n(&p->debug_focus_thread, self, __ATOMIC_RELEASE);
    proc_table_lock_release(s);

    proc_reap_exited_threads();                 // nothing dead: a live focus stays
    s = proc_table_lock_acquire();
    g_ret_focus_kept = __atomic_load_n(&p->debug_focus_thread, __ATOMIC_ACQUIRE) == self;
    __atomic_store_n(&p->debug_focus_thread, NULL, __ATOMIC_RELEASE);
    proc_table_lock_release(s);
    exits("ok");
}

void test_proc_reap_exited_threads_frees_peers(void) {
    int child_pid = rfork(RFPROC, ret_frees_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    int status = -1;
    TEST_EXPECT_EQ(wait_pid(&status), child_pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0, "child exited cleanly");

    TEST_EXPECT_EQ(g_ret_count_before, RET_N_WORKERS + 1,
        "every exited worker is still counted before the reap");
    // The destroyed counter is system-wide, so a free elsewhere can only raise
    // it; the counts and the list below are this Proc's exact evidence.
    TEST_ASSERT(g_ret_destroyed >= (u64)RET_N_WORKERS,
        "the reap freed every exited worker while the Proc lives");
    TEST_EXPECT_EQ(g_ret_count_after, 1, "only the caller is left counted");
    TEST_ASSERT(g_ret_alone, "only the caller is left on p->threads");
    TEST_EXPECT_EQ(g_ret_reaped_run, g_ret_run_sum,
        "the Proc keeps the freed workers' run time");
    TEST_ASSERT(g_ret_cpu_after >= g_ret_run_sum,
        "proc_cpu_ns still includes the freed workers' run time");
    TEST_ASSERT(g_ret_used_max > 0u, "the workers' kstacks show use");
    TEST_EXPECT_EQ(g_ret_reaped_peak, g_ret_used_max,
        "the Proc keeps the deepest freed kstack");
    TEST_ASSERT(g_ret_peak_after >= g_ret_used_max,
        "proc_kstack_peak still reports the freed workers' depth");
    TEST_ASSERT(g_ret_focus_cleared, "a debug focus naming a freed thread clears");
    TEST_ASSERT(g_ret_focus_kept, "a debug focus naming a live thread stays");
}

static volatile int  g_rkc_refused_at, g_rkc_max_count, g_rkc_count_end;
static volatile bool g_rkc_exempt;

static void rkc_entry(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;
    // A user, so the cap applies; stamped the way a spawn does, in the child
    // before it has any peer.
    proc_apply_identity(p, 1000u, 1000u, NULL, 0);
    g_rkc_exempt     = proc_resource_exempt(p);
    g_rkc_refused_at = -1;
    g_rkc_max_count  = 0;
    for (int i = 0; i < RET_CHURN; i++) {
        proc_reap_exited_threads();
        if (!proc_thread_cap_ok(p)) { g_rkc_refused_at = i; break; }
        struct Thread *w = thread_create_with_arg(p, ret_worker_entry, NULL);
        if (!w) extinction("rkc_entry: thread_create_with_arg failed");
        irq_state_t s = proc_table_lock_acquire();
        if (p->thread_count > g_rkc_max_count) g_rkc_max_count = p->thread_count;
        proc_table_lock_release(s);
        ready(w);
        TEST_YIELD_UNTIL_PROC(ret_dead(w));
    }
    proc_reap_exited_threads();
    irq_state_t s = proc_table_lock_acquire();
    g_rkc_count_end = p->thread_count;
    proc_table_lock_release(s);
    exits("ok");
}

void test_proc_reap_keeps_thread_cap_open(void) {
    int child_pid = rfork(RFPROC, rkc_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    int status = -1;
    TEST_EXPECT_EQ(wait_pid(&status), child_pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0, "child exited cleanly");
    TEST_ASSERT(!g_rkc_exempt, "the child runs as a user, so the cap applies");
    TEST_EXPECT_EQ(g_rkc_refused_at, -1,
        "the cap never refused a thread across 300 started-and-finished workers");
    TEST_EXPECT_EQ(g_rkc_max_count, 2,
        "at most the caller and one worker were ever counted");
    TEST_EXPECT_EQ(g_rkc_count_end, 1, "the last worker was freed too");
}

static volatile int g_rsq_count_before, g_rsq_count_after, g_rsq_setup_rc;
static volatile int g_rsq_sqpoll_after, g_rsq_close_rc, g_rsq_sqpoll_closed;
static volatile bool g_rsq_exempt;

static bool rsq_peers_dead(struct Proc *p, struct Thread *self) {
    bool dead = true;
    irq_state_t s = proc_table_lock_acquire();
    for (struct Thread *t = p->threads; t; t = t->next_in_proc)
        if (t != self && !ret_dead(t)) { dead = false; break; }
    proc_table_lock_release(s);
    return dead;
}

static void rsq_entry(void *arg) {
    (void)arg;
    struct Proc   *p    = current_thread()->proc;
    struct Thread *self = current_thread();
    proc_apply_identity(p, 1000u, 1000u, NULL, 0);
    g_rsq_exempt   = proc_resource_exempt(p);
    g_rsq_setup_rc = g_rsq_close_rc = -1;
    // Created directly, not through a create arm, so nothing frees them
    // before the setup does.
    for (int i = 0; i < PROC_THREAD_MAX - 1; i++) {
        struct Thread *w = thread_create_with_arg(p, ret_worker_entry, NULL);
        if (!w) extinction("rsq_entry: thread_create_with_arg failed");
        ready(w);
    }
    TEST_YIELD_UNTIL_PROC(rsq_peers_dead(p, self));

    irq_state_t s = proc_table_lock_acquire();
    g_rsq_count_before = p->thread_count;
    proc_table_lock_release(s);
    struct loom_params kp;
    hidx_t fd = -1;
    g_rsq_setup_rc = sys_loom_setup_for_proc(p, 8, LOOM_SETUP_SQPOLL, &kp, &fd);
    s = proc_table_lock_acquire();
    g_rsq_count_after  = p->thread_count;
    g_rsq_sqpoll_after = p->loom_sqpoll_count;
    proc_table_lock_release(s);
    if (g_rsq_setup_rc == 0) {
        g_rsq_close_rc = handle_close(p, fd);
        s = proc_table_lock_acquire();
        g_rsq_sqpoll_closed = p->loom_sqpoll_count;
        proc_table_lock_release(s);
    }
    exits("ok");
}

void test_proc_reap_before_sqpoll_charge(void) {
    int child_pid = rfork(RFPROC, rsq_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    int status = -1;
    TEST_EXPECT_EQ(wait_pid(&status), child_pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0, "child exited cleanly");
    TEST_ASSERT(!g_rsq_exempt, "the child runs as a user, so the cap applies");
    TEST_EXPECT_EQ(g_rsq_count_before, PROC_THREAD_MAX,
        "255 finished workers and the caller fill the thread budget");
    TEST_EXPECT_EQ(g_rsq_setup_rc, 0,
        "an SQPOLL setup frees the exited workers first and fits");
    TEST_EXPECT_EQ(g_rsq_count_after, 1, "only the caller is left counted");
    TEST_EXPECT_EQ(g_rsq_sqpoll_after, 1, "the poller took its charge");
    TEST_EXPECT_EQ(g_rsq_close_rc, 0, "close the SQPOLL ring");
    TEST_EXPECT_EQ(g_rsq_sqpoll_closed, 0, "the charge settled on close");
}
