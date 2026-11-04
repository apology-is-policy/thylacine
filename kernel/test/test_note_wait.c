// SYS_NOTE_WAIT and the one mask restore (ARCH 8.8.3, operator vote 24). A kernel test
// has no mapped user stack, so a frame never builds here: a handler entered for real is
// the guest legs' (pouch-hello-signals, viv-pheno-probe). What runs here is everything up
// to the frame and everything a call does when no handler takes over.

#include "test.h"
#include "test_registry.h"

#include <thylacine/errno.h>
#include <thylacine/notes.h>
#include <thylacine/poll.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/spinlock.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/vivarium.h>

#include "../../arch/arm64/exception.h"
#include "../../arch/arm64/timer.h"

#define NW_HANDLER 0x4000u
#define NW_BIT(b)  (1ull << (b))
#define NW_USR1    NW_BIT(NOTE_BIT_USR1)
#define NW_USR2    NW_BIT(NOTE_BIT_USR2)
#define NW_CHILD   NW_BIT(NOTE_BIT_CHILD_EXIT)
#define NW_EINTR   (-(s64)T_E_INTR)
#define NW_POLL_OFF (-2)
#define NW_TERM    NW_BIT(NOTE_BIT_TERM)

static struct Proc *nw_proc(bool handler) {
    struct Proc *p = proc_alloc();
    if (!p) return NULL;
    p->state = PROC_STATE_ALIVE;
    if (handler) notes_set_handler(p, NW_HANDLER);
    return p;
}

static void nw_free(struct Proc *p) {
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

static u32 nw_queued(struct Proc *p) {
    spin_lock(&p->notes->lock);
    u32 n = p->notes->count;
    spin_unlock(&p->notes->lock);
    return n;
}

static bool nw_name_is(const struct Note *n, const char *want) {
    for (u32 i = 0; i < NOTE_NAME_MAX; i++) {
        if (n->name[i] != want[i]) return false;
        if (want[i] == 0) return true;
    }
    return true;
}

// A Thread for what never sleeps: a poll of the queue reads its word and its mask.
static struct Thread g_nw_fake;

static void nw_fake_on(struct Proc *p) {
    u8 *raw = (u8 *)&g_nw_fake;
    for (u32 i = 0; i < sizeof(g_nw_fake); i++) raw[i] = 0;
    g_nw_fake.magic = THREAD_MAGIC;
    g_nw_fake.proc  = p;
}

// -----------------------------------------------------------------------------
// The take word, on a fake thread: nothing here sleeps.
// -----------------------------------------------------------------------------

// A tty row names itself and no other, the family bit names all five, and what is left
// keeps its order. The control is the poll that finds nothing: a take that ignored the
// word would answer 0 at the first call.
void test_notewait_take_names_its_rows(void) {
    struct Proc *p = nw_proc(false);
    TEST_ASSERT(p != NULL, "proc_alloc");
    nw_fake_on(p);
    struct Note n;
    int d = 0;

    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_TTY_HUP, 0u, NULL, true), 0, "post hup");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_INTERRUPT, 0u, NULL, true), 0, "post interrupt");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_TTY_WINCH, 0u, NULL, true), 0, "post winch");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_TTY_QUIT, 0u, NULL, true), 0, "post quit");

    s64 none = notes_wait_take(&g_nw_fake, NOTE_TAKE_TTY_SUSP | NW_USR1, 0, &n, &d);
    s64 r1   = notes_wait_take(&g_nw_fake, NOTE_TAKE_TTY_WINCH, 0, &n, &d);
    bool n1  = nw_name_is(&n, NOTE_NAME_TTY_WINCH);
    u32 left = nw_queued(p);
    s64 r2   = notes_wait_take(&g_nw_fake, NW_BIT(NOTE_BIT_TTY), 0, &n, &d);
    bool n2  = nw_name_is(&n, NOTE_NAME_TTY_HUP);
    s64 r3   = notes_wait_take(&g_nw_fake, NW_BIT(NOTE_BIT_TTY), 0, &n, &d);
    bool n3  = nw_name_is(&n, NOTE_NAME_TTY_QUIT);
    s64 r4   = notes_wait_take(&g_nw_fake, 0, 0, &n, &d);
    u32 end  = nw_queued(p);

    g_nw_fake.proc = NULL;
    nw_free(p);

    TEST_EXPECT_EQ(none, -(s64)T_E_AGAIN, "nothing the word names: the poll answers EAGAIN");
    TEST_EXPECT_EQ(r1, 0ll, "the winch row");
    TEST_ASSERT(n1, "takes the winch and not the hangup ahead of it");
    TEST_EXPECT_EQ(left, 3u, "and only the winch");
    TEST_EXPECT_EQ(r2, 0ll, "the family bit");
    TEST_ASSERT(n2, "takes the oldest of the family");
    TEST_EXPECT_EQ(r3, 0ll, "the family bit again");
    TEST_ASSERT(n3, "past the interrupt it does not name");
    TEST_EXPECT_EQ(r4, -(s64)T_E_AGAIN, "an empty word takes nothing");
    TEST_EXPECT_EQ(end, 1u, "the interrupt stays queued");
}

void test_notewait_take_word(void) {
    TEST_ASSERT(notes_take_word_valid(NW_USR1 | NOTE_TAKE_TTY_HUP), "a family and a row");
    TEST_ASSERT(notes_take_word_valid(0), "an empty word: sigwait of signals with no note");
    TEST_ASSERT(!notes_take_word_valid(NW_BIT(NOTE_BIT_KILL)), "kill is nobody's to take");
    TEST_ASSERT(!notes_take_word_valid(NW_BIT(NOTE_BIT_SNARE)), "no snare is ever queued");
    TEST_ASSERT(!notes_take_word_valid(NW_BIT(21)), "above the tty rows");
    TEST_ASSERT(!notes_take_word_valid(NW_BIT(12)), "between the families and the rows");

    const u64 sig = 1ull;
    TEST_EXPECT_EQ(notes_take_for_sigset(sig << (VIV_SIGWINCH - 1)), NOTE_TAKE_TTY_WINCH,
                   "SIGWINCH is its row, not the family");
    TEST_EXPECT_EQ(notes_take_for_sigset(sig << (VIV_SIGINT - 1)), NW_BIT(NOTE_BIT_INTERRUPT),
                   "SIGINT is its family");
    TEST_EXPECT_EQ(notes_take_for_sigset(sig << (VIV_SIGKILL - 1)), 0ull, "SIGKILL names nothing");
    TEST_EXPECT_EQ(notes_take_for_sigset(sig << (14 - 1)), 0ull, "SIGALRM has no note");
    TEST_EXPECT_EQ(notes_take_for_sigset(sig << (VIV_SIGSEGV - 1)), 0ull, "no snare is queued");
    TEST_EXPECT_EQ(notes_take_for_sigset(sig << 63), 0ull, "the top of the set");
    TEST_ASSERT(notes_take_word_valid(notes_take_for_sigset(~0ull)),
                "every signal at once reads as a word SYS_NOTE_WAIT would take");
}

