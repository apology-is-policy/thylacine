// The birth hold (DEBUG-FS-DESIGN 5f): a SPAWN_DEBUG_HELD spawn returns with
// its child parked before the child's first instruction; a debugger's stop
// takes the hold over, start or detach releases it, and a child whose spawner
// dies still holding it dies with the spawner.
//
//   birth_hold.validate_req            the ask: SPAWN_DEBUG_HELD accepted, an
//                                      unknown debug bit refused at both entries
//   birth_hold.publication_mark        a held child is born UNBORN; a plain
//                                      child, and the held child's own child,
//                                      are born NONE
//   birth_hold.released_predicate      spawn_birth_released's truth table
//   birth_hold.parked_wakes_birth_wait the PARKED mark wakes the spawner's
//                                      birth wait; a lost wake fails, bounded
//   birth_hold.orphan_rule             a held child dies "launcher exited" with
//                                      its spawner; a released one lives on
//   birth_hold.birth_wait_survives_latch
//                                      the spawner's own interrupt does not
//                                      return its birth wait: it sleeps on
//                                      until the child is born (5g)
//   birth_hold.held_spawn_parks        a real held spawn parks /hello on a
//                                      zeroed frame; release runs it; a
//                                      conversion keeps it parked at its entry
//                                      until the resume
//   birth_hold.held_spawn_death_wins   a kill at the birth park ends the child
//                                      there; an interrupt does not (5g), and
//                                      neither its wake nor a stop's reaches
//                                      the parked thread: the child stays
//                                      held, even on a frame whose SP note
//                                      delivery refuses, and dies of the note
//                                      once released
//
// The devproc half (stop converts, start and detach release, the implicit
// close keeps) is devproc.debug_birth_hold_ctl, beside the other ctl tests.

#include "test.h"

#include "../../arch/arm64/exception.h"   // struct exception_context: the birth frame

#include <thylacine/caps.h>
#include <thylacine/notes.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

struct spawn_allowance;
extern int sys_spawn_full_argv_validate_req(const struct sys_spawn_args *req);
extern int sys_spawn_full_argv_debug_for_proc(
        struct Proc *p, const char *name, size_t name_len,
        const char *argv_data, u32 argv_data_len, u32 argc,
        const u32 *fds, u32 fd_count,
        caps_t cap_mask, u32 perm_flags,
        bool set_identity, u32 principal_id, u32 primary_gid,
        const u32 *supp_gids, u32 supp_gid_count,
        const struct spawn_allowance *want_allowance,
        u32 req_budget, u32 pheno_flags, u32 debug_flags);
bool devproc_all_threads_parked(struct Proc *target);

void test_birth_hold_validate_req(void);
void test_birth_hold_publication_mark(void);
void test_birth_hold_released_predicate(void);
void test_birth_hold_parked_wakes_birth_wait(void);
void test_birth_hold_orphan_rule(void);
void test_birth_hold_birth_wait_survives_latch(void);
void test_birth_hold_held_spawn_parks(void);
void test_birth_hold_held_spawn_death_wins(void);

// Not a mark value: a child that never ran must not read as NONE (0), or every
// "born NONE" control would pass on a child that did nothing.
#define BH_UNSEEN 0xDEADu

#define BH_BUDGET_NS (3ull * 1000ull * 1000ull * 1000ull)

// Long enough for a thread spinning on a checkpoint to be switched in again
// many times over: the scheduler's tick is milliseconds.
#define BH_QUIET_NS (100ull * 1000ull * 1000ull)

// A child waits against a wall-clock budget and reports what it saw, because a
// kernel thunk cannot TEST_ASSERT: the macro returns, and an rfork entry that
// returns halts in thread_trampoline's wfe instead of exiting.
static bool bh_wait(bool (*pred)(void *), void *arg, u64 budget_ns) {
    u64 deadline = timer_now_ns() + budget_ns;
    while (!pred(arg)) {
        if (timer_now_ns() >= deadline) return false;
        sched();
    }
    return true;
}

static u32 bh_hold_of(const struct Proc *p) {
    return __atomic_load_n(&p->debug_birth_hold, __ATOMIC_ACQUIRE);
}

static bool bh_str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// =============================================================================
// birth_hold.validate_req
// =============================================================================

void test_birth_hold_validate_req(void) {
    struct sys_spawn_args req = {
        .name_va     = 0x1000,
        .name_len    = 5,
        .debug_flags = SPAWN_DEBUG_HELD,
    };
    TEST_EXPECT_EQ(sys_spawn_full_argv_validate_req(&req), 0,
        "debug_flags = SPAWN_DEBUG_HELD accepted (the ask is ungated)");
    req.debug_flags = 0;
    TEST_EXPECT_EQ(sys_spawn_full_argv_validate_req(&req), 0,
        "debug_flags = 0 accepted -- a zero-filled pre-5f req is not held");
    req.debug_flags = SPAWN_DEBUG_FLAGS_ALL | (1u << 1);
    TEST_EXPECT_EQ(sys_spawn_full_argv_validate_req(&req), -1,
        "an unknown debug bit beside the known one is refused");
    req.debug_flags = 1u << 31;
    TEST_EXPECT_EQ(sys_spawn_full_argv_validate_req(&req), -1,
        "the top debug bit alone is refused");

    // The kernel-internal entry refuses on its own, before any fork: the
    // handler's validator is not the only thing between a kernel caller and a
    // flag this kernel does not know.
    int pid = sys_spawn_full_argv_debug_for_proc(current_thread()->proc,
            "hello", 5, NULL, 0u, 0u, NULL, 0u, CAP_NONE, 0u,
            false, 0u, 0u, NULL, 0u, NULL, 0u, 0u, 1u << 5);
    if (pid > 0) {
        int st = -1;
        (void)wait_pid_for(pid, 0, &st);
    }
    TEST_EXPECT_EQ(pid, -1, "the spawn body refuses an unknown debug bit");
}

