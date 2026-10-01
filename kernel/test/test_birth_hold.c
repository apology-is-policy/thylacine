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
//   birth_hold.held_spawn_parks        a real held spawn parks /hello on a
//                                      zeroed frame; release runs it; a
//                                      conversion keeps it parked at its entry
//                                      until the resume
//   birth_hold.held_spawn_death_wins   a kill, and an interrupt, at the birth
//                                      park end the child there -- the
//                                      interrupt even on a frame whose SP note
//                                      delivery refuses
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
void test_birth_hold_held_spawn_parks(void);
void test_birth_hold_held_spawn_death_wins(void);

// Not a mark value: a child that never ran must not read as NONE (0), or every
// "born NONE" control would pass on a child that did nothing.
#define BH_UNSEEN 0xDEADu

#define BH_BUDGET_NS (3ull * 1000ull * 1000ull * 1000ull)

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
        // The delivery wakes every blocked thread of the target; the parked one
        // re-parks without leaving the birth tail, which settling again with
        // its stopped PC still the entry shows.
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

    // (b) An interrupt at the birth park: the latch wakes the park, and the park
    //     applies the interrupt's default action itself, ending the child with
    //     its name rather than eret'ing a held child to deliver it.
    pid = bh_spawn_hello_held();
    c = (pid > 0) ? proc_find_by_pid(pid) : NULL;
    settled = c && bh_wait(bh_parked_pred, c, BH_BUDGET_NS);
    int posted = -1;
    char msg[32] = "";
    st = -1;
    reaped = -1;
    killed = false;
    if (c) {
        if (settled)
            posted = notes_post(c, "interrupt", 0u, NULL, true);
        irq_state_t s = proc_table_lock_acquire();
        if (posted == 0)
            proc_interrupt_terminate_wake(c);
        else if (c->state == PROC_STATE_ALIVE)
            proc_group_terminate(c, "killed");   // never strand the child
        proc_table_lock_release(s);
        reaped = bh_reap_with_msg(pid, c, &st, &killed, msg, sizeof(msg));
    }
    TEST_ASSERT(pid > 0 && c != NULL, "a held spawn returned a live child");
    TEST_ASSERT(settled, "the child settled at its birth park before the interrupt");
    TEST_EXPECT_EQ(posted, 0, "the interrupt was posted");
    TEST_EXPECT_EQ(reaped, pid, "the interrupted child was reaped");
    TEST_ASSERT(!killed, "the interrupt ended the child without the fallback kill");
    TEST_ASSERT(st != 0, "the interrupt ended the child at its birth tail");
    TEST_ASSERT(bh_str_eq(msg, "interrupt"),
                "the child exited with the interrupt's name, as note delivery's "
                "terminate arm reports it");

    // (c) The same interrupt on a frame note delivery will not touch. The child
    //     is converted (a debug stop delivered, the hold taken over), and then
    //     its birth frame's SP is written as 0, which a debugger's regs write
    //     may do. Note delivery declines a frame whose SP it does not trust, so
    //     a birth tail that relied on it to consume the latch would re-run its
    //     checkpoint, masked, until the fallback kill here -- and hang a
    //     one-CPU machine outright. The park must end the child itself.
    pid = bh_spawn_hello_held();
    c = (pid > 0) ? proc_find_by_pid(pid) : NULL;
    settled = c && bh_wait(bh_parked_pred, c, BH_BUDGET_NS);
    bool resettled = false, sp_zeroed = false;
    posted = -1;
    msg[0] = '\0';
    st = -1;
    reaped = -1;
    killed = false;
    if (c) {
        irq_state_t s = proc_table_lock_acquire();
        if (settled && c->state == PROC_STATE_ALIVE) {
            proc_debug_stop_deliver(c);
            proc_birth_hold_convert_locked(c);
        }
        proc_table_lock_release(s);
        // The delivery's wake rouses the parked thread; write the frame only
        // once it has re-parked, as a regs write needs a settled target.
        resettled = settled && bh_wait(bh_parked_pred, c, BH_BUDGET_NS);
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
        s = proc_table_lock_acquire();
        if (posted == 0)
            proc_interrupt_terminate_wake(c);
        else if (c->state == PROC_STATE_ALIVE)
            proc_group_terminate(c, "killed");   // never strand the child
        proc_table_lock_release(s);
        reaped = bh_reap_with_msg(pid, c, &st, &killed, msg, sizeof(msg));
    }
    TEST_ASSERT(pid > 0 && c != NULL, "a held spawn returned a live child (bad SP)");
    TEST_ASSERT(settled, "the child settled at its birth park (bad SP)");
    TEST_ASSERT(resettled, "the converted child re-parked after the delivery's wake");
    TEST_ASSERT(sp_zeroed, "the parked child's birth frame took SP 0");
    TEST_EXPECT_EQ(posted, 0, "the interrupt was posted (bad SP)");
    TEST_EXPECT_EQ(reaped, pid, "the interrupted child was reaped (bad SP)");
    TEST_ASSERT(!killed,
                "the park ended the child itself: nothing spun waiting for a "
                "delivery that declines the frame");
    TEST_ASSERT(st != 0, "the interrupt ended the child at its birth park (bad SP)");
    TEST_ASSERT(bh_str_eq(msg, "interrupt"),
                "the child exited with the interrupt's name (bad SP)");
}