// The pop drains what the post armed, as a read of the fd does; a note the caller could
// not hand over goes back at the head with both latches. N-2: taken exactly once.
void test_notewait_take_drains_and_untake_rearms(void) {
    struct Proc *p = nw_proc(false);
    TEST_ASSERT(p != NULL, "proc_alloc");
    nw_fake_on(p);
    g_nw_fake.note_mask = NW_BIT(NOTE_BIT_TERM);
    struct Note n;
    int d = 0;

    TEST_EXPECT_EQ(sys_note_dispose_for_proc(p, NOTE_NAME_CHILD_EXIT, NOTE_DISP_DEFAULT, 0), 0ll,
                   "a table governs");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_TERM, 0u, NULL, true), 0, "post term");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_INTERRUPT, 0u, NULL, true), 0, "post interrupt");
    bool armed = (__atomic_load_n(&p->proc_flags, __ATOMIC_ACQUIRE) &
                  PROC_FLAG_TERM_TERMINATE_PENDING) != 0;

    s64 r = notes_wait_take(&g_nw_fake, NW_BIT(NOTE_BIT_TERM), 0, &n, &d);
    bool took = nw_name_is(&n, NOTE_NAME_TERM);
    int  from = d;
    bool drained = (__atomic_load_n(&p->proc_flags, __ATOMIC_ACQUIRE) &
                    PROC_FLAG_TERM_TERMINATE_PENDING) == 0;

    notes_wait_untake(&g_nw_fake, d, &n);
    bool rearmed = (__atomic_load_n(&p->proc_flags, __ATOMIC_ACQUIRE) &
                    PROC_FLAG_TERM_TERMINATE_PENDING) != 0;
    u32 back = nw_queued(p);
    struct Note head;
    spin_lock(&p->notes->lock);
    int peeked = notes_peek_for_fd_locked(p, NULL, &head);
    spin_unlock(&p->notes->lock);
    bool at_head = peeked && nw_name_is(&head, NOTE_NAME_TERM);

    g_nw_fake.proc = NULL;
    nw_free(p);

    TEST_ASSERT(armed, "the post armed term's latch: the fixture has something to drain");
    TEST_EXPECT_EQ(r, 0ll, "taken");
    TEST_ASSERT(took, "term");
    TEST_EXPECT_EQ(from, -1, "off the queue");
    TEST_ASSERT(drained, "the last term gone, its latch is gone");
    TEST_ASSERT(rearmed, "put back, a default that ends the Proc stays armed");
    TEST_EXPECT_EQ(back, 2u, "both notes queued");
    TEST_ASSERT(at_head, "and it is next, as it was");
}

// A thread's own note comes before a shared one of its family, as at the tail. Put back,
// it is on the word again.
void test_notewait_take_own_word_first(void) {
    struct Proc *p = nw_proc(false);
    TEST_ASSERT(p != NULL, "proc_alloc");
    nw_fake_on(p);
    struct Note n;
    int d = 0;

    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_USR1, 7u, NULL, true), 0, "post usr1");
    g_nw_fake.note_direct = (u16)NW_USR1;
    s64 r1 = notes_wait_take(&g_nw_fake, NW_USR1, 0, &n, &d);
    int d1 = d;
    u32 a1 = n.arg;
    u16 w1 = g_nw_fake.note_direct;
    notes_wait_untake(&g_nw_fake, d, &n);
    u16 w2 = g_nw_fake.note_direct;
    g_nw_fake.note_direct = 0;
    s64 r2 = notes_wait_take(&g_nw_fake, NW_USR1, 0, &n, &d);
    int d2 = d;
    u32 a2 = n.arg;

    g_nw_fake.proc = NULL;
    nw_free(p);

    TEST_EXPECT_EQ(r1, 0ll, "taken");
    TEST_EXPECT_EQ(d1, (int)NOTE_BIT_USR1, "from the word");
    TEST_EXPECT_EQ(a1, 0u, "a word's note has no arg, so it is not the queued one");
    TEST_EXPECT_EQ((u32)w1, 0u, "the bit is gone");
    TEST_EXPECT_EQ((u32)w2, (u32)NW_USR1, "put back on the word");
    TEST_EXPECT_EQ(r2, 0ll, "the queued one is still there");
    TEST_EXPECT_EQ(d2, -1, "off the queue");
    TEST_EXPECT_EQ(a2, 7u, "with its arg");
}

// As devnotes_read's R3-F1: a queued kill sends the thread to the tail, which alone may
// pop it, or a taker would sleep through its own death.
void test_notewait_take_leaves_a_kill(void) {
    struct Proc *p = nw_proc(false);
    TEST_ASSERT(p != NULL, "proc_alloc");
    nw_fake_on(p);
    struct Note n;
    int d = 0;
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_KILL, 0u, NULL, true), 0, "post kill");
    s64 r = notes_wait_take(&g_nw_fake, NW_USR1, -1, &n, &d);
    u32 left = nw_queued(p);
    g_nw_fake.proc = NULL;
    nw_free(p);
    TEST_EXPECT_EQ(r, NW_EINTR, "the wait ends without sleeping");
    TEST_EXPECT_EQ(left, 1u, "and the kill is the tail's");
}

// -----------------------------------------------------------------------------
// A thread that really waits.
// -----------------------------------------------------------------------------

static volatile u32  g_nw_done;
static volatile bool g_nw_exited;
static volatile s64  g_nw_rc;
static volatile u64  g_nw_mask_during, g_nw_mask_after, g_nw_elr, g_nw_x0;
static volatile u8   g_nw_flags_during, g_nw_flags_after;
static volatile bool g_nw_unwound;
static u64           g_nw_old, g_nw_set;
static s64           g_nw_timeout;
static bool          g_nw_take_form, g_nw_in_handler, g_nw_discard_first;
// The poll form's window, with sys_poll_sleep_for standing in for the poll: a kernel test
// has no user array to copy. < 0 is off. The result, when set, is what the call answers.
static s32           g_nw_poll_ms;
static s64           g_nw_poll_result;
static bool          g_nw_poll_answers;
static bool          g_nw_stop_at_the_tail;
static volatile s64  g_nw_begun;
static struct Note   g_nw_note;
static volatile int  g_nw_direct;
static u64           g_nw_sp;

static void nw_entry(void) {
    struct Thread *t = current_thread();
    int d = -2;

    t->note_call_x0       = 0x77u;
    t->note_mask          = g_nw_old;
    if (g_nw_in_handler) {
        t->in_handler      = true;
        t->note_saved_mask = NW_CHILD;
    }
    s64 rc;
    if (g_nw_poll_ms != NW_POLL_OFF) {
        t->note_interruptible = true;
        rc = notes_wait_mask_begin(t, g_nw_set, g_nw_poll_ms < 0);
        g_nw_begun = rc;
        if (rc == 0) {
            rc = g_nw_poll_answers ? g_nw_poll_result : sys_poll_sleep_for(g_nw_poll_ms);
            notes_wait_mask_end(t, rc);
        }
    } else {
        rc = g_nw_take_form ? notes_wait_take(t, g_nw_set, g_nw_timeout, &g_nw_note, &d)
                            : notes_wait_suspend(t, g_nw_set);
    }
    t->note_interruptible = false;
    g_nw_rc           = rc;
    g_nw_direct       = d;
    g_nw_mask_during  = t->note_mask;
    g_nw_flags_during = t->note_wait_flags;
    g_nw_unwound      = t->note_call_unwound;
    if (g_nw_in_handler) {
        t->in_handler = false;
        t->note_mask  = 0;
    }
    if (g_nw_discard_first) (void)notes_discard_name(t->proc, NOTE_NAME_USR1);
    if (g_nw_stop_at_the_tail) {
        t->note_mask = 0;
        t->note_wait_flags = 0;
        g_nw_done = 1u;
        test_thread_park_terminal(&g_nw_exited);
    }

    struct exception_context ctx;
    for (size_t k = 0; k < sizeof(ctx); k++) ((u8 *)&ctx)[k] = 0;
    ctx.sp      = g_nw_sp;
    ctx.elr     = 0x8000u;
    ctx.regs[0] = (u64)rc;
    irq_state_t s = spin_lock_irqsave(NULL);   // the tail runs masked (#713)
    notes_deliver_at_el0_return(&ctx);
    spin_unlock_irqrestore(NULL, s);
    g_nw_elr         = ctx.elr;
    g_nw_x0          = ctx.regs[0];
    g_nw_mask_after  = t->note_mask;
    g_nw_flags_after = t->note_wait_flags;

    g_nw_done = 1u;
    test_thread_park_terminal(&g_nw_exited);
}