// =============================================================================
// birth_hold.publication_mark
// =============================================================================

static struct {
    u32 held_saw;    // the held child's mark, read from inside it
    u32 plain_saw;   // a plain child's mark, read from inside it
    u32 grand_saw;   // the held child's own child's mark
    int grand_pid;
} g_bh_mark;

static void bh_mark_grand_thunk(void *arg) {
    (void)arg;
    g_bh_mark.grand_saw = bh_hold_of(current_thread()->proc);
    exits("ok");
}

static void bh_mark_held_thunk(void *arg) {
    (void)arg;
    g_bh_mark.held_saw = bh_hold_of(current_thread()->proc);
    int gpid = rfork(RFPROC, bh_mark_grand_thunk, NULL);
    g_bh_mark.grand_pid = gpid;
    if (gpid > 0) {
        int st = -1;
        (void)wait_pid_for(gpid, 0, &st);
    }
    exits("ok");
}

static void bh_mark_plain_thunk(void *arg) {
    (void)arg;
    g_bh_mark.plain_saw = bh_hold_of(current_thread()->proc);
    exits("ok");
}

// The mark is written in the lock hold that publishes the child, so the child's
// first read -- its own thread's first action -- already sees it: the birth park
// can never find a held child unmarked.
void test_birth_hold_publication_mark(void) {
    g_bh_mark.held_saw  = BH_UNSEEN;
    g_bh_mark.plain_saw = BH_UNSEEN;
    g_bh_mark.grand_saw = BH_UNSEEN;
    g_bh_mark.grand_pid = 0;

    int hpid = rfork_spawn_held(bh_mark_held_thunk, NULL, CAP_NONE);
    int hst = -1;
    int hreaped = (hpid > 0) ? wait_pid_for(hpid, 0, &hst) : -1;
    int ppid = rfork_with_caps(RFPROC, bh_mark_plain_thunk, NULL, CAP_NONE);
    int pst = -1;
    int preaped = (ppid > 0) ? wait_pid_for(ppid, 0, &pst) : -1;

    TEST_ASSERT(hpid > 0 && hreaped == hpid, "the held child spawned and was reaped");
    TEST_ASSERT(ppid > 0 && preaped == ppid, "the plain child spawned and was reaped");
    TEST_EXPECT_EQ(g_bh_mark.held_saw, BIRTH_HOLD_UNBORN,
        "a held child is born UNBORN");
    TEST_EXPECT_EQ(g_bh_mark.plain_saw, BIRTH_HOLD_NONE,
        "a plain child is born NONE");
    TEST_ASSERT(g_bh_mark.grand_pid > 0, "the held child's own rfork succeeded");
    TEST_EXPECT_EQ(g_bh_mark.grand_saw, BIRTH_HOLD_NONE,
        "the hold is never inherited: the held child's own child is born NONE");
}

// =============================================================================
// birth_hold.released_predicate
// =============================================================================

void test_birth_hold_released_predicate(void) {
    struct Proc *parent = kproc();
    TEST_ASSERT(spawn_birth_released(parent, NULL),
        "a child gone from the list releases the spawner (never hang it)");

    struct Proc *c = proc_alloc();
    TEST_ASSERT(c != NULL, "alloc a synthetic child");
    c->state = PROC_STATE_ALIVE;
    __atomic_store_n(&c->debug_birth_hold, BIRTH_HOLD_UNBORN, __ATOMIC_RELEASE);
    bool unborn = spawn_birth_released(parent, c);
    __atomic_store_n(&c->debug_birth_hold, BIRTH_HOLD_PARKED, __ATOMIC_RELEASE);
    bool parked = spawn_birth_released(parent, c);
    __atomic_store_n(&c->debug_birth_hold, BIRTH_HOLD_NONE, __ATOMIC_RELEASE);
    bool released = spawn_birth_released(parent, c);
    __atomic_store_n(&c->debug_birth_hold, BIRTH_HOLD_UNBORN, __ATOMIC_RELEASE);
    c->state = PROC_STATE_ZOMBIE;
    bool dead = spawn_birth_released(parent, c);
    __atomic_store_n(&c->debug_birth_hold, BIRTH_HOLD_NONE, __ATOMIC_RELEASE);
    proc_free(c);

    TEST_ASSERT(!unborn, "an ALIVE, UNBORN child holds the spawner");
    TEST_ASSERT(parked, "a child at its birth park releases the spawner");
    TEST_ASSERT(released, "a child whose hold was released releases the spawner");
    TEST_ASSERT(dead, "a child no longer ALIVE releases the spawner, hold or not");
}

// =============================================================================
// birth_hold.parked_wakes_birth_wait
// =============================================================================

static struct {
    struct Thread *waiter;    // the test thread
    u32 awaiting;             // the test is entering spawn_await_birth
    u32 returned;             // spawn_await_birth returned
    u32 waiter_slept;         // the child saw the waiter asleep before marking
    u32 saw_return;           // the child saw the wait return while it lived
} g_bh_wake;

static bool bh_waiter_asleep(void *arg) {
    (void)arg;
    return __atomic_load_n(&g_bh_wake.awaiting, __ATOMIC_ACQUIRE) != 0 &&
           __atomic_load_n(&g_bh_wake.waiter->state, __ATOMIC_ACQUIRE) ==
               THREAD_SLEEPING;
}

