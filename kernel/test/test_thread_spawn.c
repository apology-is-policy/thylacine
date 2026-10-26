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
//                                            Thread is freed and the
//                                            parent's wait_pid frees the
//                                            Proc.
//   proc.{exits,fault}_in_dying_group_keeps_group_status
//                                          -- a last thread leaving a group
//                                            whose termination was already
//                                            claimed reaps with the claimed
//                                            status (I-24).
//   proc.exited_threads_freed_at_switch   -- every exited peer is freed while
//                                            the Proc lives, with no spawn, exec
//                                            or wait, and the Proc keeps their
//                                            run time + kstack watermark; a
//                                            debug focus naming one clears.
//   proc.exited_threads_bounded_by_cpus   -- 64 workers exit at once: those
//                                            not yet freed never outnumber
//                                            the CPUs (I-48).
//   proc.reap_keeps_thread_cap_open       -- 300 workers started and finished
//                                            one after another under a
//                                            non-exempt principal: the cap
//                                            never refuses (it did at 256).
//   proc.sqpoll_charge_after_finished_workers
//                                          -- 255 finished workers leave the
//                                            budget open for an SQPOLL charge.
//   proc.exit_tail_runs_to_its_end        -- a preempt armed across tick
//                                            returns does not cut an exit
//                                            tail short; the tail keeps its
//                                            CPU to its own sched().
//   proc.exec_waits_for_exiting_peer      -- exec does not swap the address
//                                            space until its EXITING peer is
//                                            freed.
//   proc.exec_waits_for_exiting_peer_on_another_cpu
//                                          -- the same with a real peer holding
//                                            its exit tail on a second CPU.
//   proc.spawn_refused_in_dying_proc      -- a dying Proc refuses a new
//                                            thread -EAGAIN.
//   thread.tid_and_pid_spaces_refuse_when_spent
//                                          -- the last tid and pid are handed
//                                            out; past them a create is
//                                            refused and a spawn answers
//                                            EAGAIN, never an extinction.
//   proc.exec_claim_dooms_every_other_thread
//                                          -- a held exec claim reads as death
//                                            for every thread but the execer,
//                                            and both spawn arms and admission
//                                            refuse while it is held.
//   proc.exec_claim_kills_sleeping_and_running_peers
//                                          -- the claim kills peers asleep in
//                                            sleep() and sleep_death_only and
//                                            one at its die-check, then exec
//                                            swaps with one thread left.
//   proc.exec_claim_abandoned_on_group_death
//                                          -- a kill ends a waiting claim and
//                                            clears it.
//   proc.exec_claim_woken_by_an_abandoned_create
//                                          -- freeing a never-run thread wakes
//                                            the claim.
//   proc.exec_claim_does_not_hold_for_a_stop
//                                          -- under a job stop the claim still
//                                            returns; the stop stands for the
//                                            new image.
//   proc.exec_claim_survives_a_doomed_exit
//                                          -- a doomed thread's own exits() or
//                                            trap death leaves alone; the exec
//                                            completes, its /srv service still
//                                            posted until the real last exit.
//   proc.exec_claim_holds_through_a_terminate_latch
//                                          -- an interrupt that arms the latch
//                                            mid-wait does not end the claim;
//                                            the latch stands for the new image.
//   proc.exec_claim_refused_drops_the_new_image
//                                          -- a refused claim drops exec's own
//                                            references to the new space and
//                                            the program file, and only then.
//   proc.exec_claim_kills_a_thread_readied_after_it
//                                          -- a user thread admitted before the
//                                            claim and readied after it dies
//                                            before EL0 and frees the exec.
//   proc.exec_claim_wakes_a_vfork_parent
//                                          -- a vfork parent's park reads the
//                                            claim; the child keeps the old
//                                            image's space after the swap.
//
// The kernel test harness runs at EL1 so we can't drive a full eret-to-
// EL0 from here; the user-mode SVC dispatch + the userland_enter eret
// dance are exercised by the joey-side smoke + the /thread-probe binary
// (sub-chunk 9c, end-to-end). Here we focus on the kernel-internal
// state machine.

#include "test.h"

#include "../../arch/arm64/mmu.h"   // mmu_kernel_ttbr0_pa (RW-1 B-F1 ctx.ttbr0)
#include "../../arch/arm64/exception.h"   // struct exception_context for the clone arm
#include "../../arch/arm64/gic.h"
#include "../../arch/arm64/timer.h"
#include "../../arch/arm64/uart.h"

#include <thylacine/addrspace.h>
#include <thylacine/context.h>
#include <thylacine/devsrv.h>
#include <thylacine/errno.h>
#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/loom.h>
#include <thylacine/notes.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/smp.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

void test_thread_create_user_ctx_layout(void);
void test_thread_exit_self_marks_exiting(void);
void test_thread_exit_self_last_thread_zombies(void);
void test_proc_exited_threads_freed_at_switch(void);
void test_proc_exited_threads_bounded_by_cpus(void);
void test_proc_reap_keeps_thread_cap_open(void);
void test_proc_sqpoll_charge_after_finished_workers(void);
void test_proc_cap_counts_live_threads(void);
void test_proc_threads_lock_slot(void);
void test_proc_spawn_arms_decide_at_the_admit(void);
void test_proc_multi_thread_reap(void);
void test_proc_exit_tail_runs_to_its_end(void);
void test_proc_exec_waits_for_exiting_peer(void);
void test_proc_exec_waits_for_exiting_peer_on_another_cpu(void);
void test_proc_spawn_refused_in_dying_proc(void);
void test_thread_tid_and_pid_spaces_refuse_when_spent(void);
void test_proc_exec_claim_dooms_every_other_thread(void);
void test_proc_exec_claim_kills_sleeping_and_running_peers(void);
void test_proc_exec_claim_abandoned_on_group_death(void);
void test_proc_exec_claim_woken_by_an_abandoned_create(void);
void test_proc_exec_claim_does_not_hold_for_a_stop(void);
void test_proc_exec_claim_survives_a_doomed_exit(void);
void test_proc_exec_claim_holds_through_a_terminate_latch(void);
void test_proc_exec_claim_refused_drops_the_new_image(void);
void test_proc_exec_claim_kills_a_thread_readied_after_it(void);
void test_proc_exec_claim_wakes_a_vfork_parent(void);

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
// then calls exits("ok"). Each thread is freed by the resume path of its
// last switch-out; the parent's wait_pid frees the Proc.
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

    // Yield until every peer still on the list is EXITING. A peer leaves
    // the list when it is freed, so the walk holds p->threads_lock (ARCH
    // 7.3.1); EXITING is written under it and never undone, so a
    // yield-and-recheck loop converges.
    for (;;) {
        bool all_exiting = true;
        irq_state_t s = proc_threads_lock(p);
        for (struct Thread *peer = p->threads; peer; peer = peer->next_in_proc) {
            if (peer == current_thread()) continue;
            if (peer->state != THREAD_EXITING) { all_exiting = false; break; }
        }
        proc_threads_unlock(p, s);
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
    // freed by the resume paths of their last switch-outs.
    u64 t_created_delta   = thread_total_created()   - t_created_before;
    u64 t_destroyed_delta = thread_total_destroyed() - t_destroyed_before;
    TEST_EXPECT_EQ(t_created_delta, 3u,
                   "3 Threads created in the child Proc");
    TEST_EXPECT_EQ(t_destroyed_delta, 3u,
                   "all 3 Threads were freed");
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
// Larger reap — 5 peers; every thread is freed by the resume path of its
// last switch-out, and wait_pid frees the Proc once none is left.
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
        irq_state_t s = proc_threads_lock(p);
        for (struct Thread *peer = p->threads; peer; peer = peer->next_in_proc) {
            if (peer == current_thread()) continue;
            if (peer->state != THREAD_EXITING) { all_exiting = false; break; }
        }
        proc_threads_unlock(p, s);
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
                   "all N+1 Threads were freed");
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

static volatile int  g_dgs_main_tid;
static volatile bool g_dgs_fault;

// Main is freed at its last switch-out (ARCH 7.3.2), so the worker names it
// by the tid it stored before readying the worker.
static bool dgs_main_exited(void) {
    struct test_thread_view v = test_thread_view(current_thread()->proc,
                                                 g_dgs_main_tid);
    return !v.present || v.state == THREAD_EXITING;
}

static void dgs_worker_entry(void *arg) {
    (void)arg;
    while (!dgs_main_exited())
        sched();
    if (g_dgs_fault)
        proc_fault_terminate(NOTE_NAME_SNARE_BUS, (uintptr_t)0xdead000ul);
    exits("snare:bus");
}

static void dgs_main_entry(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;
    g_dgs_main_tid = current_thread()->tid;
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
    while (!dgs_main_exited())
        sched();
    exits("ok");
}