struct nw_run {
    struct Proc   *p;
    struct Thread *t;
    int            tid;
    bool           finished;
};

static void nw_reset(u64 old, u64 set, bool take_form, s64 timeout) {
    g_nw_done = 0; g_nw_exited = false; g_nw_rc = 0x7fffffff;
    g_nw_mask_during = g_nw_mask_after = 0; g_nw_elr = g_nw_x0 = 0;
    g_nw_flags_during = g_nw_flags_after = 0xffu; g_nw_unwound = false;
    g_nw_old = old; g_nw_set = set; g_nw_timeout = timeout;
    g_nw_take_form = take_form; g_nw_in_handler = false; g_nw_discard_first = false;
    g_nw_poll_ms = NW_POLL_OFF; g_nw_poll_result = 0; g_nw_poll_answers = false;
    g_nw_stop_at_the_tail = false;
    g_nw_begun = 0x7fffffff;
    g_nw_direct = -3;
    g_nw_sp = 0x10000000u;
    for (u32 i = 0; i < sizeof(g_nw_note); i++) ((u8 *)&g_nw_note)[i] = 0;
}

static bool nw_start(struct nw_run *r, struct Proc *p) {
    r->p = p;
    r->t = thread_create(p, nw_entry);
    r->finished = false;
    if (!r->t) return false;
    r->tid = r->t->tid;
    ready(r->t);
    return true;
}

// Runs the thread until it blocks or is done. Which of them a caller expected is the
// caller's to read off g_nw_done.
static bool nw_until_blocked(struct nw_run *r) {
    u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
    while (timer_now_ns() < dl) {
        if (g_nw_done) return false;
        if (test_thread_sleeping(r->p, r->tid)) return true;
        sched();
    }
    return false;
}

static bool nw_until_done(void) {
    u64 dl = timer_now_ns() + TEST_YIELD_BUDGET_NS;
    while (!g_nw_done && timer_now_ns() < dl) sched();
    return g_nw_done != 0;
}

static void nw_finish(struct nw_run *r) {
    if (g_nw_done) {
        test_thread_join_last(r->p, &g_nw_exited);
        r->finished = test_proc_threads_freed(r->p);
    }
    test_proc_free_drained(r->p);
}

// The mask is the wait's while it stands. A caught note the wait's mask admits and the
// thread's holds ends it, and a frame that will not build leaves the note queued and
// the EINTR standing: re-issuing would spin. The thread's mask is back all the same.
void test_notewait_suspend_frame_fails_mask_back(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = nw_proc(true);
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_EXPECT_EQ(sys_note_dispose_for_proc(p, NOTE_NAME_USR1, NOTE_DISP_CAUGHT, 0), 0ll, "row");
    nw_reset(NW_USR1 | NW_USR2, NW_USR2, false, -1);
    struct nw_run r;
    bool started = nw_start(&r, p);
    bool blocked = started && nw_until_blocked(&r);
    bool posted  = false;
    if (blocked) {
        posted = notes_post(p, NOTE_NAME_USR1, 0u, NULL, true) == 0;
        proc_caught_note_wake(p);
    }
    bool done = blocked && nw_until_done();
    u32 left = nw_queued(p);
    nw_finish(&r);

    TEST_ASSERT(started, "thread_create");
    TEST_ASSERT(blocked, "the wait stands before any note");
    TEST_ASSERT(posted && done, "a note the wait's mask admits ends it");
    TEST_EXPECT_EQ(g_nw_rc, NW_EINTR, "sigsuspend answers EINTR");
    TEST_EXPECT_EQ((u64)g_nw_mask_during, NW_USR2, "under the wait's mask");
    TEST_EXPECT_EQ((u32)g_nw_flags_during, NOTE_WF_MASK_HELD | NOTE_WF_NOHAND, "held for the tail");
    TEST_ASSERT(g_nw_unwound, "it unwound for the note");
    TEST_EXPECT_EQ((u64)g_nw_elr, 0x8000ull, "the note is still deliverable: no re-issue");
    TEST_EXPECT_EQ((s64)g_nw_x0, NW_EINTR, "x0 stands");
    TEST_EXPECT_EQ((u64)g_nw_mask_after, NW_USR1 | NW_USR2, "the tail put the thread's mask back");
    TEST_EXPECT_EQ((u32)g_nw_flags_after, 0u, "and ended the marks");
    TEST_EXPECT_EQ(left, 1u, "the note waits under the thread's own mask");
}

// A note that is gone by the tail, a peer's or a row's that changed: no handler ran, so
// the wait was never interrupted as far as the program can tell, and the call goes again
// with its first argument, under the mask the thread had.
void test_notewait_suspend_note_gone_reissues(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = nw_proc(true);
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_EXPECT_EQ(sys_note_dispose_for_proc(p, NOTE_NAME_USR1, NOTE_DISP_CAUGHT, 0), 0ll, "row");
    nw_reset(NW_USR1, 0, false, -1);
    g_nw_discard_first = true;
    struct nw_run r;
    bool started = nw_start(&r, p);
    bool blocked = started && nw_until_blocked(&r);
    if (blocked) {
        (void)notes_post(p, NOTE_NAME_USR1, 0u, NULL, true);
        proc_caught_note_wake(p);
    }
    bool done = blocked && nw_until_done();
    nw_finish(&r);

    TEST_ASSERT(started && blocked && done, "the wait stood and the note ended it");
    TEST_EXPECT_EQ(g_nw_rc, NW_EINTR, "the wait unwound");
    TEST_EXPECT_EQ((u64)g_nw_elr, 0x7ffcull, "no handler ran: the call is re-issued");
    TEST_EXPECT_EQ((u64)g_nw_x0, 0x77ull, "with its first argument");
    TEST_EXPECT_EQ((u64)g_nw_mask_after, NW_USR1, "and the thread's mask");
}

// The race sigsuspend exists to close: blocked, the note comes, then the wait opens the
// mask. It must not sleep through a note that has already arrived.
void test_notewait_suspend_held_note_ends_it(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = nw_proc(true);
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_EXPECT_EQ(sys_note_dispose_for_proc(p, NOTE_NAME_USR1, NOTE_DISP_CAUGHT, 0), 0ll, "row");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_USR1, 0u, NULL, true), 0, "posted before the wait");
    nw_reset(NW_USR1, 0, false, -1);
    struct nw_run r;
    bool started = nw_start(&r, p);
    bool done = started && nw_until_done();
    nw_finish(&r);

    TEST_ASSERT(started && done, "the wait ended with nobody waking it");
    TEST_EXPECT_EQ(g_nw_rc, NW_EINTR, "EINTR");
    TEST_ASSERT(g_nw_unwound, "marked for the tail");
    TEST_EXPECT_EQ((u64)g_nw_mask_after, NW_USR1, "the thread's mask is back");
}