static bool bh_wait_returned(void *arg) {
    (void)arg;
    return __atomic_load_n(&g_bh_wake.returned, __ATOMIC_ACQUIRE) != 0;
}

static void bh_wake_thunk(void *arg) {
    (void)arg;
    g_bh_wake.waiter_slept = bh_wait(bh_waiter_asleep, NULL, BH_BUDGET_NS);
    irq_state_t s = proc_table_lock_acquire();
    proc_birth_hold_mark_parked_locked(current_thread()->proc);
    proc_table_lock_release(s);
    g_bh_wake.saw_return = bh_wait(bh_wait_returned, NULL, BH_BUDGET_NS);
    exits("ok");
}

// The child marks itself PARKED only once the spawner is asleep in the birth
// wait, and then stays alive waiting to see that wait return. The mark's wake
// releases the spawner at once. A lost wake leaves the spawner asleep until the
// child gives up and exits, and its death wakes the wait -- too late for the
// child to have seen it.
void test_birth_hold_parked_wakes_birth_wait(void) {
    g_bh_wake.waiter       = current_thread();
    g_bh_wake.awaiting     = 0;
    g_bh_wake.returned     = 0;
    g_bh_wake.waiter_slept = 0;
    g_bh_wake.saw_return   = 0;

    int pid = rfork_spawn_held(bh_wake_thunk, NULL, CAP_NONE);
    int reaped = -1;
    if (pid > 0) {
        __atomic_store_n(&g_bh_wake.awaiting, 1u, __ATOMIC_RELEASE);
        spawn_await_birth(current_thread()->proc, pid);
        __atomic_store_n(&g_bh_wake.returned, 1u, __ATOMIC_RELEASE);
        int st = -1;
        reaped = wait_pid_for(pid, 0, &st);
    }

    TEST_ASSERT(pid > 0 && reaped == pid, "the held child spawned and was reaped");
    TEST_ASSERT(g_bh_wake.waiter_slept,
        "premise: the spawner was asleep in its birth wait before the mark");
    TEST_ASSERT(g_bh_wake.saw_return,
        "the PARKED mark woke the birth wait while the child lived");
}

// =============================================================================
// birth_hold.orphan_rule
// =============================================================================

struct bh_orphan {
    u32          release_first;   // the launcher releases the hold before exiting
    struct Proc *launcher;        // written before the grandchild is forked
    int          gpid;            // the held grandchild
    u32          settled;         // it saw its launcher gone, or its own death
    u32          hold;            // its mark when it settled
    char         msg[32];         // its group_exit_msg when it settled ("" if none)
};
static struct bh_orphan g_bh_orphan;

static bool bh_orphan_settled(void *arg) {
    struct bh_orphan *o = (struct bh_orphan *)arg;
    struct Proc *self = current_thread()->proc;
    irq_state_t s = proc_table_lock_acquire();
    bool settled =
        __atomic_load_n(&self->group_exit_msg, __ATOMIC_ACQUIRE) != NULL ||
        self->parent != o->launcher;
    proc_table_lock_release(s);
    return settled;
}

static void bh_orphan_grand_thunk(void *arg) {
    struct bh_orphan *o = (struct bh_orphan *)arg;
    struct Proc *self = current_thread()->proc;
    o->settled = bh_wait(bh_orphan_settled, o, BH_BUDGET_NS);
    // One snapshot under the lock: the orphan rule and the reparent run in one
    // hold of it, so a grandchild that sees itself adopted without a message
    // was never going to get one.
    irq_state_t s = proc_table_lock_acquire();
    const char *m = __atomic_load_n(&self->group_exit_msg, __ATOMIC_ACQUIRE);
    o->hold = bh_hold_of(self);
    proc_table_lock_release(s);
    size_t i = 0;
    if (m) {
        for (; m[i] && i + 1 < sizeof(o->msg); i++)
            o->msg[i] = m[i];
    }
    o->msg[i] = '\0';
    exits("ok");
}

static void bh_orphan_launcher_thunk(void *arg) {
    struct bh_orphan *o = (struct bh_orphan *)arg;
    struct Proc *self = current_thread()->proc;
    o->launcher = self;
    int gpid = rfork_spawn_held(bh_orphan_grand_thunk, o, CAP_NONE);
    __atomic_store_n(&o->gpid, gpid, __ATOMIC_RELEASE);
    if (gpid > 0 && o->release_first) {
        irq_state_t s = proc_table_lock_acquire();
        for (struct Proc *c = self->children; c; c = c->sibling) {
            if (c->pid == gpid)
                proc_birth_hold_release_locked(c);
        }
        proc_table_lock_release(s);
    }
    exits("ok");
}

// The launcher is reaped first, so its orphans are already kproc's (init is not
// up during the kernel tests) by the time the grandchild is polled for.
static int bh_reap_bounded(int pid, int *st) {
    if (pid <= 0) return -1;
    u64 deadline = timer_now_ns() + BH_BUDGET_NS + BH_BUDGET_NS;
    for (;;) {
        int s = -1;
        int r = wait_pid_for(pid, WAIT_WNOHANG, &s);
        if (r != 0) {
            if (st) *st = s;
            return r;
        }
        if (timer_now_ns() >= deadline) return 0;
        sched();
    }
}

static void bh_orphan_run(u32 release_first, int *lpid_out, int *gpid_out,
                          int *greaped_out) {
    g_bh_orphan.release_first = release_first;
    g_bh_orphan.launcher      = NULL;
    g_bh_orphan.gpid          = 0;
    g_bh_orphan.settled       = 0;
    g_bh_orphan.hold          = BH_UNSEEN;
    g_bh_orphan.msg[0]        = '?';
    g_bh_orphan.msg[1]        = '\0';

    int lpid = rfork(RFPROC, bh_orphan_launcher_thunk, &g_bh_orphan);
    if (lpid > 0) {
        int lst = -1;
        (void)wait_pid_for(lpid, 0, &lst);
    }
    int gpid = __atomic_load_n(&g_bh_orphan.gpid, __ATOMIC_ACQUIRE);
    *lpid_out    = lpid;
    *gpid_out    = gpid;
    *greaped_out = bh_reap_bounded(gpid, NULL);
}