static int dgs_run_entry(void (*entry)(void *), bool fault, int flags) {
    g_dgs_main_tid = 0;
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
    g_dgs_main_tid = current_thread()->tid;
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
// proc.exited_threads_freed_at_switch
// proc.exited_threads_bounded_by_cpus
// proc.reap_keeps_thread_cap_open
// proc.sqpoll_charge_after_finished_workers
//
// An exited thread is freed by the resume path of its last switch-out (ARCH
// 7.3.2, I-48). Nothing in these children spawns, execs or waits to free one:
// a program that starts and joins workers must never reach PROC_THREAD_MAX with
// a handful of live threads (OpenRCT2 froze at 256; libxml2's runtest failed).
// A worker is named by its tid, read before it is readied, and never touched
// once it may have exited.
// ---------------------------------------------------------------------------

#define RET_N_WORKERS 4
#define RET_CHURN     300
#define REB_N_WORKERS 64

static volatile u32  g_ret_spin;
static volatile bool g_ret_go;

static void ret_spin(void) {
    for (u32 i = 0; i < 20000u; i++)            // some run time to account for
        __atomic_fetch_add(&g_ret_spin, 1u, __ATOMIC_RELAXED);
}

static void ret_worker_entry(void *arg) {
    (void)arg;
    ret_spin();
    thread_exit_self();
}

static void ret_wait_go(void) {
    while (!__atomic_load_n(&g_ret_go, __ATOMIC_ACQUIRE))
        sched();
}

// Held until the test releases it, so it can be named while it is alive.
static void ret_gated_entry(void *arg) {
    ret_wait_go();
    ret_worker_entry(arg);
}

static volatile u64 g_ret_worker_run[RET_N_WORKERS];
static volatile u32 g_ret_worker_kstack[RET_N_WORKERS];

// Gated, and records what its reap must fold at least: its run time up to
// here (the slice in progress counts; masked, so no switch-out lands between
// the two reads) and its stack depth so far (the poison watermark only
// deepens).
static void ret_measured_entry(void *arg) {
    int i = (int)(uintptr_t)arg;
    ret_wait_go();
    ret_spin();
    const struct Thread *self = current_thread();
    irq_state_t s = spin_lock_irqsave(NULL);
    g_ret_worker_run[i] = self->run_ns + (timer_now_ns() - self->switched_in_at);
    spin_unlock_irqrestore(NULL, s);
    g_ret_worker_kstack[i] = thread_kstack_used(self, NULL);
    thread_exit_self();
}

static bool ret_holds_any(struct Proc *p, const int *tid, int n) {
    for (int i = 0; i < n; i++)
        if (test_proc_holds_tid(p, tid[i])) return true;
    return false;
}

static volatile int  g_ret_count_after, g_ret_live_after;
static volatile u64  g_ret_reaped_run, g_ret_cpu_after;
static volatile u32  g_ret_reaped_peak, g_ret_peak_after;
static volatile u64  g_ret_destroyed;
static volatile bool g_ret_alone, g_ret_peak_tid_ok, g_ret_focus_cleared, g_ret_focus_kept;

static void ret_frees_entry(void *arg) {
    (void)arg;
    struct Proc   *p    = current_thread()->proc;
    struct Thread *self = current_thread();
    int tid[RET_N_WORKERS];
    irq_state_t s;
    __atomic_store_n(&g_ret_go, false, __ATOMIC_RELEASE);
    for (int i = 0; i < RET_N_WORKERS; i++) {
        g_ret_worker_run[i]    = 0;
        g_ret_worker_kstack[i] = 0;
        struct Thread *w = thread_create_with_arg(p, ret_measured_entry,
                                                  (void *)(uintptr_t)i);
        if (!w) extinction("ret_frees_entry: thread_create_with_arg failed");
        tid[i] = w->tid;
        if (i == 0) {
            s = proc_threads_lock(p);
            __atomic_store_n(&p->debug_focus_thread, w, __ATOMIC_RELEASE);
            proc_threads_unlock(p, s);
        }
        ready(w);
    }
    u64 destroyed_before = thread_total_destroyed();
    __atomic_store_n(&g_ret_go, true, __ATOMIC_RELEASE);
    TEST_YIELD_UNTIL_PROC(!ret_holds_any(p, tid, RET_N_WORKERS));
    // A reaper unlinks under the lock and counts the free after it, so a
    // worker leaves the list a moment before it is counted.
    TEST_YIELD_UNTIL_PROC(thread_total_destroyed() - destroyed_before >= (u64)RET_N_WORKERS);
    g_ret_destroyed = thread_total_destroyed() - destroyed_before;

    g_ret_cpu_after     = proc_cpu_ns(p);              // each takes p->threads_lock
    g_ret_peak_after    = proc_kstack_peak(p, NULL, NULL);
    s = proc_threads_lock(p);
    g_ret_count_after   = p->thread_count;
    g_ret_live_after    = p->live_threads;
    g_ret_alone         = p->threads == self && !self->next_in_proc;
    g_ret_reaped_run    = p->reaped_run_ns;
    g_ret_reaped_peak   = p->reaped_kstack_peak;
    bool tid_ok = false;
    for (int i = 0; i < RET_N_WORKERS; i++)
        if (p->reaped_kstack_tid == tid[i]) tid_ok = true;
    g_ret_peak_tid_ok   = tid_ok;
    g_ret_focus_cleared = __atomic_load_n(&p->debug_focus_thread, __ATOMIC_ACQUIRE) == NULL;
    __atomic_store_n(&p->debug_focus_thread, self, __ATOMIC_RELEASE);
    proc_threads_unlock(p, s);

    // A focus naming a live thread survives another thread's free.
    struct Thread *w = thread_create_with_arg(p, ret_worker_entry, NULL);
    if (!w) extinction("ret_frees_entry: thread_create_with_arg failed");
    int wtid = w->tid;
    ready(w);
    TEST_YIELD_UNTIL_PROC(!test_proc_holds_tid(p, wtid));
    s = proc_threads_lock(p);
    g_ret_focus_kept = __atomic_load_n(&p->debug_focus_thread, __ATOMIC_ACQUIRE) == self;
    __atomic_store_n(&p->debug_focus_thread, NULL, __ATOMIC_RELEASE);
    proc_threads_unlock(p, s);
    exits("ok");
}

void test_proc_exited_threads_freed_at_switch(void) {
    int child_pid = rfork(RFPROC, ret_frees_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    int status = -1;
    TEST_EXPECT_EQ(wait_pid(&status), child_pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0, "child exited cleanly");

    // The destroyed counter is system-wide, so a free elsewhere can only raise
    // it; the counts and the list below are this Proc's exact evidence.
    TEST_ASSERT(g_ret_destroyed >= (u64)RET_N_WORKERS,
        "every exited worker was freed with no spawn, exec or wait");
    TEST_EXPECT_EQ(g_ret_count_after, 1, "only the caller is left counted");
    TEST_EXPECT_EQ(g_ret_live_after, 1, "and it is the one live thread");
    TEST_ASSERT(g_ret_alone, "only the caller is left on p->threads");
    u64 run_sum  = 0;
    u32 deepest  = 0;
    for (int i = 0; i < RET_N_WORKERS; i++) {
        run_sum += g_ret_worker_run[i];
        if (g_ret_worker_kstack[i] > deepest) deepest = g_ret_worker_kstack[i];
    }
    TEST_ASSERT(run_sum > 0u && deepest > 0u, "every worker measured itself");
    TEST_ASSERT(g_ret_reaped_run >= run_sum,
        "the Proc keeps at least the run time the freed workers measured");
    TEST_ASSERT(g_ret_cpu_after >= g_ret_reaped_run,
        "proc_cpu_ns still includes the freed workers' run time");
    TEST_ASSERT(g_ret_reaped_peak >= deepest,
        "the Proc keeps at least the kstack depth the freed workers reached");
    TEST_ASSERT(g_ret_peak_tid_ok, "and names the freed worker that reached it");
    TEST_ASSERT(g_ret_peak_after >= g_ret_reaped_peak,
        "proc_kstack_peak still reports the freed workers' depth");
    TEST_ASSERT(g_ret_focus_cleared, "a debug focus naming a freed thread clears");
    TEST_ASSERT(g_ret_focus_kept, "a debug focus naming a live thread stays");
}

static volatile int  g_reb_max_gap, g_reb_count_end;
static volatile u64  g_reb_destroyed;

// Every worker released at once: at each sample, the exited threads not yet
// freed number at most the CPUs (the commit's KERNEL_TESTS check extincts past
// that bound; this watches it from outside).
static void reb_entry(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;
    int tid[REB_N_WORKERS];
    __atomic_store_n(&g_ret_go, false, __ATOMIC_RELEASE);
    for (int i = 0; i < REB_N_WORKERS; i++) {
        struct Thread *w = thread_create_with_arg(p, ret_gated_entry, NULL);
        if (!w) extinction("reb_entry: thread_create_with_arg failed");
        tid[i] = w->tid;
        ready(w);
    }
    u64 destroyed_before = thread_total_destroyed();
    int max_gap = 0;
    __atomic_store_n(&g_ret_go, true, __ATOMIC_RELEASE);
    u64 deadline = timer_now_ns() + TEST_YIELD_BUDGET_NS;
    while (ret_holds_any(p, tid, REB_N_WORKERS) && timer_now_ns() < deadline) {
        irq_state_t s = proc_threads_lock(p);
        int gap = p->thread_count - p->live_threads;
        proc_threads_unlock(p, s);
        if (gap > max_gap) max_gap = gap;
        sched();
    }
    g_reb_max_gap   = max_gap;
    // Counted after the reaper's unlock (see ret_frees_entry).
    deadline = timer_now_ns() + TEST_YIELD_BUDGET_NS;
    while (thread_total_destroyed() - destroyed_before < (u64)REB_N_WORKERS &&
           timer_now_ns() < deadline)
        sched();
    g_reb_destroyed = thread_total_destroyed() - destroyed_before;
    irq_state_t s = proc_threads_lock(p);
    g_reb_count_end = p->thread_count;
    proc_threads_unlock(p, s);
    exits("ok");
}

void test_proc_exited_threads_bounded_by_cpus(void) {
    g_reb_max_gap = -1;
    int child_pid = rfork(RFPROC, reb_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    int status = -1;
    TEST_EXPECT_EQ(wait_pid(&status), child_pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0, "child exited cleanly");
    TEST_EXPECT_EQ(g_reb_count_end, 1, "every released worker was freed");
    TEST_ASSERT(g_reb_destroyed >= (u64)REB_N_WORKERS, "the frees are this burst's");
    TEST_ASSERT(g_reb_max_gap >= 0 && g_reb_max_gap <= (int)smp_cpu_online_count(),
        "exited threads not yet freed never outnumbered the CPUs");
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
        if (!proc_thread_cap_ok(p)) { g_rkc_refused_at = i; break; }
        struct Thread *w = thread_create_with_arg(p, ret_worker_entry, NULL);
        if (!w) extinction("rkc_entry: thread_create_with_arg failed");
        int wtid = w->tid;
        irq_state_t s = proc_threads_lock(p);
        if (p->thread_count > g_rkc_max_count) g_rkc_max_count = p->thread_count;
        proc_threads_unlock(p, s);
        ready(w);
        TEST_YIELD_UNTIL_PROC(!test_proc_holds_tid(p, wtid));
    }
    irq_state_t s = proc_threads_lock(p);
    g_rkc_count_end = p->thread_count;
    proc_threads_unlock(p, s);
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

// The one-thread-lock-per-CPU rule (ARCH 7.3.1) is a per-CPU slot: a second
// acquire, a release by a non-holder and a _locked walker without its lock each
// extinct on what the slot says, which a suite cannot watch. This pins the slot
// they read: empty, then naming exactly the Proc whose lock is held, then empty.
void test_proc_threads_lock_slot(void) {
    struct Proc *p = current_thread()->proc;
    struct Proc *q = proc_alloc();
    TEST_ASSERT(q != NULL, "proc_alloc");
    const struct Proc *before = proc_threads_held_here_for_test();
    irq_state_t s = proc_threads_lock(p);
    const struct Proc *held_p = proc_threads_held_here_for_test();
    proc_threads_unlock(p, s);
    const struct Proc *between = proc_threads_held_here_for_test();
    s = proc_threads_lock(q);
    const struct Proc *held_q = proc_threads_held_here_for_test();
    proc_threads_unlock(q, s);
    const struct Proc *after = proc_threads_held_here_for_test();
    q->state = PROC_STATE_ZOMBIE;
    proc_free(q);

    TEST_ASSERT(before == NULL, "no thread lock held at entry");
    TEST_ASSERT(held_p == p, "the slot names the Proc whose lock is held");
    TEST_ASSERT(between == NULL, "the release empties the slot");
    TEST_ASSERT(held_q == q, "and names the next Proc, not the last");
    TEST_ASSERT(after == NULL, "empty again at exit");
}

static volatile int g_rsq_count_before, g_rsq_count_after, g_rsq_setup_rc;
static volatile int g_rsq_sqpoll_after, g_rsq_close_rc, g_rsq_sqpoll_closed;
static volatile bool g_rsq_exempt;

static bool rsq_alone(struct Proc *p) {
    irq_state_t s = proc_threads_lock(p);
    bool alone = p->thread_count == 1;
    proc_threads_unlock(p, s);
    return alone;
}

static void rsq_entry(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;
    proc_apply_identity(p, 1000u, 1000u, NULL, 0);
    g_rsq_exempt   = proc_resource_exempt(p);
    g_rsq_setup_rc = g_rsq_close_rc = -1;
    // Created directly, not through a create arm, so the cap does not stop the
    // budget's worth of workers.
    for (int i = 0; i < PROC_THREAD_MAX - 1; i++) {
        struct Thread *w = thread_create_with_arg(p, ret_worker_entry, NULL);
        if (!w) extinction("rsq_entry: thread_create_with_arg failed");
        ready(w);
    }
    TEST_YIELD_UNTIL_PROC(rsq_alone(p));

    irq_state_t s = proc_threads_lock(p);
    g_rsq_count_before = p->thread_count;
    proc_threads_unlock(p, s);
    struct loom_params kp;
    hidx_t fd = -1;
    g_rsq_setup_rc = sys_loom_setup_for_proc(p, 8, LOOM_SETUP_SQPOLL, &kp, &fd);
    s = proc_threads_lock(p);
    g_rsq_count_after  = p->thread_count;
    g_rsq_sqpoll_after = p->loom_sqpoll_count;
    proc_threads_unlock(p, s);
    if (g_rsq_setup_rc == 0) {
        g_rsq_close_rc = handle_close(p, fd);
        s = proc_threads_lock(p);
        g_rsq_sqpoll_closed = p->loom_sqpoll_count;
        proc_threads_unlock(p, s);
    }
    exits("ok");
}

void test_proc_sqpoll_charge_after_finished_workers(void) {
    int child_pid = rfork(RFPROC, rsq_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    int status = -1;
    TEST_EXPECT_EQ(wait_pid(&status), child_pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0, "child exited cleanly");
    TEST_ASSERT(!g_rsq_exempt, "the child runs as a user, so the cap applies");
    TEST_EXPECT_EQ(g_rsq_count_before, 1,
        "255 finished workers were freed as they finished: the budget is open");
    TEST_EXPECT_EQ(g_rsq_setup_rc, 0, "an SQPOLL setup fits");
    TEST_EXPECT_EQ(g_rsq_count_after, 1, "only the caller is counted");
    TEST_EXPECT_EQ(g_rsq_sqpoll_after, 1, "the poller took its charge");
    TEST_EXPECT_EQ(g_rsq_close_rc, 0, "close the SQPOLL ring");
    TEST_EXPECT_EQ(g_rsq_sqpoll_closed, 0, "the charge settled on close");
}

// ---------------------------------------------------------------------------
// The exit tail, exec against an exiting peer, a spawn in a dying Proc, and
// the tid and pid spaces at their edge.
// ---------------------------------------------------------------------------

// Test hooks (the *_for_test convention; absent from the headers).
u32  thread_tid_counter_swap_for_test(u32 v);
u32  proc_pid_counter_swap_for_test(u32 v);
void proc_exit_tail_test_set_hook(void (*fn)(void));
u64  proc_exec_peer_waits_for_test(void);
s64  sys_thread_spawn_for_test(u64 entry_va, u64 sp_va, u64 arg_va, u64 tls_va, u64 ptid_va);

// A user entry and stack the spawn gates accept; nothing maps them, and no
// thread spawned here reaches EL0 in a passing run.
#define C0_USER_ENTRY 0x400000u
#define C0_USER_SP    0x7fff0000u

// The entry of a thread that is created and never readied.
static void c0_never_entry(void *arg) {
    (void)arg;
    thread_exit_self();
}

static volatile bool g_ett_irqs_on, g_ett_deferred, g_ett_done;

// On the exiting thread, after its EXITING commit: a preempt armed on this CPU
// across three tick returns with interrupts on. Each return passes every
// other gate preempt_check_irq has (no lock, no syscall), so only the state
// gate keeps the tail running.
static void ett_tail_hook(void) {
    proc_exit_tail_test_set_hook(NULL);
    g_ett_irqs_on = !irqs_masked();
    if (!g_ett_irqs_on) return;              // ticks would never advance
    unsigned self = smp_cpu_idx_self();
    sched_set_need_resched_for_test(self);
    u64 t0 = timer_get_ticks();
    while (timer_get_ticks() < t0 + 3)
        __asm__ __volatile__("" ::: "memory");
    g_ett_deferred = sched_need_resched_pending(self);
    g_ett_done     = true;
}

static void ett_entry(void *arg) {
    (void)arg;
    proc_exit_tail_test_set_hook(ett_tail_hook);
    thread_exit_self();
}

void test_proc_exit_tail_runs_to_its_end(void) {
    g_ett_irqs_on = g_ett_deferred = g_ett_done = false;
    int child_pid = rfork(RFPROC, ett_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    int status = -1;
    int reaped = wait_pid(&status);
    proc_exit_tail_test_set_hook(NULL);
    TEST_EXPECT_EQ(reaped, child_pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0, "child exited cleanly");
    TEST_ASSERT(g_ett_irqs_on, "the tail ran with interrupts on (the exposed shape)");
    TEST_ASSERT(g_ett_done, "a preempt at a tick return did not end the exit tail");
    TEST_ASSERT(g_ett_deferred, "the preempt stayed pending for the tail's own sched()");
}

static struct Proc      *volatile g_ewp_proc;
static struct Thread    *volatile g_ewp_peer;
static struct AddrSpace *volatile g_ewp_old;
static volatile bool g_ewp_ready, g_ewp_setup_failed, g_ewp_swapped, g_ewp_abandoned;
static volatile int  g_ewp_count_after;

// The exec'ing thread. Its peer is FABRICATED: created, never readied, and
// committed EXITING with on_cpu raised under the lock -- the state a real
// peer holds between its exit commit and its last switch-out. The test thread
// stands in for that peer's CPU: once it has seen exec wait, it lowers on_cpu
// and frees the peer as the resume path would (thread_reap_dead, IRQs masked),
// so this runs at any CPU count; the next test holds a real peer on a second
// CPU. The peer is raised only if the test thread has not given up on the
// child, decided under the lock both read, so a late child leaves no peer for
// exec to wait on.
static void ewp_entry(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;
    struct Thread *peer = thread_create_with_arg(p, c0_never_entry, NULL);
    struct AddrSpace *nas = peer ? addrspace_alloc(p->page_budget) : NULL;
    if (!nas) {
        if (peer) thread_free(peer);
        __atomic_store_n(&g_ewp_setup_failed, true, __ATOMIC_RELEASE);
        exits("setup");
    }
    irq_state_t s = proc_table_lock_acquire();
    bool abandoned = g_ewp_abandoned;
    if (!abandoned) {
        irq_state_t ts = proc_threads_lock(p);
        thread_commit_exiting_locked(peer);
        proc_threads_unlock(p, ts);
        __atomic_store_n(&peer->on_cpu, true, __ATOMIC_RELEASE);
        g_ewp_old  = p->as;
        g_ewp_peer = peer;
        g_ewp_proc = p;
        __atomic_store_n(&g_ewp_ready, true, __ATOMIC_RELEASE);
    }
    proc_table_lock_release(s);
    if (abandoned) {
        thread_free(peer);
        addrspace_unref(nas);
        exits("abandoned");
    }

    proc_exec_replace(p, nas, p->phenotype);
    g_ewp_swapped = true;
    s = proc_threads_lock(p);
    g_ewp_count_after = p->thread_count;
    proc_threads_unlock(p, s);
    exits("ok");
}

static bool ewp_as_swapped(struct Proc *p) {
    irq_state_t s = proc_table_lock_acquire();
    bool swapped = p->as != g_ewp_old;
    proc_table_lock_release(s);
    return swapped;
}

void test_proc_exec_waits_for_exiting_peer(void) {
    g_ewp_proc = NULL;
    g_ewp_peer = NULL;
    g_ewp_old  = NULL;
    g_ewp_ready = g_ewp_setup_failed = g_ewp_swapped = g_ewp_abandoned = false;
    g_ewp_count_after = -1;
    u64 waits0 = proc_exec_peer_waits_for_test();

    int child_pid = rfork(RFPROC, ewp_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    // Soft: past the budget the child is abandoned, not left behind (above).
    TEST_YIELD_UNTIL_SOFT(__atomic_load_n(&g_ewp_ready, __ATOMIC_ACQUIRE) ||
                          __atomic_load_n(&g_ewp_setup_failed, __ATOMIC_ACQUIRE));
    irq_state_t s = proc_table_lock_acquire();
    bool abandoned = !g_ewp_ready && !g_ewp_setup_failed;
    g_ewp_abandoned = abandoned;
    proc_table_lock_release(s);

    bool waited = false, swapped_early = true;
    if (g_ewp_ready) {
        struct Proc *cp = g_ewp_proc;
        u64 deadline = timer_now_ns() + TEST_YIELD_BUDGET_NS;
        while (proc_exec_peer_waits_for_test() == waits0 && !ewp_as_swapped(cp) &&
               timer_now_ns() < deadline)
            sched();
        waited = proc_exec_peer_waits_for_test() != waits0;
        // The peer stays on its CPU a while longer; exec must keep waiting.
        u64 hold = timer_now_ns() + 20000000ull;
        while (timer_now_ns() < hold)
            sched();
        swapped_early = ewp_as_swapped(cp);
        // Freed on every path: exec and wait_pid both wait for it.
        irq_state_t is = spin_lock_irqsave(NULL);
        __atomic_store_n(&g_ewp_peer->on_cpu, false, __ATOMIC_RELEASE);
        thread_reap_dead(g_ewp_peer);
        spin_unlock_irqrestore(NULL, is);
    }
    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, child_pid, "wait_pid pid mismatch");
    TEST_ASSERT(!abandoned, "the child reached exec within the budget");
    TEST_ASSERT(!g_ewp_setup_failed, "setup: a peer and a new address space");
    TEST_EXPECT_EQ(status, 0, "child exited cleanly");
    TEST_ASSERT(!swapped_early, "exec kept the old address space while the peer was unfreed");
    // The control for the line above: exec was waiting, not merely slow to swap.
    TEST_ASSERT(waited, "exec found the peer not yet freed and waited");
    TEST_ASSERT(g_ewp_swapped, "exec finished once the peer was freed");
    TEST_EXPECT_EQ(g_ewp_count_after, 1, "the new image starts alone");
}

// ---------------------------------------------------------------------------
// proc.cap_counts_live_threads
//
// The cap charges live threads (ARCH 7.3.2, vote 7). A thread stops counting
// at its EXITING commit -- its joiner is woken right after -- although the
// resume path of its last switch-out has yet to free it. The exited peer is
// fabricated in that window (created, never readied, committed with on_cpu
// raised), so this runs at any CPU count.
// ---------------------------------------------------------------------------

static struct Thread *g_ccl_held[PROC_THREAD_MAX];
static volatile bool  g_ccl_exempt, g_ccl_setup_failed, g_ccl_full_refused;
static volatile bool  g_ccl_room, g_ccl_charge, g_ccl_admit;
static volatile int   g_ccl_count, g_ccl_live;

static void ccl_entry(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;
    proc_apply_identity(p, 1000u, 1000u, NULL, 0);   // a user, so the cap applies
    g_ccl_exempt = proc_resource_exempt(p);
    int n = 0;
    while (n < PROC_THREAD_MAX - 1 &&
           (g_ccl_held[n] = thread_create_with_arg(p, c0_never_entry, NULL)) != NULL)
        n++;
    if (n != PROC_THREAD_MAX - 1) {
        for (int i = 0; i < n; i++) thread_free(g_ccl_held[i]);
        g_ccl_setup_failed = true;
        exits("setup");
    }
    g_ccl_full_refused = !proc_thread_cap_ok(p);

    struct Thread *dead = g_ccl_held[n - 1];
    irq_state_t s = proc_threads_lock(p);
    thread_commit_exiting_locked(dead);
    proc_threads_unlock(p, s);
    __atomic_store_n(&dead->on_cpu, true, __ATOMIC_RELEASE);
    s = proc_threads_lock(p);
    g_ccl_count = p->thread_count;
    g_ccl_live  = p->live_threads;
    proc_threads_unlock(p, s);

    g_ccl_room   = proc_thread_cap_ok(p);
    g_ccl_charge = proc_sqpoll_charge(p);
    if (g_ccl_charge) proc_sqpoll_uncharge(p);
    struct Thread *nt = thread_alloc_user(p, C0_USER_ENTRY, C0_USER_SP, 0u, 0u);
    g_ccl_admit = nt && proc_thread_admit(p, nt);
    if (nt && g_ccl_admit) thread_free(nt);
    else if (nt)           thread_free_unlinked(nt);

    // Freed as its resume path would, then the never-run rest.
    irq_state_t ms = spin_lock_irqsave(NULL);
    __atomic_store_n(&dead->on_cpu, false, __ATOMIC_RELEASE);
    thread_reap_dead(dead);
    spin_unlock_irqrestore(NULL, ms);
    for (int i = 0; i < n - 1; i++) thread_free(g_ccl_held[i]);
    exits("ok");
}

void test_proc_cap_counts_live_threads(void) {
    g_ccl_exempt = g_ccl_setup_failed = g_ccl_full_refused = false;
    g_ccl_room = g_ccl_charge = g_ccl_admit = false;
    g_ccl_count = g_ccl_live = -1;
    int child_pid = rfork(RFPROC, ccl_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    int status = -1;
    TEST_EXPECT_EQ(wait_pid(&status), child_pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0, "child exited cleanly");
    TEST_ASSERT(!g_ccl_exempt, "the child runs as a user, so the cap applies");
    TEST_ASSERT(!g_ccl_setup_failed, "setup: 255 threads beside the caller");
    TEST_ASSERT(g_ccl_full_refused, "control: with every slot live, the cap refuses");
    TEST_EXPECT_EQ(g_ccl_count, PROC_THREAD_MAX, "the exited peer is still on the list");
    TEST_EXPECT_EQ(g_ccl_live, PROC_THREAD_MAX - 1, "and no longer live");
    TEST_ASSERT(g_ccl_room, "a thread that committed its exit no longer counts, though unfreed");
    TEST_ASSERT(g_ccl_charge, "the executor charge sees the same room");
    TEST_ASSERT(g_ccl_admit, "and a create is admitted at once");
}

static struct Proc      *volatile g_ewr_proc;
static struct AddrSpace *volatile g_ewr_old;
static volatile u64      g_ewr_waits0;
static volatile unsigned g_ewr_peer_cpu, g_ewr_exec_cpu;
static volatile bool     g_ewr_in_tail, g_ewr_waited, g_ewr_swapped_early;
static volatile bool     g_ewr_setup_failed, g_ewr_late, g_ewr_swapped;
static volatile int      g_ewr_count_after;

static bool ewr_as_swapped(void) {
    irq_state_t s = proc_table_lock_acquire();
    bool swapped = g_ewr_proc->as != g_ewr_old;
    proc_table_lock_release(s);
    return swapped;
}

// On the peer, after its EXITING commit, still on its own CPU: wait until exec
// has found it busy (or has swapped without waiting), then hold a while.
static void ewr_tail_hook(void) {
    proc_exit_tail_test_set_hook(NULL);
    g_ewr_peer_cpu = smp_cpu_idx_self();
    __atomic_store_n(&g_ewr_in_tail, true, __ATOMIC_RELEASE);
    u64 deadline = timer_now_ns() + TEST_YIELD_BUDGET_NS;
    while (proc_exec_peer_waits_for_test() == g_ewr_waits0 && !ewr_as_swapped() &&
           timer_now_ns() < deadline)
        __asm__ __volatile__("yield" ::: "memory");
    g_ewr_waited = proc_exec_peer_waits_for_test() != g_ewr_waits0;
    u64 hold = timer_now_ns() + 20000000ull;
    while (timer_now_ns() < hold)
        __asm__ __volatile__("yield" ::: "memory");
    g_ewr_swapped_early = ewr_as_swapped();
}

static void ewr_peer_entry(void *arg) {
    (void)arg;
    proc_exit_tail_test_set_hook(ewr_tail_hook);
    thread_exit_self();
}

// The exec'ing thread. Its peer is pinned to another CPU and that CPU kicked:
// the in-kernel phase keeps secondaries parked, and a cross-CPU enqueue does
// not wake one (test_jit does the same).
static void ewr_entry(void *arg) {
    unsigned cpu = (unsigned)(uintptr_t)arg;
    struct Proc *p = current_thread()->proc;
    struct AddrSpace *nas = addrspace_alloc(p->page_budget);
    struct Thread *peer = nas ? thread_create_with_arg(p, ewr_peer_entry, NULL) : NULL;
    if (!peer) {
        if (nas) addrspace_unref(nas);
        __atomic_store_n(&g_ewr_setup_failed, true, __ATOMIC_RELEASE);
        exits("setup");
    }
    irq_state_t s = proc_table_lock_acquire();
    g_ewr_old = p->as;
    proc_table_lock_release(s);
    g_ewr_proc   = p;
    g_ewr_waits0 = proc_exec_peer_waits_for_test();
    peer->cpu_pinned = true;
    ready_on(cpu, peer);
    (void)gic_send_ipi(cpu, IPI_RESCHED);
    u64 deadline = timer_now_ns() + TEST_YIELD_BUDGET_NS;
    while (!__atomic_load_n(&g_ewr_in_tail, __ATOMIC_ACQUIRE) && timer_now_ns() < deadline)
        sched();
    if (!__atomic_load_n(&g_ewr_in_tail, __ATOMIC_ACQUIRE)) {
        // No exec without the peer in its tail. It runs its entry when its CPU
        // does and leaves as the last thread, so the Proc still reaps.
        __atomic_store_n(&g_ewr_late, true, __ATOMIC_RELEASE);
        addrspace_unref(nas);
        exits("peer-late");
    }
    g_ewr_exec_cpu = smp_cpu_idx_self();
    proc_exec_replace(p, nas, p->phenotype);
    g_ewr_swapped = true;
    s = proc_threads_lock(p);
    g_ewr_count_after = p->thread_count;
    proc_threads_unlock(p, s);
    exits("ok");
}

void test_proc_exec_waits_for_exiting_peer_on_another_cpu(void) {
    unsigned self = smp_cpu_idx_self();
    unsigned cpu  = DTB_MAX_CPUS;
    for (unsigned c = 0; c < smp_cpu_count() && c < DTB_MAX_CPUS; c++)
        if (c != self && g_cpu_alive[c]) { cpu = c; break; }
    if (cpu == DTB_MAX_CPUS) {
        uart_puts("  proc.exec_waits_for_exiting_peer_on_another_cpu: one CPU, so it did not run\n");
        return;
    }
    g_ewr_proc   = NULL;
    g_ewr_old    = NULL;
    g_ewr_waits0 = 0;
    g_ewr_peer_cpu = g_ewr_exec_cpu = DTB_MAX_CPUS;
    g_ewr_in_tail = g_ewr_waited = g_ewr_setup_failed = g_ewr_late = g_ewr_swapped = false;
    g_ewr_swapped_early = true;
    g_ewr_count_after   = -1;

    int child_pid = rfork(RFPROC, ewr_entry, (void *)(uintptr_t)cpu);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    int status = -1;
    int reaped = wait_pid(&status);
    proc_exit_tail_test_set_hook(NULL);
    TEST_EXPECT_EQ(reaped, child_pid, "wait_pid pid mismatch");
    TEST_ASSERT(!g_ewr_setup_failed, "setup: a peer and a new address space");
    TEST_ASSERT(!g_ewr_late, "the peer reached its exit tail in time");
    TEST_EXPECT_EQ(status, 0, "child exited cleanly");
    TEST_EXPECT_EQ(g_ewr_peer_cpu, cpu, "the peer's tail ran on the CPU it was pinned to");
    TEST_ASSERT(g_ewr_exec_cpu != g_ewr_peer_cpu, "exec ran on another CPU: the real shape");
    TEST_ASSERT(!g_ewr_swapped_early,
        "exec kept the old address space while the peer's tail held its CPU");
    // The control for the line above: exec was waiting, not merely slow to swap.
    TEST_ASSERT(g_ewr_waited, "exec found the peer not yet freed and waited");
    TEST_ASSERT(g_ewr_swapped, "exec finished once the peer was freed");
    TEST_EXPECT_EQ(g_ewr_count_after, 1, "the new image starts alone");
}

static volatile bool g_srd_exempt, g_srd_tid_spent;
static volatile s64  g_srd_rc;

static void srd_entry(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;
    g_srd_exempt    = proc_resource_exempt(p);
    g_srd_tid_spent = thread_tid_space_spent();
    irq_state_t s = proc_table_lock_acquire();
    proc_group_terminate(p, 0, "c0-dying");
    proc_table_lock_release(s);
    g_srd_rc = sys_thread_spawn_for_test(C0_USER_ENTRY, C0_USER_SP, 0, 0, 0);
    thread_exit_self();
}

void test_proc_spawn_refused_in_dying_proc(void) {
    g_srd_exempt = g_srd_tid_spent = false;
    g_srd_rc = 0;
    int child_pid = rfork(RFPROC, srd_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    int status = -1;
    TEST_EXPECT_EQ(wait_pid(&status), child_pid, "wait_pid pid mismatch");
    // EAGAIN has two other sources; rule both out.
    TEST_ASSERT(g_srd_exempt, "the child is exempt, so the thread cap cannot refuse");
    TEST_ASSERT(!g_srd_tid_spent, "the tid space has room, so it cannot refuse");
    TEST_EXPECT_EQ(g_srd_rc, (s64)-T_E_AGAIN, "a dying Proc refuses a new thread");
}

// Both user spawn arms decide through proc_thread_admit (ARCH 7.3.1): a claim
// that lands after an arm's lockless pre-checks, at the top of its admit, is
// refused there and links nothing. An arm that linked at create, as before C3,
// returns a thread instead -- the primitive's own test cannot see that. The hook
// running is the proof the arm reached its admit, past every earlier refusal.
s64 viv_clone_thread_for_test(struct exception_context *ctx, const u64 *args);

static volatile s64  g_sta_rc;
static volatile int  g_sta_linked;
static volatile bool g_sta_hook_ran, g_sta_exempt;

static void sta_claim(struct Proc *p) {
    g_sta_hook_ran = true;
    irq_state_t s = proc_table_lock_acquire();
    proc_group_terminate(p, NOTE_SIG_KILL, "killed");
    proc_table_lock_release(s);
}

static void sta_entry(void *arg) {
    struct Proc *p = current_thread()->proc;
    g_sta_exempt = proc_resource_exempt(p);
    int before = __atomic_load_n(&p->thread_count, __ATOMIC_ACQUIRE);
    proc_thread_admit_test_set_hook(sta_claim);
    if (arg) {
        struct exception_context ctx;
        for (size_t i = 0; i < sizeof(ctx); i++) ((u8 *)&ctx)[i] = 0;
        u64 args[5];
        args[0] = 0;               // flags: no settid / cleartid words
        args[1] = C0_USER_SP;      // child_sp
        args[2] = 0;
        args[3] = 0;
        args[4] = 0;
        g_sta_rc = viv_clone_thread_for_test(&ctx, args);
    } else {
        g_sta_rc = sys_thread_spawn_for_test(C0_USER_ENTRY, C0_USER_SP, 0, 0, 0);
    }
    proc_thread_admit_test_set_hook(NULL);
    g_sta_linked = __atomic_load_n(&p->thread_count, __ATOMIC_ACQUIRE) - before;
    thread_exit_self();
}

void test_proc_spawn_arms_decide_at_the_admit(void) {
    static const char *const arm[2] = { "thread_spawn", "clone" };
    for (int i = 0; i < 2; i++) {
        g_sta_rc = 0;
        g_sta_linked = -1;
        g_sta_hook_ran = g_sta_exempt = false;
        int child_pid = rfork(RFPROC, sta_entry, i ? (void *)1 : NULL);
        TEST_ASSERT(child_pid > 0, "rfork failed");
        int status = -1;
        TEST_EXPECT_EQ(wait_pid(&status), child_pid, "wait_pid pid mismatch");
        uart_puts("    sta arm=");
        uart_puts(arm[i]);
        uart_puts("\n");
        TEST_ASSERT(g_sta_exempt, "the child is exempt, so the thread cap cannot refuse");
        TEST_ASSERT(g_sta_hook_ran, "the arm reached proc_thread_admit");
        TEST_EXPECT_EQ(g_sta_rc, (s64)-T_E_AGAIN, "a claim at the admit refuses the spawn");
        TEST_EXPECT_EQ(g_sta_linked, 0, "the refused thread was never linked");
    }
}

static volatile int  g_tps_tid;
static volatile bool g_tps_exempt, g_tps_spent, g_tps_refused, g_tps_pid_refused;
static volatile s64  g_tps_spawn_rc;

static void tps_entry(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;
    g_tps_exempt = proc_resource_exempt(p);
    // Parked one short of the end: one create fits, then the space is spent.
    // Nothing between the swaps sleeps, so the window stays short.
    u32 saved = thread_tid_counter_swap_for_test(0x7ffffffeu);
    struct Thread *last    = thread_create_with_arg(p, c0_never_entry, NULL);
    g_tps_spent            = thread_tid_space_spent();
    struct Thread *refused = thread_create_with_arg(p, c0_never_entry, NULL);
    g_tps_spawn_rc         = sys_thread_spawn_for_test(C0_USER_ENTRY, C0_USER_SP, 0, 0, 0);
    thread_tid_counter_swap_for_test(saved);
    g_tps_tid     = last ? last->tid : -1;
    g_tps_refused = refused == NULL;
    if (last)    thread_free(last);
    if (refused) thread_free(refused);

    u32 saved_pid = proc_pid_counter_swap_for_test(0x7fffffffu);
    struct Proc *q = proc_alloc();
    proc_pid_counter_swap_for_test(saved_pid);
    g_tps_pid_refused = q == NULL;
    if (q) {
        q->state = PROC_STATE_ZOMBIE;
        proc_free(q);
    }
    exits("ok");
}

void test_thread_tid_and_pid_spaces_refuse_when_spent(void) {
    g_tps_tid = 0;
    g_tps_exempt = g_tps_spent = g_tps_refused = g_tps_pid_refused = false;
    g_tps_spawn_rc = 0;
    int child_pid = rfork(RFPROC, tps_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    int status = -1;
    TEST_EXPECT_EQ(wait_pid(&status), child_pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0, "child exited cleanly");
    TEST_EXPECT_EQ(g_tps_tid, 0x7ffffffe, "the last tid in the space is handed out");
    TEST_ASSERT(g_tps_spent, "the space reads spent once its last tid is out");
    TEST_ASSERT(g_tps_refused, "a create past the last tid is refused");
    TEST_ASSERT(g_tps_exempt, "the child is exempt, so the thread cap cannot refuse");
    TEST_EXPECT_EQ(g_tps_spawn_rc, (s64)-T_E_AGAIN,
        "a spawn past the last tid answers EAGAIN, a system limit, not ENOMEM");
    TEST_ASSERT(g_tps_pid_refused, "a Proc past the last pid is refused");
}

// ---------------------------------------------------------------------------
// Exec with threads (ARCH 7.3.3, thread-model C5; specs/thread_reap.tla
// ExecClaim, ExecAbort, ExecSwap).
//
// proc.exec_claim_dooms_every_other_thread reads the predicates and the spawn
// gates against a claim set by hand. The rest drive a real claim from a
// child's main thread: it brings its peers into place, holds them there with
// no claim (the control: nothing but the claim moves them), then claims. The
// claim sleeps for group death alone, so past the runner's budget a claim that
// never returned is ended by a kill.
// ---------------------------------------------------------------------------

static volatile bool g_xd_setup_failed, g_xd_exempt, g_xd_admitted, g_xd_hook_ran;
static volatile bool g_xd_n_die, g_xd_n_gdeath, g_xd_n_dooms;
static volatile bool g_xd_m_die, g_xd_m_gdeath, g_xd_m_dooms;
static volatile bool g_xd_ctl_die, g_xd_ctl_gdeath, g_xd_ctl_dooms;
static volatile bool g_xd_pre_reached[2], g_xd_admit_reached[2];
static volatile s64  g_xd_pre_rc[2], g_xd_admit_rc[2];
static volatile int  g_xd_admit_linked[2], g_xd_claim_rc;
static struct Thread *volatile g_xd_main;

static void xd_set_claim(struct Proc *p, struct Thread *t) {
    irq_state_t s = proc_threads_lock(p);
    __atomic_store_n(&p->exec_task, t, __ATOMIC_RELEASE);
    proc_threads_unlock(p, s);
}

static void xd_hook_note(struct Proc *p) {
    (void)p;
    g_xd_hook_ran = true;
}

// A claim landing at the top of the admit, after the arm's pre-checks passed.
static void xd_hook_claim(struct Proc *p) {
    g_xd_hook_ran = true;
    xd_set_claim(p, g_xd_main);
}

static s64 xd_spawn(int arm) {
    if (arm) {
        struct exception_context ctx;
        for (size_t i = 0; i < sizeof(ctx); i++) ((u8 *)&ctx)[i] = 0;
        u64 args[5] = { 0, C0_USER_SP, 0, 0, 0 };
        return viv_clone_thread_for_test(&ctx, args);
    }
    return sys_thread_spawn_for_test(C0_USER_ENTRY, C0_USER_SP, 0, 0, 0);
}

static void xd_entry(void *arg) {
    (void)arg;
    struct Thread *self = current_thread();
    struct Proc *p = self->proc;
    g_xd_main   = self;
    g_xd_exempt = proc_resource_exempt(p);
    struct Thread *n = thread_create_with_arg(p, c0_never_entry, NULL);
    if (!n) {
        g_xd_setup_failed = true;
        exits("setup");
    }

    xd_set_claim(p, self);
    g_xd_n_die    = thread_die_pending(n);
    g_xd_n_gdeath = thread_group_death_pending(n);
    g_xd_n_dooms  = proc_exec_dooms(p, n);
    g_xd_m_die    = thread_die_pending(self);
    g_xd_m_gdeath = thread_group_death_pending(self);
    g_xd_m_dooms  = proc_exec_dooms(p, self);
    for (int arm = 0; arm < 2; arm++) {
        g_xd_hook_ran = false;
        proc_thread_admit_test_set_hook(xd_hook_note);
        g_xd_pre_rc[arm] = xd_spawn(arm);
        proc_thread_admit_test_set_hook(NULL);
        g_xd_pre_reached[arm] = g_xd_hook_ran;
    }
    struct Thread *nt = thread_alloc_user(p, C0_USER_ENTRY, C0_USER_SP, 0, 0);
    if (nt) {
        g_xd_admitted = proc_thread_admit(p, nt);
        if (g_xd_admitted) thread_free(nt);
        else               thread_free_unlinked(nt);
    } else {
        g_xd_setup_failed = true;
    }

    xd_set_claim(p, NULL);
    g_xd_ctl_die    = thread_die_pending(n);
    g_xd_ctl_gdeath = thread_group_death_pending(n);
    g_xd_ctl_dooms  = proc_exec_dooms(p, n);
    for (int arm = 0; arm < 2; arm++) {
        g_xd_hook_ran = false;
        int before = __atomic_load_n(&p->thread_count, __ATOMIC_ACQUIRE);
        proc_thread_admit_test_set_hook(xd_hook_claim);
        g_xd_admit_rc[arm] = xd_spawn(arm);
        proc_thread_admit_test_set_hook(NULL);
        g_xd_admit_reached[arm] = g_xd_hook_ran;
        g_xd_admit_linked[arm]  = __atomic_load_n(&p->thread_count, __ATOMIC_ACQUIRE) - before;
        xd_set_claim(p, NULL);
    }

    thread_free(n);
    irq_state_t s = proc_table_lock_acquire();
    proc_group_terminate(p, 0, "c5-dying");
    proc_table_lock_release(s);
    g_xd_claim_rc = proc_exec_claim(p);
    thread_exit_self();
}

void test_proc_exec_claim_dooms_every_other_thread(void) {
    static const char *const arm[2] = { "thread_spawn", "clone" };
    g_xd_setup_failed = g_xd_exempt = g_xd_admitted = g_xd_hook_ran = false;
    g_xd_n_die = g_xd_n_gdeath = g_xd_n_dooms = false;
    g_xd_m_die = g_xd_m_gdeath = g_xd_m_dooms = true;
    g_xd_ctl_die = g_xd_ctl_gdeath = g_xd_ctl_dooms = true;
    g_xd_main = NULL;
    g_xd_claim_rc = 0x7fffffff;
    for (int i = 0; i < 2; i++) {
        g_xd_pre_reached[i] = true;
        g_xd_admit_reached[i] = false;
        g_xd_pre_rc[i] = g_xd_admit_rc[i] = 0;
        g_xd_admit_linked[i] = -1;
    }
    int child_pid = rfork(RFPROC, xd_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    int status = -1;
    TEST_EXPECT_EQ(wait_pid(&status), child_pid, "wait_pid pid mismatch");
    TEST_ASSERT(!g_xd_setup_failed, "setup: a never-run peer and a user thread to admit");
    TEST_ASSERT(g_xd_exempt, "the child is exempt, so the thread cap cannot refuse");
    TEST_ASSERT(g_xd_n_die, "another thread's claim reads as death (thread_die_pending)");
    TEST_ASSERT(g_xd_n_gdeath, "and as group death (thread_group_death_pending)");
    TEST_ASSERT(g_xd_n_dooms, "and dooms the peer");
    TEST_ASSERT(!g_xd_m_die && !g_xd_m_gdeath && !g_xd_m_dooms,
        "the execer is not doomed by its own claim");
    // The control for the three lines above, one variable away: no claim.
    TEST_ASSERT(!g_xd_ctl_die && !g_xd_ctl_gdeath && !g_xd_ctl_dooms,
        "with no claim the same peer is not doomed");
    TEST_ASSERT(!g_xd_admitted, "admission refuses a thread while a claim is held");
    for (int i = 0; i < 2; i++) {
        uart_puts("    xd arm=");
        uart_puts(arm[i]);
        uart_puts("\n");
        TEST_EXPECT_EQ(g_xd_pre_rc[i], (s64)-T_E_AGAIN, "a held claim refuses the spawn");
        TEST_ASSERT(!g_xd_pre_reached[i], "it refused before the admit");
        TEST_ASSERT(g_xd_admit_reached[i], "with no claim the arm reached its admit");
        TEST_EXPECT_EQ(g_xd_admit_rc[i], (s64)-T_E_AGAIN, "a claim at the admit refuses the spawn");
        TEST_EXPECT_EQ(g_xd_admit_linked[i], 0, "the refused thread was never linked");
    }
    TEST_EXPECT_EQ(g_xd_claim_rc, -1, "a claim on a dying Proc is refused");
}

#define XC_MAX_PEERS  3
#define XC_UNSET      0x7fffffff

enum xc_kind  { XC_SLEEPER, XC_DEATH_ONLY, XC_SPINNER, XC_DEAF, XC_EXITER, XC_TRAPPED,
                XC_VFORK };
enum xc_phase { XC_SETUP, XC_CLAIMING, XC_DONE, XC_FAILED };

#define XC_SVC      "c5svc"
#define XC_SVC_LEN  5

void vfork_await_release_for_test(struct Proc *p, int child_pid);
extern void srv_registry_reset(void);

// The fixture each test sets before it forks.
static int          g_xc_npeers;
static enum xc_kind g_xc_kind[XC_MAX_PEERS];
static bool         g_xc_unborn_wanted, g_xc_job_stop, g_xc_second_claim;
static bool         g_xc_unborn_user, g_xc_post;

static struct Proc   *volatile g_xc_proc;
static struct Thread *volatile g_xc_unborn;
static struct Rendez g_xc_rendez[XC_MAX_PEERS];
static volatile int  g_xc_phase, g_xc_main_tid;
static volatile int  g_xc_tid[XC_MAX_PEERS], g_xc_rc[XC_MAX_PEERS];
static volatile bool g_xc_up[XC_MAX_PEERS], g_xc_release;
static volatile u64  g_xc_spins[XC_MAX_PEERS];
static volatile int  g_xc_second_rc, g_xc_claim_rc, g_xc_live_at_claim, g_xc_count_after;
static volatile bool g_xc_held_in_place, g_xc_claim_owned, g_xc_stop_during, g_xc_stop_after;
static volatile bool g_xc_cleared_at_swap, g_xc_cleared_at_abort;
static volatile bool g_xc_svc_live_at_claim, g_xc_latch_after;
static struct Proc   *volatile g_xc_vfork_child;
static volatile int  g_xc_vfork_pid;
static volatile bool g_xc_vfork_release, g_xc_vfork_kept, g_xc_vfork_reaped;

static void xc_reset(void) {
    g_xc_npeers = 0;
    g_xc_unborn_wanted = g_xc_job_stop = g_xc_second_claim = false;
    g_xc_unborn_user = g_xc_post = false;
    g_xc_svc_live_at_claim = g_xc_latch_after = false;
    g_xc_vfork_child   = NULL;
    g_xc_vfork_pid     = 0;
    g_xc_vfork_release = g_xc_vfork_kept = g_xc_vfork_reaped = false;
    g_xc_proc   = NULL;
    g_xc_unborn = NULL;
    g_xc_phase  = XC_SETUP;
    g_xc_main_tid = 0;
    for (int i = 0; i < XC_MAX_PEERS; i++) {
        g_xc_kind[i]  = XC_SLEEPER;
        g_xc_tid[i]   = 0;
        g_xc_rc[i]    = XC_UNSET;
        g_xc_up[i]    = false;
        g_xc_spins[i] = 0;
        rendez_init(&g_xc_rendez[i]);
    }
    g_xc_release = false;
    g_xc_second_rc = g_xc_claim_rc = XC_UNSET;
    g_xc_live_at_claim = g_xc_count_after = -1;
    g_xc_held_in_place = g_xc_claim_owned = g_xc_stop_during = g_xc_stop_after = false;
    g_xc_cleared_at_swap = g_xc_cleared_at_abort = false;
}

static int xc_never(void *arg) {
    (void)arg;
    return 0;
}

// The vfork child: it holds the space it shares with its parent until the
// test lets it go.
static void xc_vfork_child(void *arg) {
    (void)arg;
    __atomic_store_n(&g_xc_vfork_child, current_thread()->proc, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&g_xc_vfork_release, __ATOMIC_ACQUIRE))
        sched();
    exits("ok");
}

// Lets the vfork child go and reaps it; true iff it was reaped. A dying caller
// cannot wait, and the child is then left to the orphan rule.
static bool xc_vfork_reap(void) {
    __atomic_store_n(&g_xc_vfork_release, true, __ATOMIC_RELEASE);
    int pid = __atomic_load_n(&g_xc_vfork_pid, __ATOMIC_ACQUIRE);
    if (pid <= 0) return false;
    int st = -1;
    return wait_pid_for(pid, WAIT_PACKED, &st) == pid;
}

static void xc_peer_entry(void *arg) {
    int i = (int)(uintptr_t)arg;
    struct Thread *self = current_thread();
    __atomic_store_n(&g_xc_up[i], true, __ATOMIC_RELEASE);
    switch (g_xc_kind[i]) {
    case XC_SLEEPER:
    case XC_DEATH_ONLY:
        g_xc_rc[i] = g_xc_kind[i] == XC_SLEEPER
                   ? sleep(&g_xc_rendez[i], xc_never, NULL)
                   : sleep_death_only(&g_xc_rendez[i], xc_never, NULL);
        if (g_xc_second_claim && g_xc_kind[i] == XC_SLEEPER)
            g_xc_second_rc = proc_exec_claim(self->proc);
        break;
    case XC_SPINNER:
        // A thread at EL0 meets the claim at its return tail's die-check.
        for (;;) {
            __atomic_fetch_add(&g_xc_spins[i], 1u, __ATOMIC_RELEASE);
            el0_return_die_check();
            sched();
        }
    case XC_DEAF:
        // A thread the claim has not reached: it takes no die-check until released.
        while (!__atomic_load_n(&g_xc_release, __ATOMIC_ACQUIRE))
            sched();
        break;
    case XC_EXITER:
    case XC_TRAPPED:
        // Woken to die, the thread ends itself instead: an exit call, or a death
        // by a trap.
        g_xc_rc[i] = sleep(&g_xc_rendez[i], xc_never, NULL);
        if (g_xc_kind[i] == XC_EXITER) exits("c5-peer");
        exits_note(NOTE_NAME_SNARE_ILL);
    case XC_VFORK: {
        // A vfork parent parks until its child gives back the space they share.
        int cpid = rfork(RFPROC | RFMEM, xc_vfork_child, NULL);
        __atomic_store_n(&g_xc_vfork_pid, cpid, __ATOMIC_RELEASE);
        if (cpid > 0) vfork_await_release_for_test(self->proc, cpid);
        g_xc_rc[i] = cpid > 0 ? 0 : -1;
        break;
    }
    }
    thread_exit_self();
}

// Each peer where the claim must find it: a sleeper still in its sleep, on its
// rendez or parked for the stop; a spinner running; a deaf peer up.
static bool xc_peers_in_place(struct Proc *p) {
    for (int i = 0; i < g_xc_npeers; i++) {
        if (!__atomic_load_n(&g_xc_up[i], __ATOMIC_ACQUIRE)) return false;
        if (g_xc_kind[i] == XC_SPINNER) {
            if (__atomic_load_n(&g_xc_spins[i], __ATOMIC_ACQUIRE) == 0) return false;
        } else if (g_xc_kind[i] != XC_DEAF) {
            struct test_thread_view v = test_thread_view(p, g_xc_tid[i]);
            bool on = g_xc_job_stop ? v.on_debug_rendez
                    : g_xc_kind[i] == XC_VFORK
                        ? v.blocked_on != NULL && __atomic_load_n(&g_xc_vfork_pid, __ATOMIC_ACQUIRE) > 0
                        : v.blocked_on == &g_xc_rendez[i];
            if (g_xc_rc[i] != XC_UNSET || !v.present || v.state != THREAD_SLEEPING || !on)
                return false;
        }
    }
    return true;
}

static void xc_main_entry(void *arg) {
    (void)arg;
    struct Thread *self = current_thread();
    struct Proc *p = self->proc;
    g_xc_main_tid = self->tid;
    g_xc_proc     = p;
    if (g_xc_job_stop) __atomic_store_n(&p->job_stop_req, 1u, __ATOMIC_RELEASE);
    struct AddrSpace *nas = addrspace_alloc(p->page_budget);
    struct AddrSpace *old_as = p->as;
    bool ok = nas != NULL;
    if (ok && g_xc_post) {
        proc_mark_may_post_service(p);
        struct Spoor *root = devsrv_attach_registry(srv_boot_registry());
        ok = root && devsrv_post_listener(p, root, XC_SVC, XC_SVC_LEN, SRV_MODE_9P,
                                          false, false, false) >= 0;
        if (root) spoor_clunk(root);
    }
    for (int i = 0; ok && i < g_xc_npeers; i++) {
        struct Thread *t = thread_create_with_arg(p, xc_peer_entry, (void *)(uintptr_t)i);
        ok = t != NULL;
        if (ok) {
            g_xc_tid[i] = t->tid;
            ready(t);
        }
    }
    if (ok && g_xc_unborn_wanted && g_xc_unborn_user) {
        // Built and admitted as both spawn arms do, and not readied.
        struct Thread *nt = thread_alloc_user(p, C0_USER_ENTRY, C0_USER_SP, 0, 0);
        ok = nt != NULL && proc_thread_admit(p, nt);
        if (ok)      g_xc_unborn = nt;
        else if (nt) thread_free_unlinked(nt);
    } else if (ok && g_xc_unborn_wanted) {
        g_xc_unborn = thread_create_with_arg(p, c0_never_entry, NULL);
        ok = g_xc_unborn != NULL;
    }
    u64 deadline = timer_now_ns() + TEST_YIELD_BUDGET_NS;
    while (ok && !xc_peers_in_place(p) && timer_now_ns() < deadline)
        sched();
    ok = ok && xc_peers_in_place(p);
    if (ok) {
        u64 spins0[XC_MAX_PEERS];
        for (int i = 0; i < g_xc_npeers; i++)
            spins0[i] = __atomic_load_n(&g_xc_spins[i], __ATOMIC_ACQUIRE);
        u64 hold = timer_now_ns() + 20000000ull;
        while (timer_now_ns() < hold)
            sched();
        bool held = xc_peers_in_place(p) &&
                    __atomic_load_n(&p->live_threads, __ATOMIC_RELAXED) ==
                        1 + g_xc_npeers + (g_xc_unborn_wanted ? 1 : 0);
        for (int i = 0; i < g_xc_npeers; i++)
            if (g_xc_kind[i] == XC_SPINNER &&
                __atomic_load_n(&g_xc_spins[i], __ATOMIC_ACQUIRE) == spins0[i])
                held = false;
        g_xc_held_in_place = held;
    }
    if (!ok) {
        // The runner frees the unborn thread only once the child claims.
        if (g_xc_unborn) thread_free(g_xc_unborn);
        g_xc_unborn = NULL;
        if (nas) addrspace_unref(nas);
        __atomic_store_n(&p->job_stop_req, 0u, __ATOMIC_RELEASE);
        // A vfork peer still parked leaves its park once its child is gone.
        xc_vfork_reap();
        __atomic_store_n(&g_xc_phase, XC_FAILED, __ATOMIC_RELEASE);
        exits("setup");
    }

    __atomic_store_n(&g_xc_phase, XC_CLAIMING, __ATOMIC_RELEASE);
    int rc = proc_exec_claim(p);
    irq_state_t s = proc_threads_lock(p);
    g_xc_live_at_claim = p->live_threads;
    g_xc_claim_owned   = p->exec_task == self;
    proc_threads_unlock(p, s);
    g_xc_stop_during = proc_stop_requested(p);
    if (g_xc_post) {
        struct SrvService *svc = srv_lookup_in(srv_boot_registry(), XC_SVC, XC_SVC_LEN);
        g_xc_svc_live_at_claim = svc && svc->state == SRV_STATE_LIVE &&
                                 svc->poster_stripes == proc_stripes(p);
    }
    g_xc_claim_rc = rc;
    if (rc == 0) {
        proc_exec_replace(p, nas, p->phenotype);
        s = proc_threads_lock(p);
        g_xc_count_after     = p->thread_count;
        g_xc_cleared_at_swap = p->exec_task == NULL;
        proc_threads_unlock(p, s);
        g_xc_stop_after  = proc_stop_requested(p);
        g_xc_latch_after = proc_intr_terminate_pending(p);
        // The vfork child, spinning until let go, still holds the old space.
        struct Proc *c = __atomic_load_n(&g_xc_vfork_child, __ATOMIC_ACQUIRE);
        g_xc_vfork_kept = c && c->state == PROC_STATE_ALIVE && c->as == old_as &&
                          p->as != old_as;
        if (__atomic_load_n(&g_xc_vfork_pid, __ATOMIC_ACQUIRE) > 0)
            g_xc_vfork_reaped = xc_vfork_reap();
        __atomic_store_n(&p->job_stop_req, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&g_xc_phase, XC_DONE, __ATOMIC_RELEASE);
        exits("ok");
    }
    // Abandoned: group death ended the wait, and the execer dies with the group.
    addrspace_unref(nas);
    s = proc_threads_lock(p);
    g_xc_cleared_at_abort = p->exec_task == NULL;
    proc_threads_unlock(p, s);
    __atomic_store_n(&p->job_stop_req, 0u, __ATOMIC_RELEASE);
    xc_vfork_reap();
    __atomic_store_n(&g_xc_phase, XC_DONE, __ATOMIC_RELEASE);
    thread_exit_self();
}

static int xc_phase(void) {
    return __atomic_load_n(&g_xc_phase, __ATOMIC_ACQUIRE);
}

static bool xc_main_in_claim(void) {
    struct Proc *p = g_xc_proc;
    if (!p) return false;
    struct test_thread_view v = test_thread_view(p, g_xc_main_tid);
    return v.present && v.state == THREAD_SLEEPING && v.blocked_on == &p->exec_rendez;
}

// Every peer but a deaf one dies with the group, and so does a claim's wait.
static void xc_kill(void) {
    struct Proc *p = g_xc_proc;
    if (!p) return;
    irq_state_t s = proc_table_lock_acquire();
    if (p->state == PROC_STATE_ALIVE) proc_group_kill(p);
    proc_table_lock_release(s);
}

// Once the child has claimed: hold a while and report whether its claim was
// still waiting at the end, asleep for it.
static bool xc_claim_held(void) {
    TEST_YIELD_UNTIL_SOFT(xc_main_in_claim() || xc_phase() == XC_DONE ||
                          xc_phase() == XC_FAILED);
    if (!xc_main_in_claim()) return false;
    u64 hold = timer_now_ns() + 20000000ull;
    while (timer_now_ns() < hold)
        sched();
    return xc_main_in_claim() && xc_phase() == XC_CLAIMING;
}

// The child's end: its claim resolved, or a kill past the budget, then a deaf
// peer released and the Proc reaped. Returns the packed status. A vfork child
// the dying Proc could not reap is orphaned to kproc, this thread's Proc, where
// a later test's wait for any child would take it; it is reaped here.
static int xc_finish(int child_pid) {
    TEST_YIELD_UNTIL_SOFT(xc_phase() == XC_DONE || xc_phase() == XC_FAILED);
    if (xc_phase() != XC_DONE && xc_phase() != XC_FAILED) xc_kill();
    __atomic_store_n(&g_xc_release, true, __ATOMIC_RELEASE);
    __atomic_store_n(&g_xc_vfork_release, true, __ATOMIC_RELEASE);
    int status = -1;
    int reaped = wait_pid_for(child_pid, WAIT_PACKED, &status);
    int vpid = __atomic_load_n(&g_xc_vfork_pid, __ATOMIC_ACQUIRE);
    if (vpid > 0 && !g_xc_vfork_reaped) {
        int vst = -1;
        (void)wait_pid_for(vpid, WAIT_PACKED, &vst);
    }
    return reaped == child_pid ? status : -3;
}

// A sleeper in sleep(), one in a death-only sleep and a thread at its die-check
// all die of the claim, which returns once the execer is alone; a doomed
// thread's own exec is refused; the swap leaves one thread and no claim.
void test_proc_exec_claim_kills_sleeping_and_running_peers(void) {
    xc_reset();
    g_xc_npeers  = 3;
    g_xc_kind[0] = XC_SLEEPER;
    g_xc_kind[1] = XC_DEATH_ONLY;
    g_xc_kind[2] = XC_SPINNER;
    g_xc_second_claim = true;
    int child_pid = rfork(RFPROC, xc_main_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    int status = xc_finish(child_pid);
    TEST_ASSERT(xc_phase() != XC_FAILED, "setup: three peers in place and a new address space");
    TEST_ASSERT(g_xc_held_in_place, "with no claim the sleepers slept on and the spinner ran");
    TEST_EXPECT_EQ(g_xc_claim_rc, 0, "the claim returned holding the Proc");
    TEST_ASSERT(g_xc_claim_owned, "the claim names the execer");
    TEST_EXPECT_EQ(g_xc_live_at_claim, 1, "it returned once the execer was the only live thread");
    TEST_EXPECT_EQ(g_xc_rc[0], SLEEP_INTR, "the sleep() sleeper was woken to die");
    TEST_EXPECT_EQ(g_xc_rc[1], SLEEP_INTR, "the death-only sleeper was woken to die");
    // An admitted second claim would not reach this line: its wait would find
    // the execer already waiting on exec_rendez, and a second waiter on a
    // Rendez is an extinction. The suite stops loudly rather than reporting red.
    TEST_EXPECT_EQ(g_xc_second_rc, -1, "a doomed thread's own exec was refused");
    TEST_ASSERT(g_xc_cleared_at_swap, "the swap cleared the claim");
    TEST_EXPECT_EQ(g_xc_count_after, 1, "the new image starts alone");
    TEST_EXPECT_EQ(status, WAIT_STATUS_EXITED(0), "the child exited cleanly after the exec");
}

// Group death ends a claim still waiting for a peer it has not reached: the
// exec is abandoned and its claim cleared, and the Proc dies of the kill. Its
// last thread leaves by thread_exit_self, which tombstones its /srv service.
void test_proc_exec_claim_abandoned_on_group_death(void) {
    xc_reset();
    g_xc_npeers  = 1;
    g_xc_kind[0] = XC_DEAF;
    g_xc_post    = true;
    srv_registry_reset();
    int child_pid = rfork(RFPROC, xc_main_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    bool waited = xc_claim_held();
    if (waited) xc_kill();
    int status = xc_finish(child_pid);
    TEST_ASSERT(xc_phase() != XC_FAILED, "setup: a peer in place and a new address space");
    TEST_ASSERT(g_xc_held_in_place, "with no claim the peer stayed up");
    TEST_ASSERT(waited, "the claim slept while a live peer remained");
    TEST_EXPECT_EQ(g_xc_claim_rc, -1, "group death abandoned the exec");
    TEST_ASSERT(g_xc_cleared_at_abort, "the abandoned claim was cleared");
    TEST_EXPECT_EQ(status, WAIT_STATUS_SIGNALED(NOTE_SIG_KILL), "the Proc died of the kill");
    TEST_ASSERT(g_xc_svc_live_at_claim, "the service was posted while the claim waited");
    struct SrvService *svc = srv_lookup_in(srv_boot_registry(), XC_SVC, XC_SVC_LEN);
    TEST_ASSERT(svc && svc->state == SRV_STATE_TOMBSTONED,
        "the killed Proc's last thread out tombstoned its service");
}

// A created thread that never ran counts live until its creator frees it; the
// unlink that drops the count to one wakes the claim.
void test_proc_exec_claim_woken_by_an_abandoned_create(void) {
    xc_reset();
    g_xc_unborn_wanted = true;
    int child_pid = rfork(RFPROC, xc_main_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    bool waited = xc_claim_held();
    // The runner owns the unborn thread once the child has claimed.
    int phase = xc_phase();
    if ((phase == XC_CLAIMING || phase == XC_DONE) && g_xc_unborn) {
        thread_free(g_xc_unborn);
        g_xc_unborn = NULL;
    }
    int status = xc_finish(child_pid);
    TEST_ASSERT(xc_phase() != XC_FAILED, "setup: a never-run thread and a new address space");
    TEST_ASSERT(g_xc_held_in_place, "the never-run thread counted live");
    TEST_ASSERT(waited, "the claim slept while the never-run thread counted live");
    TEST_EXPECT_EQ(g_xc_claim_rc, 0, "freeing it woke the claim");
    TEST_EXPECT_EQ(g_xc_live_at_claim, 1, "the execer was the only live thread");
    TEST_ASSERT(g_xc_cleared_at_swap, "the swap cleared the claim");
    TEST_EXPECT_EQ(g_xc_count_after, 1, "the new image starts alone");
    TEST_EXPECT_EQ(status, WAIT_STATUS_EXITED(0), "the child exited cleanly after the exec");
}

// Under a job stop the peers are parked; the claim kills them and returns with
// the stop still requested and never resumed. No thread is asked to park while
// the claim holds, and the stop stands for the new image.
void test_proc_exec_claim_does_not_hold_for_a_stop(void) {
    xc_reset();
    g_xc_npeers  = 2;
    g_xc_kind[0] = XC_SLEEPER;
    g_xc_kind[1] = XC_DEATH_ONLY;
    g_xc_job_stop = true;
    int child_pid = rfork(RFPROC, xc_main_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    int status = xc_finish(child_pid);
    TEST_ASSERT(xc_phase() != XC_FAILED, "setup: two peers parked for the stop");
    TEST_ASSERT(g_xc_held_in_place, "with no claim both peers stayed parked");
    TEST_EXPECT_EQ(g_xc_claim_rc, 0, "the claim returned though nothing resumed the stop");
    TEST_ASSERT(!g_xc_stop_during, "no thread is asked to park while the claim holds");
    TEST_EXPECT_EQ(g_xc_rc[0], SLEEP_INTR, "the parked sleep() sleeper died");
    TEST_EXPECT_EQ(g_xc_rc[1], SLEEP_INTR, "the parked death-only sleeper died");
    TEST_ASSERT(g_xc_stop_after, "the stop stands for the new image");
    TEST_EXPECT_EQ(g_xc_count_after, 1, "the new image starts alone");
    TEST_EXPECT_EQ(status, WAIT_STATUS_EXITED(0), "the child exited cleanly after the exec");
}

// A doomed thread that ends itself -- an exit call, or a death by a trap --
// leaves alone, as Linux's do_group_exit does while group_exec_task is set
// (ARCH 7.3.3): the claim still returns and the Proc goes on into the new
// image with its /srv service posted. The service goes at the real last exit.
void test_proc_exec_claim_survives_a_doomed_exit(void) {
    xc_reset();
    g_xc_npeers  = 2;
    g_xc_kind[0] = XC_EXITER;
    g_xc_kind[1] = XC_TRAPPED;
    g_xc_post    = true;
    srv_registry_reset();
    int child_pid = rfork(RFPROC, xc_main_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    int status = xc_finish(child_pid);
    TEST_ASSERT(xc_phase() != XC_FAILED, "setup: a service posted, two peers in place and a new address space");
    TEST_ASSERT(g_xc_held_in_place, "with no claim both peers slept on");
    TEST_EXPECT_EQ(g_xc_claim_rc, 0, "a doomed thread's own exit did not end the exec");
    TEST_EXPECT_EQ(g_xc_live_at_claim, 1, "the claim returned once the execer was the only live thread");
    TEST_EXPECT_EQ(g_xc_rc[0], SLEEP_INTR, "the exiting peer was woken to die before its exits()");
    TEST_EXPECT_EQ(g_xc_rc[1], SLEEP_INTR, "the trapped peer was woken to die before its trap");
    TEST_ASSERT(g_xc_svc_live_at_claim, "a doomed thread's exit left the Proc's service posted");
    TEST_EXPECT_EQ(g_xc_count_after, 1, "the new image starts alone");
    TEST_EXPECT_EQ(status, WAIT_STATUS_EXITED(0), "the new image's own exit decided the status");
    struct SrvService *svc = srv_lookup_in(srv_boot_registry(), XC_SVC, XC_SVC_LEN);
    TEST_ASSERT(svc && svc->state == SRV_STATE_TOMBSTONED,
        "the last thread out of exits() tombstoned the service");
}

// An interrupt with no handler arms its terminate latch and wakes every
// sleeper, the execer's claim wait included. The wait reads group death alone,
// so it sleeps on until the last peer is gone, and the latch is the new
// image's to meet (POSIX keeps a pending signal across exec).
void test_proc_exec_claim_holds_through_a_terminate_latch(void) {
    xc_reset();
    g_xc_npeers  = 1;
    g_xc_kind[0] = XC_DEAF;
    int child_pid = rfork(RFPROC, xc_main_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    bool waited = xc_claim_held();
    bool posted = false, armed = false, held = false;
    struct Proc *p = g_xc_proc;
    if (waited && p) {
        posted = notes_post(p, NOTE_NAME_INTERRUPT, 0u, NULL, true) == 0;
        armed  = proc_intr_terminate_pending(p);
        proc_interrupt_terminate_wake(p);
        u64 hold = timer_now_ns() + 20000000ull;
        while (timer_now_ns() < hold)
            sched();
        held = xc_main_in_claim() && xc_phase() == XC_CLAIMING;
    }
    __atomic_store_n(&g_xc_release, true, __ATOMIC_RELEASE);
    int status = xc_finish(child_pid);
    TEST_ASSERT(xc_phase() != XC_FAILED, "setup: a peer in place and a new address space");
    TEST_ASSERT(waited, "the claim slept while a live peer remained");
    TEST_ASSERT(posted, "the interrupt was queued");
    TEST_ASSERT(armed, "with no handler it armed the terminate latch");
    TEST_ASSERT(held, "the latch and its wake left the claim waiting");
    TEST_EXPECT_EQ(g_xc_claim_rc, 0, "the claim returned once the peer was gone");
    TEST_ASSERT(g_xc_latch_after, "the latch stands for the new image");
    TEST_EXPECT_EQ(g_xc_count_after, 1, "the new image starts alone");
    TEST_EXPECT_EQ(status, WAIT_STATUS_EXITED(0), "the child exited cleanly after the exec");
}

s64 exec_claim_or_drop_for_test(struct Proc *p, struct AddrSpace *nas, struct Spoor *exe);

static volatile bool g_xr_setup_failed, g_xr_ctl_owned;
static volatile s64  g_xr_ctl_rc, g_xr_rc;
static volatile int  g_xr_ctl_as_ref, g_xr_ctl_exe_ref, g_xr_as_ref, g_xr_exe_ref;

// Exec's own references to its new image are the space it built and the
// program file it opened. The test holds one more of each, so a drop shows as
// a count, not a free. The control, one variable away, claims a Proc that is
// not dying: the claim holds and nothing is dropped.
static void xr_entry(void *arg) {
    (void)arg;
    struct Thread *self = current_thread();
    struct Proc *p = self->proc;
    struct AddrSpace *nas = addrspace_alloc(p->page_budget);
    struct Spoor *exe = devsrv_attach_registry(srv_boot_registry());
    if (!nas || !exe) {
        g_xr_setup_failed = true;
        if (nas) addrspace_unref(nas);
        if (exe) spoor_clunk(exe);
        exits("setup");
    }
    addrspace_ref(nas);
    spoor_ref(exe);

    g_xr_ctl_rc = exec_claim_or_drop_for_test(p, nas, exe);
    irq_state_t s = proc_threads_lock(p);
    g_xr_ctl_owned = p->exec_task == self;
    proc_threads_unlock(p, s);
    xd_set_claim(p, NULL);
    g_xr_ctl_as_ref  = __atomic_load_n(&nas->ref, __ATOMIC_ACQUIRE);
    g_xr_ctl_exe_ref = __atomic_load_n(&exe->ref, __ATOMIC_ACQUIRE);

    s = proc_table_lock_acquire();
    proc_group_terminate(p, 0, "c5-dying");
    proc_table_lock_release(s);
    g_xr_rc = exec_claim_or_drop_for_test(p, nas, exe);
    g_xr_as_ref  = __atomic_load_n(&nas->ref, __ATOMIC_ACQUIRE);
    g_xr_exe_ref = __atomic_load_n(&exe->ref, __ATOMIC_ACQUIRE);

    addrspace_unref(nas);
    spoor_clunk(exe);
    thread_exit_self();
}

void test_proc_exec_claim_refused_drops_the_new_image(void) {
    g_xr_setup_failed = g_xr_ctl_owned = false;
    g_xr_ctl_rc = g_xr_rc = XC_UNSET;
    g_xr_ctl_as_ref = g_xr_ctl_exe_ref = g_xr_as_ref = g_xr_exe_ref = -1;
    int child_pid = rfork(RFPROC, xr_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    int status = -1;
    TEST_EXPECT_EQ(wait_pid_for(child_pid, WAIT_PACKED, &status), child_pid, "wait_pid pid mismatch");
    TEST_ASSERT(!g_xr_setup_failed, "setup: a new address space and a file to stand for the program");
    TEST_EXPECT_EQ(g_xr_ctl_rc, (s64)0, "control: the claim on a live Proc holds");
    TEST_ASSERT(g_xr_ctl_owned, "control: the claim names the caller");
    TEST_EXPECT_EQ(g_xr_ctl_as_ref, 2, "control: a held claim drops no reference to the space");
    TEST_EXPECT_EQ(g_xr_ctl_exe_ref, 2, "control: nor to the file");
    TEST_EXPECT_EQ(g_xr_rc, (s64)-T_E_AGAIN, "a claim on a dying Proc is refused with EAGAIN");
    TEST_EXPECT_EQ(g_xr_as_ref, 1, "the refusal dropped exec's reference to the new space");
    TEST_EXPECT_EQ(g_xr_exe_ref, 1, "and its reference to the program file");
    // proc_group_terminate records exit 1 for any message but "ok".
    TEST_EXPECT_EQ(status, WAIT_STATUS_EXITED(1), "the child died of its group exit");
}

// A user thread admitted before the claim and readied after it counts live
// until it dies, and it dies at the die-check its first entry takes, before
// any EL0 instruction. Were that die-check to miss the claim, the thread would
// fault at its unmapped entry and die there instead, since the fault path
// reads the claim too; the sabotage that turns this red takes the claim out of
// the die-check itself, which every EL0 return shares.
void test_proc_exec_claim_kills_a_thread_readied_after_it(void) {
    xc_reset();
    g_xc_unborn_wanted = true;
    g_xc_unborn_user   = true;
    int child_pid = rfork(RFPROC, xc_main_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    bool waited = xc_claim_held();
    // The runner owns the admitted thread once the child has claimed.
    if (waited && g_xc_unborn) {
        ready(g_xc_unborn);
        g_xc_unborn = NULL;
    }
    int status = xc_finish(child_pid);
    TEST_ASSERT(xc_phase() != XC_FAILED, "setup: an admitted user thread and a new address space");
    TEST_ASSERT(g_xc_held_in_place, "the admitted thread counted live");
    TEST_ASSERT(waited, "the claim slept while the admitted thread counted live");
    TEST_EXPECT_EQ(g_xc_claim_rc, 0, "the readied thread died and freed the claim");
    TEST_EXPECT_EQ(g_xc_live_at_claim, 1, "the execer was the only live thread");
    TEST_ASSERT(g_xc_cleared_at_swap, "the swap cleared the claim");
    TEST_EXPECT_EQ(g_xc_count_after, 1, "the new image starts alone");
    TEST_EXPECT_EQ(status, WAIT_STATUS_EXITED(0), "the child exited cleanly after the exec");
}

// A vfork parent parks, death-only, until its child gives the shared space
// back. An exec by another thread of the parent dooms it there: it leaves the
// park and dies, and the swap drops only the parent Proc's reference, so the
// child runs on in the old space until it lets go.
void test_proc_exec_claim_wakes_a_vfork_parent(void) {
    xc_reset();
    g_xc_npeers  = 1;
    g_xc_kind[0] = XC_VFORK;
    int child_pid = rfork(RFPROC, xc_main_entry, NULL);
    TEST_ASSERT(child_pid > 0, "rfork failed");
    int status = xc_finish(child_pid);
    TEST_ASSERT(xc_phase() != XC_FAILED, "setup: a vfork parent parked and a new address space");
    TEST_ASSERT(g_xc_held_in_place, "with no claim the vfork parent stayed parked");
    TEST_EXPECT_EQ(g_xc_claim_rc, 0, "the claim returned holding the Proc");
    TEST_EXPECT_EQ(g_xc_rc[0], 0, "the doomed vfork parent left its park");
    TEST_ASSERT(g_xc_vfork_kept, "the vfork child still runs in the old space after the swap");
    TEST_ASSERT(g_xc_vfork_reaped, "the new image reaped the vfork child");
    TEST_EXPECT_EQ(g_xc_count_after, 1, "the new image starts alone");
    TEST_EXPECT_EQ(status, WAIT_STATUS_EXITED(0), "the child exited cleanly after the exec");
}