// A default that ignores, queued under the mask the thread had. It arms no latch and
// wakes nobody; only the tail acts on it. So the call goes to the tail, the tail discards
// it, and with no handler run the call goes again and now it can sleep. A stop's path is
// the same one, and needs a pgrp to park in: pty-susp-pouch's.
void test_notewait_suspend_tail_note_goes_to_the_tail(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = nw_proc(false);
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_EXPECT_EQ(sys_note_dispose_for_proc(p, NOTE_NAME_CHILD_EXIT, NOTE_DISP_DEFAULT, 0), 0ll,
                   "a table governs");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_CHILD_EXIT, 0u, NULL, true), 0, "posted under the mask");
    nw_reset(NW_CHILD, 0, false, -1);
    struct nw_run r;
    bool started = nw_start(&r, p);
    bool done = started && nw_until_done();
    u32 left = nw_queued(p);
    nw_finish(&r);

    TEST_ASSERT(started && done, "the call did not sleep on a note only the tail can take");
    TEST_EXPECT_EQ((u64)g_nw_elr, 0x7ffcull, "the tail took it and the call goes again");
    TEST_EXPECT_EQ((u64)g_nw_x0, 0x77ull, "with its first argument");
    TEST_EXPECT_EQ(left, 0u, "the note is gone");
    TEST_EXPECT_EQ((u64)g_nw_mask_after, NW_CHILD, "the thread's mask is back");
}

// The same call and note, with an sp the tail refuses: it takes nothing, so a re-issue
// would find the same note and never come back to EL0. The call reports instead.
void test_notewait_suspend_bad_sp_keeps_eintr(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = nw_proc(false);
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_EXPECT_EQ(sys_note_dispose_for_proc(p, NOTE_NAME_CHILD_EXIT, NOTE_DISP_DEFAULT, 0), 0ll,
                   "a table governs");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_CHILD_EXIT, 0u, NULL, true), 0, "posted under the mask");
    nw_reset(NW_CHILD, 0, false, -1);
    g_nw_sp = 0;
    struct nw_run r;
    bool started = nw_start(&r, p);
    bool done = started && nw_until_done();
    u32 left = nw_queued(p);
    nw_finish(&r);

    TEST_ASSERT(started && done, "the call did not sleep");
    TEST_EXPECT_EQ((u64)g_nw_elr, 0x8000ull, "no re-issue");
    TEST_EXPECT_EQ((s64)g_nw_x0, NW_EINTR, "EINTR stands");
    TEST_EXPECT_EQ(left, 1u, "the tail took nothing");
    TEST_EXPECT_EQ((u64)g_nw_mask_after, NW_CHILD, "the thread's mask is back");
}

// Its waits never unwind for a caught note, so nothing could end this one. One variable
// from the legs above: the same Proc with a row.
void test_notewait_suspend_refused(void) {
    struct Proc *p = nw_proc(true);
    TEST_ASSERT(p != NULL, "proc_alloc");
    nw_fake_on(p);
    g_nw_fake.note_mask = NW_USR1;
    s64 r = notes_wait_suspend(&g_nw_fake, 0);
    u64 mask = g_nw_fake.note_mask;
    u8 flags = g_nw_fake.note_wait_flags;
    s64 row = sys_note_dispose_for_proc(p, NOTE_NAME_USR1, NOTE_DISP_CAUGHT, 0);
    bool governs = notes_table_governs(p);
    g_nw_fake.proc = NULL;
    nw_free(p);
    TEST_EXPECT_EQ(r, -(s64)T_E_INVAL, "no table: refused");
    TEST_EXPECT_EQ(mask, NW_USR1, "the mask untouched");
    TEST_EXPECT_EQ((u32)flags, 0u, "and no mark");
    TEST_EXPECT_EQ(row, 0ll, "a row");
    TEST_ASSERT(governs, "and then a table governs: what the refusal asks");
}

// No handler can run inside a handler, and the saved slot is the handler's. So the masks
// stay as they are; the wait ends at a note its mask admits, which stays queued for the
// handler's return. A note it holds does not end it.
void test_notewait_suspend_in_a_handler(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = nw_proc(true);
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_EXPECT_EQ(sys_note_dispose_for_proc(p, NOTE_NAME_USR1, NOTE_DISP_CAUGHT, 0), 0ll, "row");
    TEST_EXPECT_EQ(sys_note_dispose_for_proc(p, NOTE_NAME_USR2, NOTE_DISP_CAUGHT, 0), 0ll, "row");
    nw_reset(NW_USR1 | NW_USR2, NW_USR1, false, -1);
    g_nw_in_handler = true;
    struct nw_run r;
    bool started = nw_start(&r, p);
    bool blocked = started && nw_until_blocked(&r);
    bool rode = false;
    if (blocked) {
        (void)notes_post(p, NOTE_NAME_USR1, 0u, NULL, true);
        proc_caught_note_wake(p);
        for (u32 i = 0; i < 16u; i++) sched();
        (void)notes_post(p, NOTE_NAME_CHILD_EXIT, 0u, NULL, true);
        for (u32 i = 0; i < 16u; i++) sched();
        rode = g_nw_done == 0 && test_thread_sleeping(p, r.tid);
        (void)notes_post(p, NOTE_NAME_USR2, 0u, NULL, true);
        proc_caught_note_wake(p);
    }
    bool done = blocked && nw_until_done();
    u32 left = nw_queued(p);
    nw_finish(&r);

    TEST_ASSERT(started && blocked, "the wait stands");
    TEST_ASSERT(rode, "a note its mask holds does not end it, nor one no handler takes");
    TEST_ASSERT(done, "one it admits does");
    TEST_EXPECT_EQ(g_nw_rc, NW_EINTR, "EINTR");
    TEST_EXPECT_EQ((u64)g_nw_mask_during, NW_USR1 | NW_USR2, "the handler's mask untouched");
    TEST_EXPECT_EQ((u32)g_nw_flags_during, 0u, "nothing held: the saved slot is the handler's");
    TEST_EXPECT_EQ(left, 3u, "all three wait for the handler's return");
}

// A handler no one had installed when the wait began. The class skips a Proc with no
// handler, and a wait with no other end must still unwind for it.
void test_notewait_suspend_handler_installed_late(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = nw_proc(false);
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_EXPECT_EQ(sys_note_dispose_for_proc(p, NOTE_NAME_CHILD_EXIT, NOTE_DISP_DEFAULT, 0), 0ll,
                   "a table, and no handler");
    nw_reset(0, 0, false, -1);
    struct nw_run r;
    bool started = nw_start(&r, p);
    bool blocked = started && nw_until_blocked(&r);
    s64 row = -1;
    bool posted = false;
    if (blocked) {
        notes_set_handler(p, NW_HANDLER);
        row = sys_note_dispose_for_proc(p, NOTE_NAME_USR1, NOTE_DISP_CAUGHT, 0);
        posted = notes_post(p, NOTE_NAME_USR1, 0u, NULL, true) == 0;
        proc_caught_note_wake(p);
    }
    bool done = blocked && nw_until_done();
    nw_finish(&r);

    TEST_ASSERT(started && blocked, "the wait stands with no handler");
    TEST_EXPECT_EQ(row, 0ll, "the peer's row");
    TEST_ASSERT(posted && done, "the note ends it");
    TEST_EXPECT_EQ(g_nw_rc, NW_EINTR, "EINTR");
    TEST_ASSERT(g_nw_unwound, "it unwound for the note");
}