void test_birth_hold_orphan_rule(void) {
    int lpid = 0, gpid = 0, greaped = 0;

    // Held when the launcher dies: the rule kills it (the operator's vote).
    bh_orphan_run(0u, &lpid, &gpid, &greaped);
    TEST_ASSERT(lpid > 0, "the launcher spawned");
    TEST_ASSERT(gpid > 0, "the launcher's held spawn succeeded");
    TEST_EXPECT_EQ(greaped, gpid, "the orphaned held child was reaped by its adopter");
    TEST_ASSERT(g_bh_orphan.settled, "the held child settled inside its budget");
    TEST_ASSERT(bh_str_eq(g_bh_orphan.msg, "launcher exited"),
        "a child still held when its launcher dies dies \"launcher exited\"");

    // Control: released before the launcher dies, the child is no one's hold,
    // and it outlives the launcher.
    bh_orphan_run(1u, &lpid, &gpid, &greaped);
    TEST_ASSERT(lpid > 0, "the control launcher spawned");
    TEST_ASSERT(gpid > 0, "the control launcher's held spawn succeeded");
    TEST_EXPECT_EQ(greaped, gpid, "the control child was reaped by its adopter");
    TEST_ASSERT(g_bh_orphan.settled, "the control child saw its launcher gone");
    TEST_EXPECT_EQ(g_bh_orphan.hold, BIRTH_HOLD_NONE, "the control child's hold was released");
    TEST_ASSERT(bh_str_eq(g_bh_orphan.msg, ""),
        "a released child outlives its launcher (no group_exit_msg)");
}

// =============================================================================
// birth_hold.birth_wait_survives_latch
// =============================================================================

struct bh_latch {
    struct Proc   *launcher;         // the spawner, written before the fork
    struct Thread *launcher_thread;
    int            gpid;             // the held grandchild
    u32            awaiting;         // the launcher is entering spawn_await_birth
    u32            returned;         // spawn_await_birth returned
    u32            gdone;            // the grandchild has stopped reading the launcher
    u64            since;            // the launcher's dispatch count before the wake
    u32            asleep;           // premise: it slept in the wait before the post
    u32            latched;          // premise: the post armed its terminate latch
    u32            settled;          // it ran on the wake, then slept again or returned
    u32            early;            // it had returned with the child still unborn
    u32            saw_return;       // the child's release then returned it
};
static struct bh_latch g_bh_latch;

static bool bh_latch_asleep(void *arg) {
    struct bh_latch *l = (struct bh_latch *)arg;
    return __atomic_load_n(&l->awaiting, __ATOMIC_ACQUIRE) != 0 &&
           __atomic_load_n(&l->launcher_thread->state, __ATOMIC_ACQUIRE) ==
               THREAD_SLEEPING;
}

// A woken thread is switched in to run, so its dispatch count (nsched, stored
// atomically at every switch-in for cross-thread readers) moves; its state
// alone would read SLEEPING just the same had the wake never reached it.
static bool bh_latch_settled(void *arg) {
    struct bh_latch *l = (struct bh_latch *)arg;
    if (__atomic_load_n(&l->returned, __ATOMIC_ACQUIRE) != 0)
        return true;
    return __atomic_load_n(&l->launcher_thread->nsched, __ATOMIC_RELAXED) !=
               l->since &&
           __atomic_load_n(&l->launcher_thread->state, __ATOMIC_ACQUIRE) ==
               THREAD_SLEEPING;
}

static bool bh_latch_returned(void *arg) {
    struct bh_latch *l = (struct bh_latch *)arg;
    return __atomic_load_n(&l->returned, __ATOMIC_ACQUIRE) != 0;
}

static bool bh_latch_gdone(void *arg) {
    struct bh_latch *l = (struct bh_latch *)arg;
    return __atomic_load_n(&l->gdone, __ATOMIC_ACQUIRE) != 0;
}

// Every read of the launcher's thread happens before gdone is set, and the
// launcher does not exit until it sees gdone (or its own, longer, budget ends).
static void bh_latch_grand_thunk(void *arg) {
    struct bh_latch *l = (struct bh_latch *)arg;
    l->asleep = bh_wait(bh_latch_asleep, l, BH_BUDGET_NS);
    if (l->asleep) {
        l->since = __atomic_load_n(&l->launcher_thread->nsched, __ATOMIC_RELAXED);
        l->latched = notes_post(l->launcher, "interrupt", 0u, NULL, true) == 0 &&
                     proc_intr_terminate_pending(l->launcher);
        irq_state_t s = proc_table_lock_acquire();
        proc_interrupt_terminate_wake(l->launcher);
        proc_table_lock_release(s);
        l->settled = bh_wait(bh_latch_settled, l, BH_BUDGET_NS);
    }
    l->early = __atomic_load_n(&l->returned, __ATOMIC_ACQUIRE) != 0;
    irq_state_t s = proc_table_lock_acquire();
    proc_birth_hold_release_locked(current_thread()->proc);
    proc_table_lock_release(s);
    l->saw_return = bh_wait(bh_latch_returned, l, BH_BUDGET_NS);
    __atomic_store_n(&l->gdone, 1u, __ATOMIC_RELEASE);
    exits("ok");
}