// Masked, as POSIX asks of a taker: it arms no latch and wakes no walk. The queue's own
// hook is the wake, and a timed form and a forever form differ only in that one may be
// re-issued.
void test_notewait_take_sleeps_until_posted(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = nw_proc(true);
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_EXPECT_EQ(sys_note_dispose_for_proc(p, NOTE_NAME_USR1, NOTE_DISP_CAUGHT, 0), 0ll, "row");
    nw_reset(NW_USR1, NW_USR1, true, -1);
    struct nw_run r;
    bool started = nw_start(&r, p);
    bool blocked = started && nw_until_blocked(&r);
    if (blocked) (void)notes_post(p, NOTE_NAME_USR1, 42u, NULL, true);
    bool done = blocked && nw_until_done();
    u32 left = nw_queued(p);
    nw_finish(&r);

    TEST_ASSERT(started && blocked, "the wait stands");
    TEST_ASSERT(done, "the post alone wakes a taker that masked the family");
    TEST_EXPECT_EQ(g_nw_rc, 0ll, "taken");
    TEST_ASSERT(nw_name_is(&g_nw_note, NOTE_NAME_USR1), "the note");
    TEST_EXPECT_EQ(g_nw_note.arg, 42u, "with its arg");
    TEST_EXPECT_EQ(left, 0u, "consumed");
    TEST_EXPECT_EQ((u64)g_nw_mask_during, NW_USR1, "the mask never moved");
    TEST_EXPECT_EQ((u32)g_nw_flags_during, NOTE_WF_NOHAND, "a forever wait goes again if no handler ran");
    TEST_EXPECT_EQ((u64)g_nw_elr, 0x8000ull, "a take is an answer, not a re-issue");
    TEST_EXPECT_EQ((u32)g_nw_flags_after, 0u, "the tail ended the mark");
}

// A note posted at this thread, masked. The word is not the queue, so the post has to
// wake the list as well as rouse the thread.
void test_notewait_take_wakes_for_its_word(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = nw_proc(true);
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_EXPECT_EQ(sys_note_dispose_for_proc(p, NOTE_NAME_USR1, NOTE_DISP_CAUGHT, 0), 0ll, "row");
    nw_reset(NW_USR1, NW_USR1, true, -1);
    struct nw_run r;
    bool started = nw_start(&r, p);
    bool blocked = started && nw_until_blocked(&r);
    int  posted = blocked ? proc_note_thread(p, r.tid, NOTE_NAME_USR1, NULL) : -99;
    bool done = blocked && nw_until_done();
    nw_finish(&r);

    TEST_ASSERT(started && blocked, "the wait stands");
    TEST_EXPECT_EQ(posted, 0, "posted at the thread");
    TEST_ASSERT(done, "its word wakes it");
    TEST_EXPECT_EQ(g_nw_rc, 0ll, "taken");
    TEST_EXPECT_EQ(g_nw_direct, (int)NOTE_BIT_USR1, "off the word");
    TEST_ASSERT(nw_name_is(&g_nw_note, NOTE_NAME_USR1), "the note");
}

void test_notewait_take_times_out(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = nw_proc(true);
    TEST_ASSERT(p != NULL, "proc_alloc");
    nw_reset(NW_USR1, NW_USR1, true, 5ll * 1000ll * 1000ll);
    struct nw_run r;
    bool started = nw_start(&r, p);
    bool done = started && nw_until_done();
    nw_finish(&r);
    TEST_ASSERT(started && done, "the deadline ends the wait");
    TEST_EXPECT_EQ(g_nw_rc, -(s64)T_E_AGAIN, "sigtimedwait's EAGAIN");
    TEST_EXPECT_EQ((u32)g_nw_flags_during, 0u, "a timed wait is not re-issued: it would start again");
}

// A caught note the word does not name: the handler is owed, so the wait ends. A frame
// that will not build keeps the EINTR.
void test_notewait_take_unwinds_for_another_note(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = nw_proc(true);
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_EXPECT_EQ(sys_note_dispose_for_proc(p, NOTE_NAME_USR2, NOTE_DISP_CAUGHT, 0), 0ll, "row");
    nw_reset(NW_USR1, NW_USR1, true, -1);
    struct nw_run r;
    bool started = nw_start(&r, p);
    bool blocked = started && nw_until_blocked(&r);
    if (blocked) {
        (void)notes_post(p, NOTE_NAME_USR2, 0u, NULL, true);
        proc_caught_note_wake(p);
    }
    bool done = blocked && nw_until_done();
    nw_finish(&r);
    TEST_ASSERT(started && blocked && done, "the wait stood and the note ended it");
    TEST_EXPECT_EQ(g_nw_rc, NW_EINTR, "EINTR");
    TEST_ASSERT(g_nw_unwound, "it unwound");
    TEST_EXPECT_EQ((u64)g_nw_elr, 0x8000ull, "the handler is still owed");
}

// A caught note the word does name, left unmasked. It may unwind the wait, and the look
// after the unwind still takes it: Linux dequeues before it reports EINTR.
void test_notewait_take_before_unwinding(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = nw_proc(true);
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_EXPECT_EQ(sys_note_dispose_for_proc(p, NOTE_NAME_USR1, NOTE_DISP_CAUGHT, 0), 0ll, "row");
    nw_reset(0, NW_USR1, true, -1);
    struct nw_run r;
    bool started = nw_start(&r, p);
    bool blocked = started && nw_until_blocked(&r);
    if (blocked) {
        (void)notes_post(p, NOTE_NAME_USR1, 0u, NULL, true);
        proc_caught_note_wake(p);
    }
    bool done = blocked && nw_until_done();
    u32 left = nw_queued(p);
    nw_finish(&r);
    TEST_ASSERT(started && blocked && done, "the wait stood and the note ended it");
    TEST_EXPECT_EQ(g_nw_rc, 0ll, "taken, not interrupted");
    TEST_EXPECT_EQ(left, 0u, "and consumed");
}

// -----------------------------------------------------------------------------
// The one way into a handler's mask and the one way out.
// -----------------------------------------------------------------------------

void test_notewait_handler_mask_enter_and_restore(void) {
    struct Proc *p = nw_proc(true);
    TEST_ASSERT(p != NULL, "proc_alloc");
    nw_fake_on(p);
    struct exception_context ctx;

    // Plain delivery: what goes back is what the interrupted code ran under.
    g_nw_fake.note_mask = NW_USR1;
    u64 i1 = notes_interrupted_mask_for_test(&g_nw_fake);
    notes_handler_mask_enter_for_test(&g_nw_fake, NW_USR1 | NW_USR2);
    u64 s1 = g_nw_fake.note_saved_mask, m1 = g_nw_fake.note_mask;
    g_nw_fake.in_handler = true;
    int r1 = notes_noted_restore(&ctx, &g_nw_fake);
    u64 a1 = g_nw_fake.note_mask;

    // A handler's own change does not outlive it.
    notes_handler_mask_enter_for_test(&g_nw_fake, NW_USR1);
    g_nw_fake.in_handler = true;
    g_nw_fake.note_mask  = NW_CHILD;
    (void)notes_noted_restore(&ctx, &g_nw_fake);
    u64 a2 = g_nw_fake.note_mask;

    // Out of a wait: the handler builds on the wait's mask, a frame reports the thread's,
    // and the thread's goes back. The re-issue mark stays for the tail.
    g_nw_fake.note_saved_mask = NW_CHILD;
    g_nw_fake.note_mask       = NW_USR2;
    g_nw_fake.note_wait_flags = NOTE_WF_MASK_HELD | NOTE_WF_NOHAND;
    u64 i3 = notes_interrupted_mask_for_test(&g_nw_fake);
    notes_handler_mask_enter_for_test(&g_nw_fake, NW_USR2 | NW_USR1);
    u64 s3 = g_nw_fake.note_saved_mask, m3 = g_nw_fake.note_mask;
    u8  f3 = g_nw_fake.note_wait_flags;
    g_nw_fake.in_handler = true;
    (void)notes_noted_restore(&ctx, &g_nw_fake);
    u64 a3 = g_nw_fake.note_mask;

    g_nw_fake.proc = NULL;
    nw_free(p);

    TEST_EXPECT_EQ(i1, NW_USR1, "no wait: the interrupted mask is the thread's");
    TEST_EXPECT_EQ(s1, NW_USR1, "saved");
    TEST_EXPECT_EQ(m1, NW_USR1 | NW_USR2, "the handler runs under its own");
    TEST_EXPECT_EQ(r1, 0, "restored");
    TEST_EXPECT_EQ(a1, NW_USR1, "and it goes back");
    TEST_EXPECT_EQ(a2, NW_USR1, "whatever the handler did");
    TEST_EXPECT_EQ(i3, NW_CHILD, "in a wait: a frame reports the thread's mask");
    TEST_EXPECT_EQ(s3, NW_CHILD, "the save keeps it");
    TEST_EXPECT_EQ(m3, NW_USR2 | NW_USR1, "the handler builds on the wait's");
    TEST_EXPECT_EQ((u32)f3, NOTE_WF_NOHAND, "the handler holds the mask now, the re-issue mark stays");
    TEST_EXPECT_EQ(a3, NW_CHILD, "and its return ends the wait's mask too");
}