static void bh_latch_launcher_thunk(void *arg) {
    struct bh_latch *l = (struct bh_latch *)arg;
    struct Proc *self = current_thread()->proc;
    l->launcher        = self;
    l->launcher_thread = current_thread();
    int gpid = rfork_spawn_held(bh_latch_grand_thunk, l, CAP_NONE);
    __atomic_store_n(&l->gpid, gpid, __ATOMIC_RELEASE);
    if (gpid > 0) {
        __atomic_store_n(&l->awaiting, 1u, __ATOMIC_RELEASE);
        spawn_await_birth(self, gpid);
        __atomic_store_n(&l->returned, 1u, __ATOMIC_RELEASE);
        (void)bh_wait(bh_latch_gdone, l, 4u * BH_BUDGET_NS);
    }
    exits("ok");
}

// The spawner's own interrupt lands while it sleeps in the birth wait. The wait
// is death-only (DEBUG-FS-DESIGN 5g): the latch's wake is absorbed and the
// spawner sleeps on until the child is born; a wait that returned for it would
// hand its caller a pid whose child is still loading. The spawner is a Proc of
// its own, because kproc, the test thread's, never latches. Its held child
// interrupts it, watches it settle, and only then releases itself.
void test_birth_hold_birth_wait_survives_latch(void) {
    struct bh_latch *l = &g_bh_latch;
    l->launcher        = NULL;
    l->launcher_thread = NULL;
    l->gpid            = 0;
    l->awaiting        = 0;
    l->returned        = 0;
    l->gdone           = 0;
    l->since           = 0;
    l->asleep          = 0;
    l->latched         = 0;
    l->settled         = 0;
    l->early           = 1;   // a grandchild that never ran reads as the failure
    l->saw_return      = 0;

    int lpid = rfork(RFPROC, bh_latch_launcher_thunk, l);
    int lreaped = -1;
    if (lpid > 0) {
        int lst = -1;
        lreaped = wait_pid_for(lpid, 0, &lst);
    }
    int gpid = __atomic_load_n(&l->gpid, __ATOMIC_ACQUIRE);
    int greaped = bh_reap_bounded(gpid, NULL);

    TEST_ASSERT(lpid > 0 && lreaped == lpid, "the launcher spawned and was reaped");
    TEST_ASSERT(gpid > 0, "the launcher's held spawn succeeded");
    TEST_EXPECT_EQ(greaped, gpid, "the held child was reaped by its adopter");
    TEST_ASSERT(l->asleep,
        "premise: the launcher slept in its birth wait before the post");
    TEST_ASSERT(l->latched, "premise: the post armed the launcher's terminate latch");
    TEST_ASSERT(l->settled,
        "the launcher ran on the latch's wake, then slept again or returned, "
        "inside the budget");
    TEST_ASSERT(!l->early,
        "the latch did not return the birth wait: the launcher slept on with "
        "its child unborn");
    TEST_ASSERT(l->saw_return, "the child's release returned the birth wait");
}

// =============================================================================
// birth_hold.held_spawn_parks + birth_hold.held_spawn_death_wins
// =============================================================================

static int bh_spawn_hello_held(void) {
    return sys_spawn_full_argv_debug_for_proc(current_thread()->proc,
            "hello", 5, NULL, 0u, 0u, NULL, 0u, CAP_NONE, 0u,
            false, 0u, 0u, NULL, 0u, NULL, 0u, 0u, SPAWN_DEBUG_HELD);
}

static bool bh_parked_pred(void *arg) {
    return devproc_all_threads_parked((struct Proc *)arg);
}

// What the frame must hold before the child's first instruction: userland_enter's
// SPSR (EL0t, DAIF clear), a user-half PC and SP, and every GPR zero.
static bool bh_frame_is_birth(const struct exception_context *f) {
    if (!f) return false;
    if (f->spsr != 0 || f->elr == 0 || (f->elr >> 48) != 0 ||
        f->sp == 0 || (f->sp >> 48) != 0)
        return false;
    for (int i = 0; i < 31; i++) {
        if (f->regs[i] != 0) return false;
    }
    return true;
}

static void bh_release_locked_pair(struct Proc *c) {
    // The start verb's order: clear the hold, then the resume's wake.
    irq_state_t s = proc_table_lock_acquire();
    proc_birth_hold_release_locked(c);
    proc_debug_resume(c);
    proc_table_lock_release(s);
}

// Read a SETTLED child's head thread and its published frame. Under the lock
// and only while the child is ALIVE: on a failing run the child may have run
// and exited, and its thread would then be no longer ours to read.
static void bh_inspect_parked(struct Proc *c, bool *one_thread, bool *birth,
                              const struct exception_context **frame, u64 *elr) {
    irq_state_t s = proc_table_lock_acquire();
    if (c->state == PROC_STATE_ALIVE) {
        struct Thread *th = c->threads;
        const struct exception_context *f = th ? th->debug_trapframe : NULL;
        if (one_thread) *one_thread = th && th->next_in_proc == NULL;
        if (birth)      *birth = bh_frame_is_birth(f);
        if (frame)      *frame = f;
        if (elr)        *elr = f ? f->elr : 0;
    }
    proc_table_lock_release(s);
}

// Reap within the budget; a child still unreaped then is killed and polled for
// once more, so a failing leg reports instead of hanging the boot. *killed says
// the fallback was needed: a leg whose own action should have ended the child
// asserts it was not.
static int bh_reap_or_kill(int pid, struct Proc *c, int *st, bool *killed) {
    *killed = false;
    int r = bh_reap_bounded(pid, st);
    if (r != 0) return r;
    *killed = true;
    irq_state_t s = proc_table_lock_acquire();
    if (c->state == PROC_STATE_ALIVE)
        proc_group_terminate(c, "killed");
    proc_table_lock_release(s);
    return bh_reap_bounded(pid, st);
}

static bool bh_zombie_pred(void *arg) {
    struct Proc *c = (struct Proc *)arg;
    irq_state_t s = proc_table_lock_acquire();
    bool zombie = c->state == PROC_STATE_ZOMBIE;
    proc_table_lock_release(s);
    return zombie;
}

// bh_reap_or_kill, recording the exit message first: it lives in the ZOMBIE,
// which the reap frees. "" when the child did not become a ZOMBIE on its own
// inside the budget.
static int bh_reap_with_msg(int pid, struct Proc *c, int *st, bool *killed,
                            char *msg, size_t msg_len) {
    msg[0] = '\0';
    if (bh_wait(bh_zombie_pred, c, BH_BUDGET_NS)) {
        irq_state_t s = proc_table_lock_acquire();
        const char *m = (c->state == PROC_STATE_ZOMBIE) ? c->exit_msg : NULL;
        size_t i = 0;
        if (m) {
            for (; m[i] && i + 1 < msg_len; i++)
                msg[i] = m[i];
        }
        msg[i] = '\0';
        proc_table_lock_release(s);
    }
    return bh_reap_or_kill(pid, c, st, killed);
}

void test_birth_hold_held_spawn_parks(void) {
    // (a) The spawn returns with the child parked; a release runs it.
    int pid = bh_spawn_hello_held();
    struct Proc *c = (pid > 0) ? proc_find_by_pid(pid) : NULL;
    u32 hold = c ? bh_hold_of(c) : BH_UNSEEN;
    bool settled = c && bh_wait(bh_parked_pred, c, BH_BUDGET_NS);
    bool one_thread = false, birth = false;
    if (settled)
        bh_inspect_parked(c, &one_thread, &birth, NULL, NULL);
    u32 stop_req = c ? (u32)__atomic_load_n(&c->debug_stop_req, __ATOMIC_ACQUIRE) : 1u;
    int st = -1, reaped = -1;
    bool killed = false;
    if (c) {
        bh_release_locked_pair(c);
        reaped = bh_reap_or_kill(pid, c, &st, &killed);
    }
    TEST_ASSERT(pid > 0 && c != NULL, "a held spawn of /hello returned a live child");
    TEST_EXPECT_EQ(hold, BIRTH_HOLD_PARKED,
        "the held spawn returned only once the child reached its birth park");
    TEST_ASSERT(settled, "the head thread settled on its own debug_rendez");
    TEST_ASSERT(one_thread, "the held child has exactly its head thread");
    TEST_ASSERT(birth, "the published frame is the zeroed first-entry frame");
    TEST_EXPECT_EQ(stop_req, 0u,
        "no debug stop is pending: the hold alone keeps the child, so the "
        "stopped-only surface stays closed, and the stop scan, which wants the "
        "flag as well as the parked threads settled above, does not report it "
        "stopped until a debugger's stop");
    TEST_EXPECT_EQ(reaped, pid, "the released child ran and was reaped");
    TEST_ASSERT(!killed, "the released child ran without the fallback kill");
    TEST_EXPECT_EQ(st, 0, "the released child ran /hello to a clean exit");

    // (b) A conversion keeps the child parked at its entry until the resume: the
    //     stop is delivered, then the hold is converted.
    pid = bh_spawn_hello_held();
    c  = (pid > 0) ? proc_find_by_pid(pid) : NULL;
    bool settled1 = c && bh_wait(bh_parked_pred, c, BH_BUDGET_NS);
    const struct exception_context *f1 = NULL, *f2 = NULL;
    u64 entry1 = 0, entry2 = 0;
    if (settled1)
        bh_inspect_parked(c, NULL, NULL, &f1, &entry1);
    bool settled2 = false;
    u32 hold2 = BH_UNSEEN, stop2 = 0;
    st = -1;
    reaped = -1;
    killed = false;
    if (c) {
        irq_state_t s = proc_table_lock_acquire();
        proc_debug_stop_deliver(c);
        proc_birth_hold_convert_locked(c);
        proc_table_lock_release(s);
        // The delivery passes the parked thread by -- a stop park is woken by a
        // resume or by death alone -- and the thread stays at its birth tail:
        // settled, with its stopped PC still the entry.
        settled2 = bh_wait(bh_parked_pred, c, BH_BUDGET_NS);
        if (settled2)
            bh_inspect_parked(c, NULL, NULL, &f2, &entry2);
        hold2 = bh_hold_of(c);
        stop2 = (u32)__atomic_load_n(&c->debug_stop_req, __ATOMIC_ACQUIRE);
        s = proc_table_lock_acquire();
        proc_debug_resume(c);
        proc_table_lock_release(s);
        reaped = bh_reap_or_kill(pid, c, &st, &killed);
    }
    // The frame ADDRESS is no evidence: every EL0 entry of this thread lands
    // its frame where the birth frame was, so f2 == f1 would hold after any
    // escape. The PC is: a child that ran would stop past its entry. It cannot
    // see an interrupt taken before the first instruction retires; the
    // debug-probe held phase's entry breakpoint, which fires only on an entry
    // not yet executed, is the check for that.
    bool at_entry = f1 != NULL && f2 != NULL && entry1 != 0 && entry2 == entry1;
    TEST_ASSERT(pid > 0 && c != NULL, "a second held spawn returned a live child");
    TEST_ASSERT(settled1, "the second child settled at its birth park");
    TEST_ASSERT(settled2, "the converted child is parked again after the delivery's wake");
    TEST_ASSERT(at_entry, "the converted child's stopped PC is still its entry");
    TEST_EXPECT_EQ(hold2, BIRTH_HOLD_NONE, "the conversion cleared the hold");
    TEST_EXPECT_EQ(stop2, 1u, "the conversion left the debug stop in its place");
    TEST_EXPECT_EQ(reaped, pid, "the resumed child ran and was reaped");
    TEST_ASSERT(!killed, "the resumed child ran without the fallback kill");
    TEST_EXPECT_EQ(st, 0, "the resumed child ran /hello to a clean exit");
}