// -----------------------------------------------------------------------------
// The row and the call's edges.
// -----------------------------------------------------------------------------

void test_notewait_dispose_row_carries_mask(void) {
    struct Proc *p = nw_proc(true);
    TEST_ASSERT(p != NULL, "proc_alloc");
    u64 mask = NW_USR2 | NW_CHILD;
    s64 r0 = sys_note_dispose_for_proc(p, NOTE_NAME_USR1, NOTE_DISP_CAUGHT,
                                       NOTE_DISP_RESTART | NOTE_DISP_RESETHAND |
                                       NOTE_DISP_NODEFER | NOTE_DISP_MASK(mask));
    struct viv_ksigaction act;
    for (u32 i = 0; i < sizeof(act); i++) ((u8 *)&act)[i] = 0;
    bool read = viv_sigtab_note_handler(p->sigtab, VIV_SIGNOTE_USR1, &act);

    s64 r_bit  = sys_note_dispose_for_proc(p, NOTE_NAME_USR2, NOTE_DISP_CAUGHT, 8u);
    s64 r_gap  = sys_note_dispose_for_proc(p, NOTE_NAME_USR2, NOTE_DISP_CAUGHT, 1u << 15);
    s64 r_mask = sys_note_dispose_for_proc(p, NOTE_NAME_USR2, NOTE_DISP_CAUGHT,
                                           NOTE_DISP_MASK(NOTE_MASK_SUPPORTED + 1ull));
    s64 r_ign  = sys_note_dispose_for_proc(p, NOTE_NAME_USR2, NOTE_DISP_IGNORE, NOTE_DISP_MASK(mask));
    s64 r_good = sys_note_dispose_for_proc(p, NOTE_NAME_USR2, NOTE_DISP_CAUGHT,
                                           NOTE_DISP_MASK(NOTE_MASK_SUPPORTED));

    nw_free(p);
    TEST_EXPECT_EQ(r0, 0ll, "the row takes all of it");
    TEST_ASSERT(read, "and it reads as caught");
    TEST_EXPECT_EQ(act.flags & (u64)VIV_SA_RESTART, (u64)VIV_SA_RESTART, "restart");
    TEST_EXPECT_EQ(act.flags & (u64)VIV_SA_RESETHAND, (u64)VIV_SA_RESETHAND, "resethand");
    TEST_EXPECT_EQ(act.flags & (u64)VIV_SA_NODEFER, (u64)VIV_SA_NODEFER, "nodefer");
    TEST_EXPECT_EQ(act.mask, mask, "the mask, in note bits");
    TEST_EXPECT_EQ(r_bit, -(s64)T_E_INVAL, "a flag this does not name");
    TEST_EXPECT_EQ(r_gap, -(s64)T_E_INVAL, "the gap below the mask");
    TEST_EXPECT_EQ(r_mask, -(s64)T_E_INVAL, "a mask bit no note has");
    TEST_EXPECT_EQ(r_ign, -(s64)T_E_INVAL, "a mask on a row that is not caught");
    TEST_EXPECT_EQ(r_good, 0ll, "every family at once is a mask");
}

// A poll that answers without a note: the mask is back before the tail, so a note only the
// call's mask let in runs no handler and stays queued. The answer here is handed in, so this
// is the restore and not the order: that a ready fd answers first is poll.c's, and only
// the guest legs run it (pouch-hello-signals ppoll-ready, viv-pheno-probe L360).
void test_notewait_poll_ready_keeps_the_note(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = nw_proc(true);
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_EXPECT_EQ(sys_note_dispose_for_proc(p, NOTE_NAME_USR1, NOTE_DISP_CAUGHT, 0), 0ll, "row");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_USR1, 0u, NULL, true), 0, "posted under the mask");
    nw_reset(NW_USR1 | NW_USR2, 0, false, -1);
    g_nw_poll_ms = -1; g_nw_poll_answers = true; g_nw_poll_result = 1;
    struct nw_run r;
    bool started = nw_start(&r, p);
    bool done = started && nw_until_done();
    u32 left = nw_queued(p);
    nw_finish(&r);

    TEST_ASSERT(started && done, "the call ran");
    TEST_EXPECT_EQ((s64)g_nw_begun, 0ll, "a caught note does not stop the poll");
    TEST_EXPECT_EQ(g_nw_rc, 1ll, "the ready count stands");
    TEST_EXPECT_EQ((u64)g_nw_mask_during, NW_USR1 | NW_USR2, "the thread's mask is back at once");
    TEST_EXPECT_EQ((u32)g_nw_flags_during, 0u, "and the marks are gone");
    TEST_EXPECT_EQ((u64)g_nw_elr, 0x8000ull, "no re-issue");
    TEST_EXPECT_EQ((s64)g_nw_x0, 1ll, "x0 stands");
    TEST_EXPECT_EQ(left, 1u, "the note waits under the thread's own mask");
}

// The same at the deadline: 0 is no note's doing either.
void test_notewait_poll_deadline_restores(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = nw_proc(true);
    TEST_ASSERT(p != NULL, "proc_alloc");
    nw_reset(NW_USR2, NW_USR1, false, -1);
    g_nw_poll_ms = 1;
    struct nw_run r;
    bool started = nw_start(&r, p);
    bool done = started && nw_until_done();
    nw_finish(&r);

    TEST_ASSERT(started && done, "the sleep ended by itself");
    TEST_EXPECT_EQ(g_nw_rc, 0ll, "0 at the deadline");
    TEST_EXPECT_EQ((u64)g_nw_mask_during, NW_USR2, "the thread's mask is back at once");
    TEST_EXPECT_EQ((u64)g_nw_mask_after, NW_USR2, "and after the tail");
}

// A caught note the call's mask admits ends the wait, and the mask stays the call's until
// the tail. No frame builds here, so the note stays queued and the EINTR stands: the
// handler's half is the guest legs'.
void test_notewait_poll_note_ends_the_wait(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = nw_proc(true);
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_EXPECT_EQ(sys_note_dispose_for_proc(p, NOTE_NAME_USR1, NOTE_DISP_CAUGHT, 0), 0ll, "row");
    nw_reset(NW_USR1 | NW_USR2, NW_USR2, false, -1);
    g_nw_poll_ms = -1;
    struct nw_run r;
    bool started = nw_start(&r, p);
    bool blocked = started && nw_until_blocked(&r);
    bool posted  = false;
    if (blocked) {
        posted = notes_post(p, NOTE_NAME_USR1, 0u, NULL, true) == 0;
        proc_caught_note_wake(p);
    }
    bool done = blocked && nw_until_done();
    u32 left = nw_queued(p);
    nw_finish(&r);

    TEST_ASSERT(started && blocked, "the wait stands before any note");
    TEST_ASSERT(posted && done, "a note the call's mask admits ends it");
    TEST_EXPECT_EQ(g_nw_rc, NW_EINTR, "EINTR");
    TEST_EXPECT_EQ((u64)g_nw_mask_during, NW_USR2, "still the call's mask");
    TEST_EXPECT_EQ((u32)g_nw_flags_during, NOTE_WF_MASK_HELD | NOTE_WF_NOHAND, "held for the tail");
    TEST_EXPECT_EQ((u64)g_nw_mask_after, NW_USR1 | NW_USR2, "the tail put the thread's mask back");
    TEST_EXPECT_EQ((u32)g_nw_flags_after, 0u, "and ended the marks");
    TEST_EXPECT_EQ(left, 1u, "the note waits under the thread's own mask");
}