// ALIVE and not dying: a group terminate publishes its message at once, while
// the state reads ALIVE until the last thread has gone.
static bool bh_alive(struct Proc *c) {
    irq_state_t s = proc_table_lock_acquire();
    bool alive = c->state == PROC_STATE_ALIVE &&
                 __atomic_load_n(&c->group_exit_msg, __ATOMIC_ACQUIRE) == NULL;
    proc_table_lock_release(s);
    return alive;
}

// The head thread asleep on its own debug_rendez, with its dispatch count
// (nsched), read under the lock while the child is ALIVE. Positive where
// devproc_all_threads_parked is not: that predicate passes over a dying
// thread, so it holds for a child already on its way out.
static bool bh_head_parked(struct Proc *c, u64 *nsched) {
    irq_state_t s = proc_table_lock_acquire();
    struct Thread *th = (c->state == PROC_STATE_ALIVE) ? c->threads : NULL;
    bool parked = false;
    if (th) {
        irq_state_t ws = spin_lock_irqsave(&th->wait_lock);
        parked = __atomic_load_n(&th->state, __ATOMIC_ACQUIRE) == THREAD_SLEEPING &&
                 th->rendez_blocked_on == &th->debug_rendez;
        spin_unlock_irqrestore(&th->wait_lock, ws);
        *nsched = __atomic_load_n(&th->nsched, __ATOMIC_RELAXED);
    }
    proc_table_lock_release(s);
    return parked;
}

// The head thread is parked at the end of the window and was never switched in
// after its dispatch count read `since`. A woken thread is readied at once and
// switched in within the window, so the count moves; its state alone would read
// the same had it run and parked again.
static bool bh_head_quiet_since(struct Proc *c, u64 since, u64 window_ns) {
    u64 deadline = timer_now_ns() + window_ns;
    while (timer_now_ns() < deadline)
        sched();
    u64 at = since;
    return bh_head_parked(c, &at) && at == since;
}