// A note a mask held that no handler takes arms nothing a poll reads. Only the tail acts on
// it, so the call goes there before waiting, and comes back with the note gone, with no
// time lost, so even a timed call re-issues. The window is the call's own.
void test_notewait_poll_tail_note_goes_to_the_tail(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = nw_proc(false);
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_EXPECT_EQ(sys_note_dispose_for_proc(p, NOTE_NAME_CHILD_EXIT, NOTE_DISP_DEFAULT, 0), 0ll,
                   "a table governs");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_CHILD_EXIT, 0u, NULL, true), 0, "posted under the mask");
    nw_reset(NW_CHILD, 0, false, -1);
    g_nw_poll_ms = 1000;
    struct nw_run r;
    bool started = nw_start(&r, p);
    bool done = started && nw_until_done();
    u32 left = nw_queued(p);
    nw_finish(&r);

    TEST_ASSERT(started && done, "the call did not wait on a note only the tail can take");
    TEST_EXPECT_EQ((s64)g_nw_begun, NW_EINTR, "it went to the tail");
    TEST_EXPECT_EQ((u64)g_nw_elr, 0x7ffcull, "the tail took it and the call goes again");
    TEST_EXPECT_EQ((u64)g_nw_x0, 0x77ull, "with its first argument");
    TEST_EXPECT_EQ(left, 0u, "the note is gone");
    TEST_EXPECT_EQ((u64)g_nw_mask_after, NW_CHILD, "the thread's mask is back");
}

// A fatal note the call's mask lets in, posted while the poll sleeps. The poll core answers
// the death wake with 0, and the mask must stay: the tail ends the Proc under it. Restored,
// the note would wait behind the caller's mask and an untimed poll would read a timeout.
// The harness stops short of the tail, which would end this test's thread.
void test_notewait_poll_death_wake_keeps_the_mask(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = nw_proc(false);
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_EXPECT_EQ(sys_note_dispose_for_proc(p, NOTE_NAME_TERM, NOTE_DISP_DEFAULT, 0), 0ll,
                   "a table governs");
    nw_reset(NW_TERM, 0, false, -1);
    g_nw_poll_ms = -1;
    g_nw_stop_at_the_tail = true;
    struct nw_run r;
    bool started = nw_start(&r, p);
    bool blocked = started && nw_until_blocked(&r);
    bool posted  = false;
    if (blocked) {
        posted = notes_post(p, NOTE_NAME_TERM, 0u, NULL, true) == 0;
        proc_interrupt_terminate_wake(p);
    }
    bool done = blocked && nw_until_done();
    u32 left = nw_queued(p);
    nw_finish(&r);

    TEST_ASSERT(started && blocked, "the wait stands before any note");
    TEST_ASSERT(posted && done, "the death wake ends it");
    TEST_EXPECT_EQ(g_nw_rc, 0ll, "the poll core's answer to a death wake");
    TEST_EXPECT_EQ((u64)g_nw_mask_during, 0ull, "still the call's mask");
    TEST_EXPECT_EQ((u32)g_nw_flags_during, NOTE_WF_MASK_HELD | NOTE_WF_NOHAND, "held for the tail");
    TEST_EXPECT_EQ(left, 1u, "and the note is the tail's");
}

// Each answer, one variable from the others, with a fatal note the call's mask admits.
void test_notewait_mask_end_reads_the_answer(void) {
    struct Proc *p = nw_proc(false);
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_EXPECT_EQ(sys_note_dispose_for_proc(p, NOTE_NAME_TERM, NOTE_DISP_DEFAULT, 0), 0ll,
                   "a table governs");
    nw_fake_on(p);

    #define NW_HOLD() do { g_nw_fake.note_saved_mask = NW_TERM; g_nw_fake.note_mask = 0; \
                           g_nw_fake.note_wait_flags = NOTE_WF_MASK_HELD; } while (0)
    NW_HOLD();
    notes_wait_mask_end(&g_nw_fake, 0);
    u64 quiet_zero = g_nw_fake.note_mask;
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_TERM, 0u, NULL, true), 0, "term");
    NW_HOLD();
    notes_wait_mask_end(&g_nw_fake, 0);
    u64 dying_zero = g_nw_fake.note_mask;
    NW_HOLD();
    notes_wait_mask_end(&g_nw_fake, 1);
    u64 dying_ready = g_nw_fake.note_mask;
    NW_HOLD();
    notes_wait_mask_end(&g_nw_fake, -(s64)T_E_FAULT);
    u64 dying_fault = g_nw_fake.note_mask;
    NW_HOLD();
    notes_wait_mask_end(&g_nw_fake, -(s64)T_E_INTR);
    u64 dying_eintr = g_nw_fake.note_mask;
    #undef NW_HOLD
    g_nw_fake.note_wait_flags = 0;
    g_nw_fake.proc = NULL;
    nw_free(p);

    TEST_EXPECT_EQ(quiet_zero, NW_TERM, "control: a deadline with nothing fatal restores");
    TEST_EXPECT_EQ(dying_zero, 0ull, "a death wake's 0 keeps the call's mask");
    TEST_EXPECT_EQ(dying_ready, NW_TERM, "a ready count restores: the fds win");
    TEST_EXPECT_EQ(dying_fault, NW_TERM, "so does a write-back's fault");
    TEST_EXPECT_EQ(dying_eintr, 0ull, "EINTR keeps it");
}

// With no table the tail leaves a note whose default ignores queued, so going to the tail
// for it would re-issue the call for ever. The call waits, under its mask, and answers.
void test_notewait_poll_no_table_has_no_tail_note(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = nw_proc(false);
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_TTY_WINCH, 0u, NULL, true), 0, "posted");
    nw_reset(NW_BIT(NOTE_BIT_TTY), 0, false, -1);
    g_nw_poll_ms = 1;
    struct nw_run r;
    bool started = nw_start(&r, p);
    bool done = started && nw_until_done();
    u32 left = nw_queued(p);
    nw_finish(&r);

    TEST_ASSERT(started && done, "the sleep ended by itself");
    TEST_EXPECT_EQ((s64)g_nw_begun, 0ll, "not sent to a tail that would not take it");
    TEST_EXPECT_EQ(g_nw_rc, 0ll, "0 at the deadline");
    TEST_EXPECT_EQ((u64)g_nw_elr, 0x8000ull, "no re-issue");
    TEST_EXPECT_EQ((u64)g_nw_mask_after, NW_BIT(NOTE_BIT_TTY), "the thread's mask is back");
    TEST_EXPECT_EQ(left, 1u, "the note stays");
}

// In a handler the saved slot holds the mask the handler returns to, and no note can run
// anyway. The call leaves both alone.
void test_notewait_poll_in_a_handler(void) {
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree must be empty at test entry");
    struct Proc *p = nw_proc(true);
    TEST_ASSERT(p != NULL, "proc_alloc");
    nw_reset(NW_USR1, NW_USR2, false, -1);
    g_nw_in_handler = true;
    g_nw_poll_ms = 1;
    struct nw_run r;
    bool started = nw_start(&r, p);
    bool done = started && nw_until_done();
    nw_finish(&r);

    TEST_ASSERT(started && done, "the sleep ended by itself");
    TEST_EXPECT_EQ(g_nw_rc, 0ll, "0 at the deadline");
    TEST_EXPECT_EQ((u64)g_nw_mask_during, NW_USR1, "the handler's mask is untouched");
    TEST_EXPECT_EQ((u32)g_nw_flags_during, 0u, "and nothing is held");
}

// What sigpending reads: queued and held, by row where the row is known; the thread's own
// word as well as the queue; nothing the mask lets through; everything in a handler.
void test_notewait_pending_reads_what_is_held(void) {
    struct Proc *p = nw_proc(true);
    TEST_ASSERT(p != NULL, "proc_alloc");
    nw_fake_on(p);

    g_nw_fake.note_mask = NW_USR1 | NW_BIT(NOTE_BIT_TTY);
    u64 empty = notes_wait_pending(&g_nw_fake);
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_USR1, 0u, NULL, true), 0, "usr1");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_USR2, 0u, NULL, true), 0, "usr2");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_TTY_WINCH, 0u, NULL, true), 0, "winch");
    u64 held = notes_wait_pending(&g_nw_fake);
    __atomic_store_n(&g_nw_fake.note_direct, (u16)NW_USR2, __ATOMIC_RELEASE);
    u64 unheld_own = notes_wait_pending(&g_nw_fake);
    g_nw_fake.note_mask |= NW_USR2;
    u64 held_own = notes_wait_pending(&g_nw_fake);
    __atomic_store_n(&g_nw_fake.note_direct, (u16)0, __ATOMIC_RELEASE);
    g_nw_fake.note_mask = 0;
    u64 none = notes_wait_pending(&g_nw_fake);
    g_nw_fake.in_handler = true;
    u64 in_handler = notes_wait_pending(&g_nw_fake);
    g_nw_fake.in_handler = false;
    g_nw_fake.note_mask = NW_USR2;
    g_nw_fake.note_saved_mask = NW_USR1;
    g_nw_fake.note_wait_flags = NOTE_WF_MASK_HELD;
    u64 call_mask = notes_wait_pending(&g_nw_fake);
    u32 left = nw_queued(p);
    g_nw_fake.proc = NULL;
    nw_free(p);

    TEST_EXPECT_EQ(empty, 0ull, "nothing queued reads nothing");
    TEST_EXPECT_EQ(held, NW_USR1 | NOTE_TAKE_TTY_WINCH, "held notes, by row: usr2 is not held");
    TEST_EXPECT_EQ(unheld_own, NW_USR1 | NOTE_TAKE_TTY_WINCH, "an own bit the mask lets through");
    TEST_EXPECT_EQ(held_own, NW_USR1 | NW_USR2 | NOTE_TAKE_TTY_WINCH, "an own bit the mask holds");
    TEST_EXPECT_EQ(none, 0ull, "a clear mask holds nothing");
    TEST_EXPECT_EQ(in_handler, NW_USR1 | NW_USR2 | NOTE_TAKE_TTY_WINCH, "a handler holds all of it");
    TEST_EXPECT_EQ(call_mask, NW_USR1, "a wait's own mask is not the thread's");
    TEST_EXPECT_EQ(left, 3u, "and reading it takes nothing");
    TEST_ASSERT(notes_take_word_valid(in_handler), "the answer is a take word");
}

void test_notewait_pending_sigset_names_the_row(void) {
    TEST_EXPECT_EQ(notes_sigset_for_take(NW_USR1), 1ull << (10u - 1u), "usr1 is SIGUSR1");
    TEST_EXPECT_EQ(notes_sigset_for_take(NOTE_TAKE_TTY_WINCH), 1ull << (28u - 1u),
                   "the winch row is SIGWINCH and no other tty signal");
    TEST_EXPECT_EQ(notes_take_for_sigset(notes_sigset_for_take(NW_USR2 | NOTE_TAKE_TTY_HUP)),
                   NW_USR2 | NOTE_TAKE_TTY_HUP, "and back");
    TEST_EXPECT_EQ(notes_sigset_for_take(0), 0ull, "nothing is nothing");
}

void test_notewait_syscall_refusals(void) {
    TEST_EXPECT_EQ(sys_note_wait_handler(4u, 0, 0, (u64)-1ll, 0), -(s64)T_E_INVAL, "no fifth form");
    TEST_EXPECT_EQ(sys_note_wait_handler(NOTE_WAIT_SUSPEND, 0, 0x1000u, (u64)-1ll, 0), -(s64)T_E_INVAL,
                   "a suspend has no record");
    TEST_EXPECT_EQ(sys_note_wait_handler(NOTE_WAIT_SUSPEND, 0, 0, 0u, 0), -(s64)T_E_INVAL,
                   "and no timeout");
    TEST_EXPECT_EQ(sys_note_wait_handler(NOTE_WAIT_TAKE, NW_BIT(NOTE_BIT_KILL), 0, 0u, 0), -(s64)T_E_INVAL,
                   "kill is nobody's to take");
    TEST_EXPECT_EQ(sys_note_wait_handler(NOTE_WAIT_POLL, 0, 0, 0u, POLL_MAX_NFDS + 1u), -(s64)T_E_INVAL,
                   "nfds over the bound");
    TEST_EXPECT_EQ(sys_note_wait_handler(NOTE_WAIT_POLL, 0, (u64)-16ll, 0u, 1u), -(s64)T_E_FAULT,
                   "a pollfd array that is not the user's");
    TEST_EXPECT_EQ(sys_note_wait_handler(NOTE_WAIT_PENDING, 1u, 0, 0u, 0), -(s64)T_E_INVAL,
                   "pending takes no mask");
    TEST_EXPECT_EQ(sys_note_wait_handler(NOTE_WAIT_PENDING, 0, 0x1000u, 0u, 0), -(s64)T_E_INVAL,
                   "pending takes no address");
    TEST_EXPECT_EQ(sys_note_wait_handler(NOTE_WAIT_PENDING, 0, 0, 1u, 0), -(s64)T_E_INVAL,
                   "pending takes no timeout");
    TEST_EXPECT_EQ(sys_note_wait_handler(NOTE_WAIT_TAKE, NW_USR1, (u64)-16ll, 0u, 0), -(s64)T_E_FAULT,
                   "a record the kernel may not write");
}

void test_vivarium_timespec_to_ns(void) {
    s64 ns = -1;
    s32 err = 0;
    TEST_ASSERT(vivarium_timespec_to_ns(2, 5, &ns, &err), "valid");
    TEST_EXPECT_EQ(ns, 2000000005ll, "seconds and nanoseconds");
    TEST_ASSERT(vivarium_timespec_to_ns(0, 0, &ns, &err), "zero is a poll");
    TEST_EXPECT_EQ(ns, 0ll, "and stays zero");
    TEST_ASSERT(!vivarium_timespec_to_ns(-1, 0, &ns, &err), "negative seconds");
    TEST_EXPECT_EQ(err, (s32)T_E_INVAL, "EINVAL");
    TEST_ASSERT(!vivarium_timespec_to_ns(0, 1000000000, &ns, &err), "nanoseconds past a second");
    TEST_EXPECT_EQ(err, (s32)T_E_INVAL, "EINVAL");
    TEST_ASSERT(!vivarium_timespec_to_ns(0, -1, &ns, &err), "negative nanoseconds");
    TEST_ASSERT(vivarium_timespec_to_ns(0x7FFFFFFFFFFFFFFFll, 999999999, &ns, &err), "the largest");
    TEST_EXPECT_EQ(ns, 0x7FFFFFFFFFFFFFFFll, "saturates: a negative result would wait forever");
}