void test_birth_hold_held_spawn_death_wins(void) {
    // (a) A kill at the birth park: the park's death check ends the thread.
    //     /hello would exit 0 had it run, so a nonzero status is the kill's.
    int pid = bh_spawn_hello_held();
    struct Proc *c = (pid > 0) ? proc_find_by_pid(pid) : NULL;
    bool settled = c && bh_wait(bh_parked_pred, c, BH_BUDGET_NS);
    int st = -1, reaped = -1;
    bool killed = false;
    if (c) {
        // Only a child still ALIVE: on a failing run it may already have run
        // /hello to its exit, and the kill has nothing to act on.
        irq_state_t s = proc_table_lock_acquire();
        if (c->state == PROC_STATE_ALIVE)
            proc_group_terminate(c, "killed");
        proc_table_lock_release(s);
        reaped = bh_reap_or_kill(pid, c, &st, &killed);
    }
    TEST_ASSERT(pid > 0 && c != NULL, "a held spawn returned a live child");
    TEST_ASSERT(settled, "the child settled at its birth park before the kill");
    TEST_EXPECT_EQ(reaped, pid, "the killed child was reaped");
    TEST_ASSERT(!killed, "the kill ended the child inside the budget");
    TEST_ASSERT(st != 0, "the child died of the kill at its birth park");

    // (b) An interrupt at the birth park does not end the child (5g), and its
    //     wake passes the parked thread by: a stop park is woken by a resume
    //     or by death alone. The thread stays parked, never switched in, and
    //     the child stays held. Released, the child runs /hello and meets the
    //     note at its first checkpoint -- the return of libt _start's first
    //     syscall, SYS_NOTE_MASK -- and dies of it there.
    pid = bh_spawn_hello_held();
    c = (pid > 0) ? proc_find_by_pid(pid) : NULL;
    settled = c && bh_wait(bh_parked_pred, c, BH_BUDGET_NS);
    bool stamped = false, latched = false, alive = false, quiet = false;
    u64 since = 0;
    u32 hold = BH_UNSEEN;
    int posted = -1;
    char msg[32] = "";
    st = -1;
    reaped = -1;
    killed = false;
    if (c) {
        stamped = settled && bh_head_parked(c, &since);
        if (stamped)
            posted = notes_post(c, "interrupt", 0u, NULL, true);
        // The walk wakes only a Proc whose latch is armed, and a queued note
        // need not arm it: unarmed, the quiet verdict below would be vacuous.
        latched = posted == 0 && proc_intr_terminate_pending(c);
        if (latched) {
            irq_state_t s = proc_table_lock_acquire();
            proc_interrupt_terminate_wake(c);
            proc_table_lock_release(s);
            quiet = bh_head_quiet_since(c, since, BH_QUIET_NS);
            alive = bh_alive(c);
            hold  = bh_hold_of(c);
        }
        if (quiet) {
            bh_release_locked_pair(c);
        } else {
            irq_state_t s = proc_table_lock_acquire();
            if (c->state == PROC_STATE_ALIVE)
                proc_group_terminate(c, "killed");   // never strand the child
            proc_table_lock_release(s);
        }
        reaped = bh_reap_with_msg(pid, c, &st, &killed, msg, sizeof(msg));
    }
    TEST_ASSERT(pid > 0 && c != NULL, "a held spawn returned a live child");
    TEST_ASSERT(settled, "the child settled at its birth park before the interrupt");
    TEST_ASSERT(stamped, "the settled child's thread sleeps on its debug_rendez");
    TEST_EXPECT_EQ(posted, 0, "the interrupt was posted");
    TEST_ASSERT(latched, "premise: the post armed the terminate latch");
    TEST_ASSERT(alive, "the interrupt did not end the held child");
    TEST_ASSERT(quiet,
        "the latch's wake passed the parked thread by: it stayed parked, never "
        "switched in");
    TEST_EXPECT_EQ(hold, BIRTH_HOLD_PARKED, "the child is still held");
    TEST_EXPECT_EQ(reaped, pid, "the released child was reaped");
    TEST_ASSERT(!killed, "the note ended the released child without the fallback kill");
    TEST_ASSERT(st != 0, "the released child died rather than exit cleanly");
    TEST_ASSERT(bh_str_eq(msg, "interrupt"),
        "released, the child met the note at its first checkpoint and died of "
        "the interrupt");

    // (c) The same on a frame note delivery will not touch. The child is
    //     converted (a debug stop delivered, the hold taken over), and then its
    //     birth frame's SP is written as 0, which a debugger's regs write may
    //     do. Neither the stop nor the interrupt wakes the parked thread, so it
    //     stays parked, never switched in, with nothing re-running a checkpoint
    //     it cannot pass -- which would hang a one-CPU machine. A kill still
    //     ends it there.
    pid = bh_spawn_hello_held();
    c = (pid > 0) ? proc_find_by_pid(pid) : NULL;
    settled = c && bh_wait(bh_parked_pred, c, BH_BUDGET_NS);
    bool converted = false, resettled = false, stop_quiet = false, sp_zeroed = false;
    stamped = latched = alive = quiet = false;
    since = 0;
    u64 at = 0;
    posted = -1;
    msg[0] = '\0';
    st = -1;
    reaped = -1;
    killed = false;
    if (c) {
        stamped = settled && bh_head_parked(c, &since);
        irq_state_t s = proc_table_lock_acquire();
        if (stamped && c->state == PROC_STATE_ALIVE) {
            bool delivered = proc_debug_stop_deliver(c);
            proc_birth_hold_convert_locked(c);
            converted = delivered &&
                        __atomic_load_n(&c->debug_stop_req, __ATOMIC_ACQUIRE) == 1u &&
                        bh_hold_of(c) == BIRTH_HOLD_NONE;
        }
        proc_table_lock_release(s);
        // A regs write needs a settled target. A woken thread is readied at
        // once, so had the delivery woken the park, this wait would settle only
        // after the thread had run -- and its stamp would have moved.
        resettled = stamped && bh_wait(bh_parked_pred, c, BH_BUDGET_NS);
        stop_quiet = resettled && bh_head_parked(c, &at) && at == since;
        s = proc_table_lock_acquire();
        if (resettled && c->state == PROC_STATE_ALIVE) {
            struct Thread *th = c->threads;
            struct exception_context *f = th ? th->debug_trapframe : NULL;
            if (f) {
                f->sp = 0;
                sp_zeroed = true;
            }
        }
        proc_table_lock_release(s);
        if (sp_zeroed)
            posted = notes_post(c, "interrupt", 0u, NULL, true);
        latched = posted == 0 && proc_intr_terminate_pending(c);
        if (latched) {
            s = proc_table_lock_acquire();
            proc_interrupt_terminate_wake(c);
            proc_table_lock_release(s);
            quiet = bh_head_quiet_since(c, at, BH_QUIET_NS);
            alive = bh_alive(c);
        }
        s = proc_table_lock_acquire();
        if (c->state == PROC_STATE_ALIVE)
            proc_group_terminate(c, "killed");
        proc_table_lock_release(s);
        reaped = bh_reap_with_msg(pid, c, &st, &killed, msg, sizeof(msg));
    }
    TEST_ASSERT(pid > 0 && c != NULL, "a held spawn returned a live child (bad SP)");
    TEST_ASSERT(settled, "the child settled at its birth park (bad SP)");
    TEST_ASSERT(stamped, "the settled child's thread sleeps on its debug_rendez (bad SP)");
    TEST_ASSERT(converted, "premise: the stop was delivered and converted the hold (bad SP)");
    TEST_ASSERT(resettled, "the converted child is settled at its park");
    TEST_ASSERT(sp_zeroed, "the parked child's birth frame took SP 0");
    TEST_EXPECT_EQ(posted, 0, "the interrupt was posted (bad SP)");
    TEST_ASSERT(latched, "premise: the post armed the terminate latch (bad SP)");
    // The interrupt's verdict first, then each wake's: on a kernel without one
    // of the three rules, the first failure names that rule.
    TEST_ASSERT(alive, "the interrupt did not end the stopped child (bad SP)");
    TEST_ASSERT(stop_quiet,
        "the stop passed the parked thread by: never switched in (bad SP)");
    TEST_ASSERT(quiet,
        "the latch's wake passed the parked thread by: nothing re-ran a "
        "checkpoint it cannot pass (bad SP)");
    TEST_EXPECT_EQ(reaped, pid, "the killed child was reaped (bad SP)");
    TEST_ASSERT(!killed, "the kill ended the stopped child inside the budget");
    TEST_ASSERT(st != 0, "the child died of the kill (bad SP)");
    TEST_ASSERT(bh_str_eq(msg, "killed"),
        "the kill, not the interrupt, ended the stopped child (bad SP)");
}
