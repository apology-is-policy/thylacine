// P6-pouch-signals-impl (sub-chunk 13a) — tests for the kernel notes substrate.
//
// Coverage focuses on the queue + post + dequeue invariants (N-1..N-5)
// and the synthetic-poster helpers. The async-handler-on-EL0-return path
// is exercised end-to-end by the future /pouch-hello-signals proving
// binary (sub-chunk 13b); these kernel tests cover the leaf-level
// substrate.
//
// Tests:
//   notes.queue_alloc_free_smoke      — alloc + free without leak
//   notes.post_dequeue_smoke          — single post → single dequeue
//   notes.post_ordering               — three posts dequeued in order
//   notes.unknown_name_rejected       — notes_post with unknown name → -EINVAL
//   notes.snare_forge_rejected        — user-path snare:* post → -EINVAL (ABI)
//   notes.queue_full_returns_eagain   — non-synthetic posts fail at full: -EAGAIN
//   notes.coalesce_synthetic          — synthetic poster merges at threshold
//   notes.mask_defers                 — masked entries skipped at dequeue
//   notes.kill_dequeue_smoke          — kill dequeues normally (no special
//                                        kernel-side action; the EL0-return-
//                                        tail dispatch is what makes it
//                                        non-catchable at delivery time)
//   notes.post_child_exit_helper      — synthetic helper packs arg
//   notes.post_pipe_helper            — synthetic helper smoke
//   notes.proc_lifecycle              — proc_alloc gives a non-NULL queue;
//                                        proc_free cleans up
//   notes.peek_does_not_pop           — peek leaves count unchanged

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/notes.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>   // #97: struct t_stat + T_S_IFCHR
#include <thylacine/thread.h>
#include <thylacine/types.h>
#include <thylacine/vivarium.h>     // V-6b: the Linux disposition discard

#include "../../arch/arm64/exception.h"  // #15: struct exception_context
#include "../../arch/arm64/timer.h"
#include "../../mm/slub.h"          // V-6b: a real sigtab, so proc_free frees it

// ---------------------------------------------------------------------------
// queue_alloc_free_smoke
// ---------------------------------------------------------------------------

void test_notes_queue_alloc_free_smoke(void) {
    struct NoteQueue *q = notes_queue_alloc();
    TEST_ASSERT(q != NULL, "notes_queue_alloc returned non-NULL");
    TEST_EXPECT_EQ(q->count, 0u, "fresh queue has count == 0");
    TEST_EXPECT_EQ(q->head, 0u, "fresh queue head == 0");
    TEST_EXPECT_EQ(q->tail, 0u, "fresh queue tail == 0");
    notes_queue_free(q);
}

// ---------------------------------------------------------------------------
// post_dequeue_smoke
// ---------------------------------------------------------------------------

void test_notes_post_dequeue_smoke(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    TEST_ASSERT(p->notes != NULL, "proc_alloc populated notes queue");

    int rc = notes_post(p, "interrupt", 0u, NULL, true);
    TEST_EXPECT_EQ(rc, 0, "notes_post(interrupt) returned 0");
    TEST_EXPECT_EQ(p->notes->count, 1u, "queue count == 1 after post");

    struct Note got;
    spin_lock(&p->notes->lock);
    int popped = notes_dequeue_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(popped, 1, "notes_dequeue_locked returned 1");
    TEST_ASSERT(got.name[0] == 'i' && got.name[1] == 'n', "popped name starts with 'in'");
    TEST_EXPECT_EQ(p->notes->count, 0u, "queue empty after dequeue");

    // Cleanup. proc_free requires ZOMBIE; this is a test fixture so we
    // poke the state directly (mirrors test_torpor's pattern).
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// post_ordering
// ---------------------------------------------------------------------------

void test_notes_post_ordering(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");

    TEST_EXPECT_EQ(notes_post(p, "interrupt", 1u, NULL, true), 0, "post 1");
    TEST_EXPECT_EQ(notes_post(p, "pipe",      2u, NULL, true), 0, "post 2");
    TEST_EXPECT_EQ(notes_post(p, "child_exit",3u, NULL, true), 0, "post 3");
    TEST_EXPECT_EQ(p->notes->count, 3u, "count == 3");

    struct Note got;
    spin_lock(&p->notes->lock);
    notes_dequeue_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(got.arg, 1u, "first dequeue is post #1");

    spin_lock(&p->notes->lock);
    notes_dequeue_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(got.arg, 2u, "second dequeue is post #2");

    spin_lock(&p->notes->lock);
    notes_dequeue_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(got.arg, 3u, "third dequeue is post #3");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// unknown_name_rejected
// ---------------------------------------------------------------------------

void test_notes_unknown_name_rejected(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");

    TEST_EXPECT_EQ(notes_post(p, "alarm",      0u, NULL, true), -(int)T_E_INVAL,
                   "post(alarm) rejected (deferred at v1.0)");
    TEST_EXPECT_EQ(notes_post(p, "garbage",    0u, NULL, true), -(int)T_E_INVAL,
                   "post(garbage) rejected");
    TEST_EXPECT_EQ(notes_post(p, "",           0u, NULL, true), -(int)T_E_INVAL,
                   "post(empty) rejected");
    TEST_EXPECT_EQ(p->notes->count, 0u, "no rejected post landed in queue");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// snare_forge_rejected
// ---------------------------------------------------------------------------

void test_notes_snare_forge_rejected(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");

    // The ERRORS.md ABI commitment: the snare: prefix is reserved for
    // kernel-synthetic posters. A userspace-path post (synthetic=false,
    // the SYS_POSTNOTE shape) of ANY snare:* name must be rejected —
    // independent of whether snare:* ever joins the supported name set.
    TEST_EXPECT_EQ(notes_post(p, "snare:segv", 0u, NULL, false), -(int)T_E_INVAL,
                   "user-path post(snare:segv) rejected");
    TEST_EXPECT_EQ(notes_post(p, "snare:bus", 0u, NULL, false), -(int)T_E_INVAL,
                   "user-path post(snare:bus) rejected");
    TEST_EXPECT_EQ(notes_post(p, "snare:", 0u, NULL, false), -(int)T_E_INVAL,
                   "user-path post(snare:) rejected");
    TEST_EXPECT_EQ(p->notes->count, 0u, "no forged snare landed in queue");

    // Control (non-vacuous): the same user path lands a supported name.
    TEST_EXPECT_EQ(notes_post(p, "interrupt", 0u, NULL, false), 0,
                   "user-path post(interrupt) lands");
    TEST_EXPECT_EQ(p->notes->count, 1u, "control note queued");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// queue_full_returns_eagain
// ---------------------------------------------------------------------------

void test_notes_queue_full_returns_eagain(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");

    // Fill the queue with NOTE_QUEUE_DEPTH userspace-style posts (synthetic
    // = false; no coalesce). Use unique-arg posts so the queue holds
    // NOTE_QUEUE_DEPTH distinct entries. We use the same name "interrupt"
    // — synthetic=false skips coalesce regardless of name, so each call
    // either succeeds (adds an entry) or hits queue-full.
    for (u32 i = 0; i < NOTE_QUEUE_DEPTH; i++) {
        TEST_EXPECT_EQ(notes_post(p, "interrupt", i, NULL, false), 0,
                       "fill: post succeeded");
    }
    TEST_EXPECT_EQ(p->notes->count, NOTE_QUEUE_DEPTH,
                   "queue at full depth");

    // Next non-synthetic post must fail.
    TEST_EXPECT_EQ(notes_post(p, "interrupt", 99u, NULL, false), -(int)T_E_AGAIN,
                   "post at full -> -EAGAIN");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// coalesce_synthetic
// ---------------------------------------------------------------------------

void test_notes_coalesce_synthetic(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");

    // Fill past the coalesce threshold with synthetic posts. The first
    // NOTE_COALESCE_THRESHOLD posts enqueue normally; once count reaches
    // the threshold, the SAME-name same-source post overwrites the
    // already-queued entry's arg (the head-of-bucket; FIFO position
    // preserved). We then keep posting; each new arg overwrites in place.
    for (u32 i = 0; i < NOTE_COALESCE_THRESHOLD; i++) {
        TEST_EXPECT_EQ(notes_post(p, "child_exit", i, NULL, true), 0,
                       "fill below threshold");
    }
    TEST_EXPECT_EQ(p->notes->count, NOTE_COALESCE_THRESHOLD,
                   "queue at coalesce threshold");

    // Subsequent synthetic posts of the SAME name coalesce — count
    // stays at threshold, arg of the first entry updates.
    TEST_EXPECT_EQ(notes_post(p, "child_exit", 1000u, NULL, true), 0,
                   "coalesce post 1 succeeded");
    TEST_EXPECT_EQ(notes_post(p, "child_exit", 2000u, NULL, true), 0,
                   "coalesce post 2 succeeded");
    TEST_EXPECT_EQ(p->notes->count, NOTE_COALESCE_THRESHOLD,
                   "queue still at threshold after coalesce posts");

    // The head entry's arg now reflects the LAST coalesce update.
    // (The first matching entry from head — same (name, sender) — was
    // updated; since all our posts share name="child_exit" sender=NULL,
    // the head entry is the one we walked to.)
    struct Note got;
    spin_lock(&p->notes->lock);
    notes_dequeue_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(got.arg, 2000u, "head entry's arg = latest coalesce");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// mask_defers
// ---------------------------------------------------------------------------

void test_notes_mask_defers(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");

    notes_post(p, "interrupt", 0u, NULL, true);
    notes_post(p, "pipe", 0u, NULL, true);

    // Simulate a Thread with interrupt masked (bit 0).
    struct Thread fake_t;
    fake_t.note_direct = 0;
    fake_t.note_mask = (1u << NOTE_BIT_INTERRUPT);

    // Dequeue with the masked Thread: skip "interrupt", return "pipe".
    struct Note got;
    spin_lock(&p->notes->lock);
    int popped = notes_dequeue_locked(p, &fake_t, &got);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(popped, 1, "dequeue returned 1 (skipped masked)");
    TEST_ASSERT(got.name[0] == 'p' && got.name[1] == 'i',
                "popped 'pipe' (interrupt was masked)");
    TEST_EXPECT_EQ(p->notes->count, 1u, "interrupt still queued");

    // Clear mask; now "interrupt" dequeues.
    fake_t.note_mask = 0;
    spin_lock(&p->notes->lock);
    popped = notes_dequeue_locked(p, &fake_t, &got);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(popped, 1, "dequeue interrupt after unmask");
    TEST_ASSERT(got.name[0] == 'i', "popped 'interrupt'");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// kill_dequeue_smoke
// ---------------------------------------------------------------------------

void test_notes_kill_dequeue_smoke(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");

    notes_post(p, "kill", 0u, NULL, true);
    struct Note got;
    spin_lock(&p->notes->lock);
    int popped = notes_dequeue_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(popped, 1, "kill dequeued");
    TEST_ASSERT(got.name[0] == 'k' && got.name[1] == 'i',
                "popped name is 'kill'");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// kill_bypasses_mask (F2 audit regression)
// ---------------------------------------------------------------------------
//
// Per ARCH §7.6.7 N-4: a `kill` note must be deliverable regardless of
// the calling Thread's note_mask. The prior implementation walked the
// queue once with the mask filter, so a Thread with NOTE_BIT_KILL set
// would skip kill entries entirely — defeating SIGKILL semantics. The
// fix: peek/dequeue scan for kill first regardless of mask.

void test_notes_kill_bypasses_mask(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");

    // Queue an interrupt then a kill (in that order).
    notes_post(p, "interrupt", 0u, NULL, true);
    notes_post(p, "kill",      0u, NULL, true);

    // Thread with EVERY supported bit masked.
    struct Thread fake_t;
    fake_t.note_direct = 0;
    fake_t.note_mask = (1u << NOTE_BIT_INTERRUPT) | (1u << NOTE_BIT_KILL) |
                       (1u << NOTE_BIT_PIPE) | (1u << NOTE_BIT_CHILD_EXIT);

    // Peek must find kill first regardless of mask.
    struct Note got;
    spin_lock(&p->notes->lock);
    int has = notes_peek_locked(p, &fake_t, &got);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(has, 1, "peek with all-masked thread finds kill anyway");
    TEST_ASSERT(got.name[0] == 'k' && got.name[1] == 'i',
                "kill peeked over interrupt despite mask");

    // Dequeue must also find kill first.
    spin_lock(&p->notes->lock);
    int popped = notes_dequeue_locked(p, &fake_t, &got);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(popped, 1, "dequeue finds kill despite mask");
    TEST_ASSERT(got.name[0] == 'k', "dequeued name is kill");

    // After kill is popped, the masked interrupt is still queued (since
    // it's masked, this Thread can't dequeue it). count should be 1.
    TEST_EXPECT_EQ(p->notes->count, 1u, "interrupt still queued (masked)");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// fd_read_skips_kill (R2-F1 audit regression)
// ---------------------------------------------------------------------------
//
// Per ARCH §7.6.7 N-4: kill is non-catchable AND must bypass the fd-read
// path. devnotes_read uses notes_dequeue_for_fd_locked which skips kill
// entries entirely; only the EL0-return-tail dispatcher (which uses
// notes_dequeue_locked) may pop kill. Without this, a Proc reading
// /dev/notes could consume its own kill and remain alive.

void test_notes_fd_read_skips_kill(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");

    // Queue: [kill, interrupt]. The fd-read dequeue must SKIP kill and
    // return interrupt.
    notes_post(p, "kill",      0u, NULL, true);
    notes_post(p, "interrupt", 5u, NULL, true);

    struct Note got;
    spin_lock(&p->notes->lock);
    int popped = notes_dequeue_for_fd_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(popped, 1, "fd dequeue returned 1 (interrupt; skipped kill)");
    TEST_ASSERT(got.name[0] == 'i' && got.name[1] == 'n',
                "fd dequeue popped interrupt, not kill");
    TEST_EXPECT_EQ(p->notes->count, 1u, "kill remains queued for dispatcher");

    // Queue still has the kill -- second fd-dequeue must return 0 (kill
    // is the only entry, and fd-read refuses to pop it).
    spin_lock(&p->notes->lock);
    popped = notes_dequeue_for_fd_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(popped, 0, "fd dequeue returns 0 -- kill not popped");
    TEST_EXPECT_EQ(p->notes->count, 1u, "kill still queued");

    // The dispatcher path CAN pop kill via notes_dequeue_locked.
    spin_lock(&p->notes->lock);
    popped = notes_dequeue_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(popped, 1, "dispatcher dequeue popped kill");
    TEST_ASSERT(got.name[0] == 'k', "dispatcher popped name is kill");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// fd_peek_skips_kill (R2-F6 audit regression)
// ---------------------------------------------------------------------------

void test_notes_fd_peek_skips_kill(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");

    notes_post(p, "kill", 0u, NULL, true);

    struct Note got;
    spin_lock(&p->notes->lock);
    int has = notes_peek_for_fd_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(has, 0,
                   "fd peek returns 0 when only kill is queued");

    // Add an interrupt; fd peek should return it (kill still skipped).
    notes_post(p, "interrupt", 7u, NULL, true);
    spin_lock(&p->notes->lock);
    has = notes_peek_for_fd_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(has, 1, "fd peek finds interrupt past kill");
    TEST_ASSERT(got.name[0] == 'i', "fd peek returned interrupt");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// reenqueue_head_smoke (F5/F6 audit regression — helper used to re-push
// a note on uaccess failure)
// ---------------------------------------------------------------------------

void test_notes_reenqueue_head_smoke(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");

    notes_post(p, "interrupt", 1u, NULL, true);
    notes_post(p, "pipe",      2u, NULL, true);

    // Pop the head (interrupt).
    struct Note popped;
    spin_lock(&p->notes->lock);
    notes_dequeue_locked(p, NULL, &popped);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(popped.arg, 1u, "popped interrupt arg=1");
    TEST_EXPECT_EQ(p->notes->count, 1u, "count == 1 after pop");

    // Re-enqueue at head. Order should be restored: interrupt then pipe.
    spin_lock(&p->notes->lock);
    int rc = notes_reenqueue_head_locked(p->notes, &popped);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(rc, 0, "reenqueue_head returned 0");
    TEST_EXPECT_EQ(p->notes->count, 2u, "count back to 2");

    // Now dequeue twice — should see interrupt then pipe.
    struct Note got;
    spin_lock(&p->notes->lock);
    notes_dequeue_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(got.arg, 1u, "first dequeue is interrupt (re-enqueued at head)");

    spin_lock(&p->notes->lock);
    notes_dequeue_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(got.arg, 2u, "second dequeue is pipe");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// post_child_exit_helper
// ---------------------------------------------------------------------------

void test_notes_post_child_exit_helper(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");

    notes_post_child_exit(p, 42, 7);
    TEST_EXPECT_EQ(p->notes->count, 1u, "child_exit posted");

    struct Note got;
    spin_lock(&p->notes->lock);
    notes_dequeue_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_ASSERT(got.name[0] == 'c', "name is 'child_exit'");
    TEST_EXPECT_EQ(got.arg, ((u32)42 << 16) | 7u, "arg packs (pid, status)");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// post_pipe_helper
// ---------------------------------------------------------------------------

void test_notes_post_pipe_helper(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");

    notes_post_pipe(p);
    TEST_EXPECT_EQ(p->notes->count, 1u, "pipe note posted");
    // #237: the pipe post arms the terminate latch (a fresh Proc has no handler
    // + is not self-managing) -- pipe is now a terminate-class note.
    TEST_ASSERT(proc_intr_terminate_pending(p),
                "#237: pipe post arms the terminate latch");

    struct Note got;
    spin_lock(&p->notes->lock);
    notes_dequeue_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_ASSERT(got.name[0] == 'p' && got.name[1] == 'i',
                "name is 'pipe'");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// proc_lifecycle
// ---------------------------------------------------------------------------

void test_notes_proc_lifecycle(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    TEST_ASSERT(p->notes != NULL, "p->notes non-NULL after proc_alloc");
    TEST_EXPECT_EQ(p->handler_va, 0ull, "fresh proc has handler_va == 0");

    // Post a few notes to confirm the queue is usable.
    notes_post(p, "interrupt", 0u, NULL, true);
    notes_post(p, "pipe", 0u, NULL, true);
    TEST_EXPECT_EQ(p->notes->count, 2u, "queue holds 2 entries");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
    // proc_free's notes_queue_free should free the queue. There's no
    // direct probe; the smoke is that proc_free returned without
    // extinction.
}

// ---------------------------------------------------------------------------
// peek_does_not_pop
// ---------------------------------------------------------------------------

void test_notes_peek_does_not_pop(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");

    notes_post(p, "interrupt", 7u, NULL, true);
    TEST_EXPECT_EQ(p->notes->count, 1u, "one entry");

    struct Note peeked;
    spin_lock(&p->notes->lock);
    int has = notes_peek_locked(p, NULL, &peeked);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(has, 1, "peek returned 1");
    TEST_EXPECT_EQ(peeked.arg, 7u, "peek saw arg=7");
    TEST_EXPECT_EQ(p->notes->count, 1u, "count unchanged after peek");

    // Second peek still observes the same entry.
    spin_lock(&p->notes->lock);
    has = notes_peek_locked(p, NULL, &peeked);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(has, 1, "second peek still returns 1");
    TEST_EXPECT_EQ(p->notes->count, 1u, "count still 1");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// interrupt_terminate_gate (LS-5 P2)
// ---------------------------------------------------------------------------
//
// The full truth table of notes_interrupt_should_terminate_locked -- the pure
// decision the EL0-return-tail uses to default-terminate an uncaught
// `interrupt`. The dispatcher itself calls the noreturn exits() on a `true`
// result, so the unit test drives the decision function directly.

void test_notes_interrupt_terminate_gate(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    TEST_ASSERT(p->notes != NULL, "notes queue present");

    // Fresh Proc: no async handler (KP_ZERO handler_va), not self-managing,
    // empty queue -> nothing to terminate for.
    spin_lock(&p->notes->lock);
    int d = notes_interrupt_should_terminate_locked(p, NULL);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(d, 0, "empty queue -> no terminate");

    // A non-interrupt note alone (child_exit) does NOT terminate -- only
    // `interrupt` newly default-terminates; child_exit stays queued.
    notes_post(p, "child_exit", 0u, NULL, true);
    spin_lock(&p->notes->lock);
    d = notes_interrupt_should_terminate_locked(p, NULL);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(d, 0, "only child_exit -> no terminate");

    // Queue an `interrupt` BEHIND the child_exit. The scan finds it regardless
    // of FIFO position -> an unmanaged, handler-less Proc terminates.
    notes_post(p, "interrupt", 0u, NULL, true);
    spin_lock(&p->notes->lock);
    d = notes_interrupt_should_terminate_locked(p, NULL);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(d, 1, "unmanaged + queued interrupt -> terminate");

    // A registered async handler catches interrupt (the async-delivery path
    // runs it) -> never auto-terminate.
    p->handler_va = 0x1000u;
    spin_lock(&p->notes->lock);
    d = notes_interrupt_should_terminate_locked(p, NULL);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(d, 0, "handler registered -> no terminate");
    p->handler_va = 0u;

    // A Thread with `interrupt` masked: not deliverable -> no terminate.
    struct Thread fake_t;
    fake_t.note_direct = 0;
    fake_t.note_mask = (1u << NOTE_BIT_INTERRUPT);
    spin_lock(&p->notes->lock);
    d = notes_interrupt_should_terminate_locked(p, &fake_t);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(d, 0, "masked interrupt -> no terminate");

    // Same Thread, mask cleared: deliverable again -> terminate.
    fake_t.note_mask = 0u;
    spin_lock(&p->notes->lock);
    d = notes_interrupt_should_terminate_locked(p, &fake_t);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(d, 1, "unmasked thread + queued interrupt -> terminate");

    // A self-managing Proc (opened its notes fd) consumes its own notes ->
    // exempt even with an interrupt queued and no handler.
    p->state = PROC_STATE_ALIVE;
    proc_mark_self_managing_notes(p);
    spin_lock(&p->notes->lock);
    d = notes_interrupt_should_terminate_locked(p, NULL);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(d, 0, "self-managing -> exempt from terminate");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// self_managing_flag (LS-5 P2)
// ---------------------------------------------------------------------------
//
// proc_mark_self_managing_notes / proc_is_self_managing_notes round-trip.

void test_notes_self_managing_flag(void) {
    // Fail-closed: a NULL Proc reads as NOT self-managing.
    TEST_EXPECT_EQ(proc_is_self_managing_notes(NULL) ? 1 : 0, 0,
                   "NULL Proc is not self-managing (fail-closed)");

    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");

    // Fresh Proc: not self-managing (KP_ZERO proc_flags).
    TEST_EXPECT_EQ(proc_is_self_managing_notes(p) ? 1 : 0, 0,
                   "fresh Proc is not self-managing");

    p->state = PROC_STATE_ALIVE;
    proc_mark_self_managing_notes(p);
    TEST_EXPECT_EQ(proc_is_self_managing_notes(p) ? 1 : 0, 1,
                   "after mark, Proc is self-managing");

    // One-way + idempotent: a second mark is a no-op (stays set).
    proc_mark_self_managing_notes(p);
    TEST_EXPECT_EQ(proc_is_self_managing_notes(p) ? 1 : 0, 1,
                   "mark is idempotent");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// intr_latch_lifecycle (LS-5c P3-terminate)
// ---------------------------------------------------------------------------
//
// The PROC_FLAG_INTR_TERMINATE_PENDING latch: armed by notes_post's
// interrupt arm (no handler + not self-managing), cleared by handler
// registration (notes_set_handler), the self-managing mark
// (notes_mark_self_managing), and draining the last queued interrupt.

void test_notes_intr_latch_lifecycle(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");

    // (a) An interrupt post to a handler-less non-self-managing Proc ARMS.
    TEST_EXPECT_EQ(notes_post(p, "interrupt", 0u, NULL, true), 0, "post 1");
    TEST_ASSERT(proc_intr_terminate_pending(p),
                "interrupt post armed the latch");

    // (b) Registering a handler clears it (under q->lock, serialized with
    // the arm -- the SYS_NOTIFY-vs-post race close).
    notes_set_handler(p, 0x1000u);
    TEST_ASSERT(!proc_intr_terminate_pending(p),
                "handler registration cleared the latch");

    // (c) With a handler registered, a second post does NOT re-arm.
    TEST_EXPECT_EQ(notes_post(p, "interrupt", 0u, NULL, true), 0, "post 2");
    TEST_ASSERT(!proc_intr_terminate_pending(p),
                "handler-bearing Proc never arms");

    // (d) Unregistering re-reads what is queued (ARCH 8.8.3): the two interrupts
    // posted past the handler have no handler now, and a wait must not ride them out.
    notes_set_handler(p, 0u);
    TEST_ASSERT(proc_intr_terminate_pending(p),
                "unregister arms from the queue");
    TEST_EXPECT_EQ(notes_post(p, "interrupt", 0u, NULL, true), 0, "post 3");
    TEST_ASSERT(proc_intr_terminate_pending(p),
                "the latch stands across another post");

    // (e) Drain-clear: three interrupts queued; the latch survives popping
    // the first two and clears on the LAST.
    struct Note got;
    spin_lock(&p->notes->lock);
    TEST_EXPECT_EQ(notes_dequeue_for_fd_locked(p, NULL, &got), 1, "pop 1");
    spin_unlock(&p->notes->lock);
    TEST_ASSERT(proc_intr_terminate_pending(p),
                "latch survives a pop with interrupts remaining");
    spin_lock(&p->notes->lock);
    TEST_EXPECT_EQ(notes_dequeue_for_fd_locked(p, NULL, &got), 1, "pop 2");
    spin_unlock(&p->notes->lock);
    TEST_ASSERT(proc_intr_terminate_pending(p), "latch survives pop 2");
    spin_lock(&p->notes->lock);
    TEST_EXPECT_EQ(notes_dequeue_for_fd_locked(p, NULL, &got), 1, "pop 3");
    spin_unlock(&p->notes->lock);
    TEST_ASSERT(!proc_intr_terminate_pending(p),
                "draining the last interrupt cleared the latch");

    // (f) The self-managing mark clears + suppresses future arms.
    TEST_EXPECT_EQ(notes_post(p, "interrupt", 0u, NULL, true), 0, "post 4");
    TEST_ASSERT(proc_intr_terminate_pending(p), "post 4 armed");
    p->state = PROC_STATE_ALIVE;
    notes_mark_self_managing(p);
    TEST_ASSERT(!proc_intr_terminate_pending(p),
                "self-managing mark cleared the latch");
    TEST_EXPECT_EQ(notes_post(p, "interrupt", 0u, NULL, true), 0, "post 5");
    TEST_ASSERT(!proc_intr_terminate_pending(p),
                "self-managing Proc never arms");

    // (g) Non-terminate names never arm (fresh Proc -- p carries the
    // self-managing mark from (f)). #237 made `pipe` terminate-class (it now
    // arms), so the non-arming stand-in here is tty:winch (IGNORE class); pipe's
    // arming is covered by notes.pipe_die_pending + the latch-map test.
    struct Proc *p2 = proc_alloc();
    TEST_ASSERT(p2 != NULL, "proc_alloc p2 succeeded");
    TEST_EXPECT_EQ(notes_post(p2, "child_exit", 0u, NULL, true), 0,
                   "post child_exit");
    TEST_EXPECT_EQ(notes_post(p2, NOTE_NAME_TTY_WINCH, 0u, NULL, true), 0,
                   "post tty:winch");
    TEST_ASSERT(!proc_intr_terminate_pending(p2),
                "non-terminate posts never arm");

    // (h) The kproc guard: an interrupt post to kproc's queue never arms
    // (in-kernel tests post to kproc's queue via the boot thread; an armed
    // kproc would *_INTR every kernel-thread sleep). Only when the boot
    // queue is empty, so the drain leaves it exactly as found.
    struct Proc *kp = kproc();
    if (kp && kp->notes) {
        spin_lock(&kp->notes->lock);
        u32 pre = kp->notes->count;
        spin_unlock(&kp->notes->lock);
        if (pre == 0u) {
            TEST_EXPECT_EQ(notes_post(kp, "interrupt", 0u, NULL, true), 0,
                           "post to kproc queue accepted");
            TEST_ASSERT(!proc_intr_terminate_pending(kp),
                        "kproc never arms (the guard)");
            struct Note kgot;
            spin_lock(&kp->notes->lock);
            int kpop = notes_dequeue_for_fd_locked(kp, NULL, &kgot);
            spin_unlock(&kp->notes->lock);
            TEST_EXPECT_EQ(kpop, 1, "kproc queue drained back to empty");
        }
    }

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
    p2->state = PROC_STATE_ZOMBIE;
    proc_free(p2);
}

// ---------------------------------------------------------------------------
// die_pending_predicate (LS-5c P3-terminate)
// ---------------------------------------------------------------------------
//
// thread_die_pending — the widened #811 sleep predicate: group-exit death
// (mask-blind) OR the terminate latch gated by the thread's own mask.

void test_notes_die_pending_predicate(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");

    // The predicate reads t->proc + t->note_mask + t->exit_close_active
    // (the LS-5b fake-thread idiom, plus the proc binding; #68 F1 added the
    // exit-close gate, so the flag must be explicitly initialized here).
    struct Thread fake_t;
    fake_t.note_direct = 0;
    fake_t.proc              = p;
    fake_t.note_mask         = 0u;
    fake_t.exit_close_active = false;
    fake_t.kthread_join_active = false;

    TEST_ASSERT(!thread_die_pending(NULL), "NULL thread -> false");
    TEST_ASSERT(!thread_die_pending(&fake_t), "fresh Proc -> false");

    // Latch leg: armed + unmasked -> true; masked -> false (masking defers).
    TEST_EXPECT_EQ(notes_post(p, "interrupt", 0u, NULL, true), 0, "post");
    TEST_ASSERT(thread_die_pending(&fake_t), "armed + unmasked -> true");
    fake_t.note_mask = (1u << NOTE_BIT_INTERRUPT);
    TEST_ASSERT(!thread_die_pending(&fake_t),
                "armed + MASKED -> false (the thread defers)");

    // Death leg: group_exit_msg overrides the mask (death is not deferrable).
    __atomic_store_n(&p->group_exit_msg, "killed", __ATOMIC_RELEASE);
    TEST_ASSERT(thread_die_pending(&fake_t),
                "group exit -> true even with interrupt masked");

    // #68 F1: the exit-close window suppresses BOTH legs -- the last-out
    // closer's sends/waits must behave like a live thread's even though
    // group_exit_msg is set (every SYS_EXIT_GROUP sets it; without this the
    // dev9p write-behind close-flush dropped its staged bytes and the
    // close-time Tclunk was never sent).
    fake_t.exit_close_active = true;
    TEST_ASSERT(!thread_die_pending(&fake_t),
                "exit-close window -> false even with group_exit_msg set");
    fake_t.note_mask = 0u; // the latch leg too (interrupt armed above)
    TEST_ASSERT(!thread_die_pending(&fake_t),
                "exit-close window -> false even with the latch armed");
    // Round-2 F1: the LATCH-ONLY case (gmsg NULL) -- the LS-5 interrupt
    // default-terminate calls exits() with the latch deliberately still
    // armed and NO group_exit_msg; the flag must suppress that leg alone.
    __atomic_store_n(&p->group_exit_msg, (const char *)NULL, __ATOMIC_RELEASE);
    TEST_ASSERT(!thread_die_pending(&fake_t),
                "exit-close window -> false with the latch armed, gmsg NULL");
    __atomic_store_n(&p->group_exit_msg, "killed", __ATOMIC_RELEASE);
    fake_t.exit_close_active = false;
    TEST_ASSERT(thread_die_pending(&fake_t),
                "flag cleared -> the death leg reads true again");

    __atomic_store_n(&p->group_exit_msg, (const char *)NULL, __ATOMIC_RELEASE);
    fake_t.note_mask = 0u;

    // Drain the interrupt -> the latch clears -> false again.
    struct Note got;
    spin_lock(&p->notes->lock);
    (void)notes_dequeue_for_fd_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_ASSERT(!thread_die_pending(&fake_t), "drained -> false");

    // RW-0 F3 (defense-in-depth): the latch leg never fires for kproc, even
    // with the flag FORCED onto proc_flags (the arm site refuses kproc; this
    // guards a future arm path that forgets to). Forced directly because
    // notes_post's arm cannot set it.
    fake_t.proc = kproc();
    __atomic_or_fetch(&kproc()->proc_flags, PROC_FLAG_INTR_TERMINATE_PENDING,
                      __ATOMIC_RELEASE);
    TEST_ASSERT(!thread_die_pending(&fake_t),
                "kproc latch leg guarded even when forced (RW-0 F3)");
    __atomic_and_fetch(&kproc()->proc_flags,
                       ~PROC_FLAG_INTR_TERMINATE_PENDING, __ATOMIC_RELEASE);

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// thread_death_reaches -- can a death end this thread's waits? A user
// thread's, yes; not a kernel thread's, not one inside an exit close, and not
// a thread with no Proc.
void test_notes_death_reaches_predicate(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    struct Thread fake_t;
    fake_t.note_direct = 0;
    fake_t.proc              = p;
    fake_t.exit_close_active = false;
    fake_t.kthread_join_active = false;
    bool user = thread_death_reaches(&fake_t);
    fake_t.exit_close_active = true;
    bool ecl  = thread_death_reaches(&fake_t);
    fake_t.exit_close_active = false;
    fake_t.proc = kproc();
    bool kth  = thread_death_reaches(&fake_t);
    fake_t.proc = NULL;
    bool none = thread_death_reaches(&fake_t);
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);

    TEST_ASSERT(user, "a user thread: a death reaches it");
    TEST_ASSERT(!ecl, "inside an exit close: no death reaches it");
    TEST_ASSERT(!kth, "a kernel thread: no death reaches it");
    TEST_ASSERT(!none && !thread_death_reaches(NULL), "no Proc, no thread: false");
}

// ARCH 7.9.1 part B: a kill that forces the final close lifts its hold on
// death -- every death leg reaches it again -- but never loom_free's kthread
// join, and never caught delivery. The control, one variable away: the same
// exit close in a dying group, unforced.
void test_notes_forced_close_lifts_the_hold(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    __atomic_store_n(&p->group_exit_msg, "killed", __ATOMIC_RELEASE);
    struct Thread fake_t;
    fake_t.note_direct = 0;
    fake_t.proc                = p;
    fake_t.note_mask           = 0u;
    fake_t.exit_close_active   = true;
    fake_t.kthread_join_active = false;
    bool held_die   = thread_die_pending(&fake_t);
    bool held_group = thread_group_death_pending(&fake_t);
    bool held_reach = thread_death_reaches(&fake_t);

    __atomic_or_fetch(&p->proc_flags, PROC_FLAG_EXIT_CLOSE_FORCED, __ATOMIC_RELEASE);
    bool f_die    = thread_die_pending(&fake_t);
    bool f_group  = thread_group_death_pending(&fake_t);
    bool f_reach  = thread_death_reaches(&fake_t);
    bool f_caught = thread_caught_note_deliverable(&fake_t);

    fake_t.kthread_join_active = true;
    bool j_die   = thread_die_pending(&fake_t);
    bool j_group = thread_group_death_pending(&fake_t);
    bool j_reach = thread_death_reaches(&fake_t);
    fake_t.exit_close_active = false;             // a peer's close, outside the exit
    bool jp_die  = thread_die_pending(&fake_t);

    __atomic_and_fetch(&p->proc_flags, ~PROC_FLAG_EXIT_CLOSE_FORCED, __ATOMIC_RELEASE);
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);

    TEST_ASSERT(!held_die && !held_group && !held_reach,
                "an unforced exit close holds every death (control)");
    TEST_ASSERT(f_die, "forced: the death reaches the close");
    TEST_ASSERT(f_group, "forced: so does the group-death leg");
    TEST_ASSERT(f_reach, "forced: thread_death_reaches agrees");
    TEST_ASSERT(!f_caught, "forced: still no caught delivery");
    TEST_ASSERT(!j_die && !j_group && !j_reach,
                "the kthread join holds every death, forced or not");
    TEST_ASSERT(!jp_die, "and outside the exit close too");
}

// ---------------------------------------------------------------------------
// pipe_die_pending (#237) -- the pipe terminate latch at the sleep predicate
// ---------------------------------------------------------------------------
//
// The regression proof for #237: BEFORE the fix `pipe` carried no latch, so
// thread_die_pending was FALSE for an armed pipe note -- this test's first live
// assertion fails on pre-#237 code. It also pins the three properties the
// native mask relies on: (a) masking NOTE_BIT_PIPE defers (the EPIPE-not-death
// path libthyla-rs/libt/the Go rt0 take), (b) the mask is PER-FAMILY (an
// interrupt mask does not defer a pipe note -- the reason thread_die_pending
// keeps per-latch ifs rather than the whole-class MASK), and (c) the wake gate
// (proc_intr_terminate_pending) fires on pipe, so a pipe-terminate latched on a
// syscall-blocked thread is actually woken.
void test_notes_pipe_die_pending(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");

    struct Thread fake_t;
    fake_t.note_direct = 0;
    fake_t.proc              = p;
    fake_t.note_mask         = 0u;
    fake_t.exit_close_active = false;
    fake_t.kthread_join_active = false;

    TEST_ASSERT(!thread_die_pending(&fake_t), "fresh Proc -> false");

    // A write to a closed pipe posts exactly this note (kernel/notes.c pipe arm).
    TEST_EXPECT_EQ(notes_post(p, "pipe", 0u, NULL, true), 0, "post pipe");
    TEST_ASSERT(proc_intr_terminate_pending(p),
                "pipe post fires the wake gate (#237: PIPE in the class mask)");
    TEST_ASSERT(thread_die_pending(&fake_t),
                "armed + unmasked -> true (the #237 behaviour; FALSE pre-fix)");

    // Masking NOTE_BIT_PIPE defers -- the native-runtime EPIPE-not-death path.
    fake_t.note_mask = (1u << NOTE_BIT_PIPE);
    TEST_ASSERT(!thread_die_pending(&fake_t),
                "armed + pipe MASKED -> false (libthyla-rs / libt / Go mask it)");

    // Per-family precision: masking an UNRELATED family never defers pipe.
    fake_t.note_mask = (1u << NOTE_BIT_INTERRUPT);
    TEST_ASSERT(thread_die_pending(&fake_t),
                "interrupt masked, pipe unmasked -> true (per-family latch)");

    // Death overrides the pipe mask (death is not deferrable).
    fake_t.note_mask = (1u << NOTE_BIT_PIPE);
    __atomic_store_n(&p->group_exit_msg, "killed", __ATOMIC_RELEASE);
    TEST_ASSERT(thread_die_pending(&fake_t),
                "group exit -> true even with pipe masked");
    __atomic_store_n(&p->group_exit_msg, (const char *)NULL, __ATOMIC_RELEASE);
    fake_t.note_mask = 0u;

    // The GENERIC drain clears the pipe latch: notes_drain_intr_locked is a
    // per-class computed ~latch, so #237 added no drain code -- draining the
    // last pipe note clears PROC_FLAG_PIPE_TERMINATE_PENDING for free.
    struct Note got;
    spin_lock(&p->notes->lock);
    (void)notes_dequeue_for_fd_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_ASSERT(!thread_die_pending(&fake_t), "drained -> latch clears -> false");
    TEST_ASSERT(!proc_intr_terminate_pending(p), "drained -> wake gate clears");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// term_die_pending -- SIGTERM's own note (the signal chunk's S-2)
// ---------------------------------------------------------------------------
//
// Pins the `term` row (default TERMINATE, death SIGTERM), its OWN latch, and the
// per-family pairing the note exists for: a thread that masked interrupt (SIGINT
// blocked) still unwinds for term, one that masked term does not, and term never
// arms interrupt's latch. Red before S-2, where "term" was no note at all and the
// post itself failed. Every reading is taken before the fixture is released, and
// asserted after.
void test_notes_term_die_pending(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");

    struct Thread fake_t;
    fake_t.note_direct = 0;
    fake_t.proc                = p;
    fake_t.note_mask           = 0u;
    fake_t.exit_close_active   = false;
    fake_t.kthread_join_active = false;

    bool fresh = thread_die_pending(&fake_t);
    int  dfl   = (int)notes_default_action(NOTE_NAME_TERM);
    int  sig   = notes_death_signal(NOTE_NAME_TERM);
    int  post  = notes_post(p, NOTE_NAME_TERM, 0u, NULL, false);
    u32  flags = __atomic_load_n(&p->proc_flags, __ATOMIC_ACQUIRE);
    bool gate  = proc_intr_terminate_pending(p);
    bool open_ = thread_die_pending(&fake_t);
    fake_t.note_mask = (1u << NOTE_BIT_INTERRUPT);
    bool intr_masked = thread_die_pending(&fake_t);
    fake_t.note_mask = (1u << NOTE_BIT_TERM);
    bool term_masked = thread_die_pending(&fake_t);
    fake_t.note_mask = 0u;
    struct Note got;
    got.name[0] = 0;
    spin_lock(&p->notes->lock);
    int deq = notes_dequeue_for_fd_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    bool got_term = got.name[0] == 't' && got.name[1] == 'e' && got.name[2] == 'r' &&
                    got.name[3] == 'm' && got.name[4] == 0;
    bool drained  = thread_die_pending(&fake_t);

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);

    TEST_ASSERT(!fresh, "fresh Proc -> false");
    TEST_EXPECT_EQ(dfl, (int)NOTE_DFL_TERMINATE, "term's default action is TERMINATE");
    TEST_EXPECT_EQ(sig, (int)NOTE_SIG_TERM, "a death by term records SIGTERM (15)");
    TEST_EXPECT_EQ(post, 0, "a userspace post of term is accepted");
    TEST_ASSERT((flags & PROC_FLAG_TERM_TERMINATE_PENDING) != 0, "term arms its own latch");
    TEST_ASSERT((flags & PROC_FLAG_INTR_TERMINATE_PENDING) == 0, "and not interrupt's");
    TEST_ASSERT(gate, "the wake gate fires on term");
    TEST_ASSERT(open_, "armed + unmasked -> true");
    TEST_ASSERT(intr_masked, "interrupt masked, term unmasked -> true (per-family)");
    TEST_ASSERT(!term_masked, "term masked -> false");
    TEST_ASSERT(deq >= 0 && got_term, "the queued note is term");
    TEST_ASSERT(!drained, "drained -> the latch clears -> false");
}

// ---------------------------------------------------------------------------
// caught_note_latch_lifecycle (item 11, ARCH §8.8.3)
// ---------------------------------------------------------------------------
//
// The caught latch (Proc.note_caught, CAUGHT_LATCH_MASK): armed by notes_post's caught arm
// (a live handler OR self-managing -- the EXACT COMPLEMENT of the terminate
// arm), cleared by draining the last queued note of that family. The two
// latches are DISJOINT -- a given note arms exactly one.
void test_notes_caught_note_latch_lifecycle(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    p->state = PROC_STATE_ALIVE;

    // (a) An interrupt post to a HANDLER-BEARING Proc arms the CAUGHT latch and
    // NOT the terminate latch -- the complement of test_notes_intr_latch (b).
    notes_set_handler(p, 0x1000u);
    TEST_EXPECT_EQ(notes_post(p, "interrupt", 0u, NULL, true), 0, "post 1");
    TEST_ASSERT(proc_caught_note_pending(p),
                "caught interrupt (handler) armed the caught latch");
    TEST_ASSERT(!proc_intr_terminate_pending(p),
                "a caught interrupt does NOT arm the terminate latch");

    // (b) Draining the last interrupt clears the caught latch.
    struct Note got;
    spin_lock(&p->notes->lock);
    TEST_EXPECT_EQ(notes_dequeue_for_fd_locked(p, NULL, &got), 1, "pop 1");
    spin_unlock(&p->notes->lock);
    TEST_ASSERT(!proc_caught_note_pending(p),
                "draining the last caught note cleared the caught latch");

    // (c) Self-managing is the OTHER caught path: a note to a self-managing
    // Proc (no handler) arms the caught latch, and still refuses the terminate
    // latch.
    notes_set_handler(p, 0u);
    proc_mark_self_managing_notes(p);
    TEST_EXPECT_EQ(notes_post(p, "pipe", 0u, NULL, true), 0, "post pipe");
    TEST_ASSERT(proc_caught_note_pending(p),
                "a note to a self-managing Proc armed the caught latch");
    TEST_ASSERT(!proc_intr_terminate_pending(p),
                "self-managing suppresses the terminate latch (unchanged)");
    spin_lock(&p->notes->lock);
    (void)notes_dequeue_for_fd_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_ASSERT(!proc_caught_note_pending(p), "pipe drained -> caught clear");

    // (d) KILL is EXCLUDED: a kill note never arms the caught latch (it is
    // non-catchable and routes through group_exit_msg, not caught delivery),
    // even on a self-managing Proc.
    struct Proc *pk = proc_alloc();
    TEST_ASSERT(pk != NULL, "proc_alloc pk succeeded");
    pk->state = PROC_STATE_ALIVE;
    proc_mark_self_managing_notes(pk);
    TEST_EXPECT_EQ(notes_post(pk, "kill", 0u, NULL, true), 0, "post kill");
    TEST_ASSERT(!proc_caught_note_pending(pk),
                "a kill note never arms the caught latch (KILL excluded)");

    // (e) The kproc guard: a caught-family post to kproc's queue never arms
    // (the arm inherits notes_post's kproc guard). Only when empty, so the
    // drain leaves the boot queue exactly as found.
    struct Proc *kp = kproc();
    if (kp && kp->notes) {
        spin_lock(&kp->notes->lock);
        u32 pre = kp->notes->count;
        spin_unlock(&kp->notes->lock);
        if (pre == 0u) {
            TEST_EXPECT_EQ(notes_post(kp, "interrupt", 0u, NULL, true), 0,
                           "post to kproc queue accepted");
            TEST_ASSERT(!proc_caught_note_pending(kp),
                        "kproc never arms the caught latch (the guard)");
            struct Note kgot;
            spin_lock(&kp->notes->lock);
            (void)notes_dequeue_for_fd_locked(kp, NULL, &kgot);
            spin_unlock(&kp->notes->lock);
        }
    }

    p->state  = PROC_STATE_ZOMBIE; proc_free(p);
    pk->state = PROC_STATE_ZOMBIE; proc_free(pk);
}

// ---------------------------------------------------------------------------
// caught_note_deliverable_predicate (item 11, ARCH §8.8.3)
// ---------------------------------------------------------------------------
//
// thread_caught_note_deliverable -- the NON-death sleep predicate: a caught
// note of a family unmasked for the thread. The mirror of die_pending, and
// DISJOINT from it.
void test_notes_caught_note_deliverable_predicate(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    p->state = PROC_STATE_ALIVE;

    struct Thread fake_t;
    fake_t.note_direct = 0;
    fake_t.proc               = p;
    fake_t.note_mask          = 0u;
    fake_t.exit_close_active  = false;
    fake_t.kthread_join_active = false;
    fake_t.in_handler         = false;
    fake_t.note_interruptible = true;   // a wait on signal(7)'s list

    TEST_ASSERT(!thread_caught_note_deliverable(NULL), "NULL thread -> false");
    TEST_ASSERT(!thread_caught_note_deliverable(&fake_t), "fresh Proc -> false");

    // A caught (self-managing) note -> deliverable to an unmasked thread;
    // masked -> false (the thread defers, exactly as the terminate latch).
    proc_mark_self_managing_notes(p);
    TEST_EXPECT_EQ(notes_post(p, "interrupt", 0u, NULL, true), 0, "post");
    TEST_ASSERT(thread_caught_note_deliverable(&fake_t),
                "caught + unmasked -> true");

    // ARCH 8.8.3 (signal(7)'s list): the same note does NOT interrupt a wait
    // whose syscall is not on the list -- socket(), open(), a regular file's
    // read(), a page-in. Those ride the note out; only death unwinds them, and
    // death is thread_die_pending's, which this flag never touches.
    fake_t.note_interruptible = false;
    TEST_ASSERT(!thread_caught_note_deliverable(&fake_t),
                "caught + unmasked, but the wait is not interruptible -> false");
    fake_t.note_interruptible = true;
    TEST_ASSERT(thread_caught_note_deliverable(&fake_t),
                "the flag alone decides: interruptible again -> true");
    fake_t.note_mask = (1u << NOTE_BIT_INTERRUPT);
    TEST_ASSERT(!thread_caught_note_deliverable(&fake_t),
                "caught + MASKED -> false (the thread defers)");
    fake_t.note_mask = 0u;

    // arm-2 regression: a caught + unmasked note is NOT deliverable while a
    // handler is running (in_handler), because notes_deliver_at_el0_return's
    // N-3 re-entrancy guard refuses non-kill delivery then. Reporting it
    // deliverable is a read-interrupt LIVELOCK -- the sleep unwinds to EL0 to
    // deliver, the guard re-queues the note, the next read unwinds again, with
    // zero progress (measured: a child_exit stormed a pts read after a pouch
    // shell's SIGINT handler escaped its frame without rt_sigreturn, leaving
    // in_handler stuck). The predicate must agree with the delivery site.
    fake_t.in_handler = true;
    TEST_ASSERT(!thread_caught_note_deliverable(&fake_t),
                "caught + unmasked but in_handler -> false (N-3 would refuse)");
    fake_t.in_handler = false;
    TEST_ASSERT(thread_caught_note_deliverable(&fake_t),
                "in_handler cleared -> deliverable again");

    // DISJOINT from death: a caught note is deliverable but the terminate latch
    // stayed clear (self-managing refused it), and there is no group_exit_msg.
    TEST_ASSERT(!thread_die_pending(&fake_t),
                "a caught note is not a death (terminate latch clear)");

    // The exit-close window suppresses it, the same gate thread_die_pending
    // applies (a closing thread must not EINTR-unwind its orderly close).
    fake_t.exit_close_active = true;
    TEST_ASSERT(!thread_caught_note_deliverable(&fake_t),
                "exit-close window -> false");
    fake_t.exit_close_active = false;

    // Drain -> the family bit clears -> false again.
    struct Note got;
    spin_lock(&p->notes->lock);
    (void)notes_dequeue_for_fd_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_ASSERT(!thread_caught_note_deliverable(&fake_t), "drained -> false");

    // kproc guard: forced caught bits never make a kproc thread deliverable.
    fake_t.proc = kproc();
    __atomic_or_fetch(&kproc()->note_caught, CAUGHT_LATCH_MASK,
                      __ATOMIC_RELEASE);
    TEST_ASSERT(!thread_caught_note_deliverable(&fake_t),
                "kproc guarded even when the caught sub-field is forced");
    __atomic_and_fetch(&kproc()->note_caught, ~CAUGHT_LATCH_MASK,
                       __ATOMIC_RELEASE);

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// caught_note_claim_once (ARCH 8.8.3, VIV-EINTR audit F2)
// ---------------------------------------------------------------------------
//
// One caught note unwinds ONE interruptible sleeper, as Linux interrupts one
// thread for a process-directed signal. thread_caught_note_claim is the sleep
// arms' decision: the first thread claims the family, a second is refused while
// the claim is held, and the claimant's own claim stays open to it. The claim
// ends at the claimant's EL0-return tail, not at the family's drain: released
// while its note is still queued, the note is open to another thread (round-2
// F1 -- a tail that ended short of the note stranded it). Families claim
// independently.
void test_notes_caught_note_claim_once(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    p->state     = PROC_STATE_ALIVE;
    p->phenotype = PHENO_LINUX;
    notes_set_handler(p, 0x1000u);

    static struct Thread a, b, c;       // BSS-zeroed, reset each run
    a.proc = b.proc = c.proc = p;
    a.note_mask = b.note_mask = c.note_mask = 0u;
    a.exit_close_active = b.exit_close_active = c.exit_close_active = false;
    a.in_handler = b.in_handler = c.in_handler = false;
    a.note_interruptible = b.note_interruptible = c.note_interruptible = true;
    a.note_claim = b.note_claim = c.note_claim = 0u;

    bool none0 = thread_caught_note_claim(&a);                  // nothing queued
    int  post1 = notes_post(p, "interrupt", 0u, NULL, true);
    bool a1    = thread_caught_note_claim(&a);
    bool b1    = thread_caught_note_claim(&b);
    bool b1dl  = thread_caught_note_deliverable(&b);            // still pending
    bool a1own = thread_caught_note_claim(&a);                  // a's own claim
    // A second interrupt is QUEUED, not coalesced: the ring coalesces only past
    // NOTE_COALESCE_THRESHOLD.
    int  post2 = notes_post(p, "interrupt", 0u, NULL, true);
    bool b2    = thread_caught_note_claim(&b);
    int  post3 = notes_post(p, "child_exit", 0u, NULL, true);   // another family
    bool b3    = thread_caught_note_claim(&b);
    bool c3    = thread_caught_note_claim(&c);

    // Drain one interrupt: the other is still queued, and a's claim holds.
    struct Note got;
    spin_lock(&p->notes->lock);
    int d1 = notes_dequeue_for_fd_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    bool c4    = thread_caught_note_claim(&c);
    // a's tail releases its claim with an interrupt still queued.
    notes_release_claims_for_test(&a);
    u16  a_rec = a.note_claim;
    bool c5    = thread_caught_note_claim(&c);
    // Drain the rest: every caught bit clears, and the claims stay with their
    // owners until their tails.
    spin_lock(&p->notes->lock);
    int d2 = notes_dequeue_for_fd_locked(p, NULL, &got);
    int d3 = notes_dequeue_for_fd_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    u32 flags  = __atomic_load_n(&p->note_caught, __ATOMIC_ACQUIRE);
    u32 caught = flags & CAUGHT_LATCH_MASK;
    u32 claims = (flags & CAUGHT_CLAIM_MASK) >> CAUGHT_CLAIM_SHIFT;
    notes_release_claims_for_test(&b);
    notes_release_claims_for_test(&c);
    u32 claims_after = __atomic_load_n(&p->note_caught, __ATOMIC_ACQUIRE) &
                       CAUGHT_CLAIM_MASK;
    int  post4 = notes_post(p, "interrupt", 0u, NULL, true);
    b.note_mask = 1u << NOTE_BIT_INTERRUPT;                     // b defers it
    bool b5m   = thread_caught_note_claim(&b);
    b.note_mask = 0u;
    bool b5    = thread_caught_note_claim(&b);

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);

    TEST_ASSERT(!none0, "no caught note queued -> no claim");
    TEST_ASSERT(post1 == 0 && post2 == 0 && post3 == 0 && post4 == 0, "posts accepted");
    TEST_ASSERT(a1, "the first sleeper claims the caught family");
    TEST_ASSERT(!b1, "a second sleeper is REFUSED while that note is pending "
                     "(one note, one unwind)");
    TEST_ASSERT(b1dl, "control: the note is still deliverable to the second "
                      "thread -- only the unwind is refused");
    TEST_ASSERT(a1own, "the claimant's own claim stays open to it: a wait that "
                       "claimed and completed must not refuse the call's next wait");
    TEST_ASSERT(!b2, "a second queued note of a claimed family unwinds nobody else");
    TEST_ASSERT(b3, "another family claims independently");
    TEST_ASSERT(!c3, "and is then taken for every other thread");
    TEST_ASSERT(d1 == 1 && d2 == 1 && d3 == 1, "three notes dequeued");
    TEST_ASSERT(!c4, "an interrupt still queued, a's claim held -> refused");
    TEST_EXPECT_EQ(a_rec, (u8)0u, "the release clears the claimant's record");
    TEST_ASSERT(c5, "released, the still-queued interrupt is open to another "
                    "thread: a claim never outlives its claimant's tail");
    TEST_EXPECT_EQ(caught, 0u, "the last drain of each family clears its caught bit");
    TEST_EXPECT_EQ(claims, (1u << NOTE_BIT_INTERRUPT) | (1u << NOTE_BIT_CHILD_EXIT),
                   "but not the claims, which end with their owners' tails");
    TEST_EXPECT_EQ(claims_after, 0u, "the owners' releases clear them");
    TEST_ASSERT(!b5m, "a thread masking the family cannot claim it");
    TEST_ASSERT(b5, "a fresh note can be claimed again");
}

// ---------------------------------------------------------------------------
// handler_escape_predicate (bug-2 / VIVARIUM.md §6.23 regression)
// ---------------------------------------------------------------------------
//
// thread_note_handler_escaped detects a PHENO_LINUX note handler that escaped
// its frame via siglongjmp (no rt_sigreturn), leaving in_handler stuck true --
// which the N-3 re-entrancy guard would otherwise read as "a handler is still
// running" and use to refuse EVERY future caught-note delivery for the life of
// the guest (permanent signal-deafness). The discrimination is TOTAL, not a
// heuristic: a LIVE handler always runs BELOW the pre-handler sp (its sigframe is
// pushed below note_saved_sp_el0, and nested/deep handlers only go lower), while
// a siglongjmp target must be an ANCESTOR frame (jumping to a returned env is UB)
// -- older, hence at a HIGHER sp. So sp < saved <=> live and sp >= saved <=>
// escaped, with no overlap. Gated to in_handler AND PHENO_LINUX.
void test_notes_handler_escape_predicate(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    p->state = PROC_STATE_ALIVE;
    p->phenotype = PHENO_LINUX;

    static struct Thread t;                 // F3: BSS-zeroed (like th below) so a
                                            // future predicate reading a 4th field
                                            // can't read garbage -- and no runtime
                                            // memset (a stack `= {0}` on this large
                                            // struct emits an undefined memset call)
    t.proc              = p;
    t.in_handler        = true;
    t.note_saved_sp_el0 = 0x8000u;          // the pre-handler (interrupted) sp

    TEST_ASSERT(!thread_note_handler_escaped(NULL, 0x9000u), "NULL thread -> false");

    // A LIVE handler: sp strictly BELOW the saved sp. Never flagged, at any
    // depth -- the sigframe is below note_saved_sp_el0 and recursion goes lower.
    TEST_ASSERT(!thread_note_handler_escaped(&t, 0x7FFFu),
                "live handler (sp just below saved) -> false");
    TEST_ASSERT(!thread_note_handler_escaped(&t, 0x0001u),
                "deeply-nested handler (sp far below saved) -> false");

    // An ESCAPE: sp AT the boundary and ABOVE it both trip -- an ancestor
    // sigsetjmp frame sits at or above the pre-handler sp.
    TEST_ASSERT(thread_note_handler_escaped(&t, 0x8000u),
                "escape at the boundary (sp == saved) -> true");
    TEST_ASSERT(thread_note_handler_escaped(&t, 0x9000u),
                "escape above (sp > saved) -> true");

    // The in_handler gate: no handler running -> nothing could have escaped.
    t.in_handler = false;
    TEST_ASSERT(!thread_note_handler_escaped(&t, 0x9000u),
                "in_handler clear -> false whatever the sp");
    t.in_handler = true;

    // The PHENO gate: a NATIVE Proc is off this path entirely (its longjmp
    // discipline is a separate question §6.23 does not touch), even with the
    // escape sp condition satisfied.
    p->phenotype = PHENO_NATIVE;
    TEST_ASSERT(!thread_note_handler_escaped(&t, 0x9000u),
                "PHENO_NATIVE -> false even with sp >= saved");
    p->phenotype = PHENO_LINUX;

    // The clear the caller performs on a true return re-arms delivery: once
    // in_handler is false, the escape is consumed and the predicate stops firing
    // (so it cannot clear twice or fight a fresh delivery's in_handler set).
    TEST_ASSERT(thread_note_handler_escaped(&t, 0x9000u), "escape still true pre-clear");
    t.in_handler = false;                    // the caller's action on a true return
    TEST_ASSERT(!thread_note_handler_escaped(&t, 0x9000u),
                "after the caller clears in_handler -> false (escape consumed)");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// caught_note_stop_dequeue_drains (item 11, round F1 regression)
// ---------------------------------------------------------------------------
//
// notes_stop_dequeue_locked popped a STOP note WITHOUT running the caught drain
// pre-fix. Because the caught latch is per-FAMILY and the TTY family bit is
// SHARED, a caught tty:winch (SIGWINCH handler) arms the bit, and popping an
// uncaught tty:susp then STRANDED it -> thread_caught_note_deliverable read true
// with no tty note queued -> an opted-in reader EINTR-livelocked.
void test_notes_caught_note_stop_dequeue_drains(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    p->state = PROC_STATE_ALIVE;

    // Simulate a caught tty:winch having armed the SHARED TTY family bit. (The
    // realistic arm needs a phenotype per-signal sigtab; forcing the bit tests
    // the FIX -- does the stop-dequeue drain it? -- with the same end state.)
    __atomic_or_fetch(&p->note_caught,
                      1u << (CAUGHT_LATCH_SHIFT + NOTE_BIT_TTY),
                      __ATOMIC_RELEASE);
    TEST_ASSERT(proc_caught_note_pending(p),
                "caught-TTY bit armed (simulated caught tty:winch sibling)");

    // Queue an UNCAUGHT tty:susp: a fresh native Proc has no handler, so the
    // STOP default applies and notes_stop_dequeue_locked will pop it.
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_TTY_SUSP, 0u, NULL, true), 0,
                   "post tty:susp");

    struct Note got;
    spin_lock(&p->notes->lock);
    int r = notes_stop_dequeue_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(r, 1, "stop-dequeue popped the tty:susp");

    // The FIX: the pop drained the caught latch -> no tty note remains -> the
    // stale caught-TTY bit clears. PRE-FIX this asserts FALSE (bit stranded).
    TEST_ASSERT(!proc_caught_note_pending(p),
                "F1: stop-dequeue drained the stale caught-TTY bit");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// notes.fstat_reports_chr — #97 (the #96 sibling): POSIX fstat on a notes fd.
//
// Pre-#97 devnotes had no .stat_native, so SYS_FSTAT returned -1 on a notes
// fd. Fail-closed but wrong, and #96 shows the cost is real: a caller that
// treats a non-EBADF fstat failure on a standard fd as FATAL (clang does)
// dies on a Proc that put its notes fd on 0/1/2.
//
// Drives spoor_stat_native -- the exact function SYS_FSTAT calls -- not the
// vtable slot, so a refactor that reintroduces the NULL slot fails here.
void test_notes_fstat_reports_chr(void) {
    struct Spoor *c = devnotes.attach(NULL);
    TEST_ASSERT(c != NULL, "devnotes attach yields a Spoor");

    struct t_stat st;
    // Poison: a stat_native returning 0 without filling must not pass by
    // leaving stale zeroes that happen to look right.
    for (size_t i = 0; i < sizeof(st); i++) ((u8 *)&st)[i] = 0xA5;

    TEST_EXPECT_EQ(spoor_stat_native(c, &st), 0,
        "fstat on a notes fd succeeds (pre-#97 this returned -1)");
    TEST_EXPECT_EQ((long)(st.mode & T_S_IFMT), (long)T_S_IFCHR,
        "notes reports S_IFCHR");
    TEST_EXPECT_EQ((long)(st.mode & 07777u), 0666L,
        "notes reports 0666 (any Proc may open its own)");
    TEST_EXPECT_EQ((long)st.size, 0L, "notes reports size 0");
    TEST_EXPECT_EQ((long)st.nlink, 1L, "notes reports nlink 1");
    // Not advisory: devnotes_read rejects a buffer smaller than this.
    TEST_EXPECT_EQ((long)st.blksize, (long)sizeof(struct note_record),
        "blksize is the minimum viable read (sizeof note_record)");

    // The load-bearing property. pouch decodes is-a-pts as S_ISCHR + qid bit
    // 40 and is-a-cons as S_ISCHR + qid bit 41. Now that a notes fd reports
    // S_IFCHR, those bits are all that keep it from reading as a TERMINAL --
    // so a future change that stamps a qid here must not touch them.
    TEST_ASSERT((st.qid_path & (1ULL << 40)) == 0,
        "notes qid does NOT set the is-a-pts bit (40)");
    TEST_ASSERT((st.qid_path & (1ULL << 41)) == 0,
        "notes qid does NOT set the is-a-cons bit (41)");

    spoor_clunk(c);
}

// ---------------------------------------------------------------------------
// linux_sigign_discard (VIVARIUM V-6b)
// ---------------------------------------------------------------------------
//
// A PHENO_LINUX Proc that has set SIG_IGN for a signal discards that signal's
// note AT GENERATION, exactly as Linux does -- and reports SUCCESS, because on
// Linux `kill()` to a process ignoring the signal succeeds.
//
// The queue is directly observable here, which no in-guest leg can manage (a
// Linux guest has no notes fd and rt_sigpending is ENOSYS). The guest-side
// generator DOES exist since V-6c -- fd 0 is a reader-less pipe write end, so
// viv-pheno-probe raises SIGPIPE at will; its L205-L216 legs drive the
// install-time twin of this hook (notes_discard_name) through rt_sigaction.
//
// Post-time and NOT delivery-time is the property under test. An ignored note
// that reached the queue would occupy a slot, arm the LS-5c terminate latch,
// and leave blocked threads unwinding *_INTR until the EL0-return tail got
// round to dropping it -- so `count` staying 0 is the assertion, not just
// "the note never fired".
void test_notes_linux_sigign_discard(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");

    struct viv_sigtab *tab =
        (struct viv_sigtab *)kzalloc(sizeof(struct viv_sigtab), 0);
    TEST_ASSERT(tab != NULL, "sigtab alloc");

    // A NATIVE Proc is unaffected even with a table hung off it -- the gate is
    // the phenotype, and this asserts the branch is not simply always-on.
    p->sigtab = tab;
    struct viv_ksigaction ign = { .handler = VIV_SIG_IGN, .flags = 0,
                                  .restorer = 0, .mask = 0 };
    (void)viv_sigtab_set(tab, VIV_SIGNOTE_PIPE, &ign);
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_PIPE, 7u, NULL, true), 0,
                   "native Proc: post accepted");
    TEST_EXPECT_EQ(p->notes->count, 1u,
                   "native Proc: the note was QUEUED -- phenotype gates this");

    // Drain it so the next assertion reads a clean queue.
    struct Note got;
    spin_lock(&p->notes->lock);
    notes_dequeue_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(p->notes->count, 0u, "queue drained");

    // Now the Linux phenotype: the same post is discarded, and SUCCEEDS.
    p->phenotype = PHENO_LINUX;
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_PIPE, 9u, NULL, true), 0,
                   "linux + SIG_IGN: post reports success (Linux kill() does)");
    TEST_EXPECT_EQ(p->notes->count, 0u,
                   "linux + SIG_IGN: nothing was queued at all");

    // A DIFFERENT note is untouched -- the discard is per-note, not a blanket
    // mute. tty:hup shares NOTE_BIT_TTY with tty:winch but is its own note, so
    // this also pins that the table is keyed by note and not by mask bit.
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_INTERRUPT, 1u, NULL, true), 0,
                   "linux: interrupt still posts");
    TEST_EXPECT_EQ(p->notes->count, 1u, "linux: interrupt was queued");

    (void)viv_sigtab_set(tab, VIV_SIGNOTE_TTY_WINCH, &ign);
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_TTY_HUP, 2u, NULL, true), 0,
                   "linux: tty:hup posts though tty:winch is ignored");
    TEST_EXPECT_EQ(p->notes->count, 2u,
                   "tty:hup queued -- one MASK bit, but separate dispositions");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_TTY_WINCH, 3u, NULL, true), 0,
                   "linux: tty:winch accepted");
    TEST_EXPECT_EQ(p->notes->count, 2u, "tty:winch discarded, not queued");

    // proc_free releases the table; a leak here would show up as a slab
    // imbalance rather than a failed assertion, which is why the alloc is real
    // rather than a stack struct.
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// notes_discard_name -- the INSTALL-time discard (POSIX 2.4.3 / Linux
// do_sigaction's flush_sigqueue_mask), the half of "SIG_IGN discards pending
// signals" that notes_post's generation-time hook cannot do: a note that was
// queued BEFORE the disposition became SIG_IGN. The phenotype rt_sigaction
// shell calls it after the store; the in-guest legs (viv-pheno-probe L205-L216)
// drive that shell. This pins the primitive where the queue is observable:
// every note of ONE name goes, whatever any mask says; the class latch drains
// per removal and only when the class is empty; the survivors keep their
// order; `kill` is untouchable; an absent name removes nothing.
// ---------------------------------------------------------------------------
void test_notes_discard_name_purges_pending(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    struct Note got;

    // [child_exit, interrupt, tty:winch, interrupt]: two interrupts under
    // SIG_DFL (no handler, not self-managing) arm the interrupt terminate latch.
    // tty:winch (IGNORE class) is the non-arming filler -- #237 made `pipe`
    // terminate-class, so it can no longer stand in for "a queued note that does
    // not arm the terminate latch" here.
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_CHILD_EXIT, 1u, NULL, true), 0, "post child_exit");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_INTERRUPT, 2u, NULL, true), 0, "post interrupt 1");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_TTY_WINCH, 3u, NULL, true), 0, "post tty:winch");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_INTERRUPT, 4u, NULL, true), 0, "post interrupt 2");
    TEST_EXPECT_EQ(p->notes->count, 4u, "four queued");
    TEST_ASSERT(proc_intr_terminate_pending(p), "the interrupts armed the latch");

    // An absent name changes nothing.
    TEST_EXPECT_EQ(notes_discard_name(p, NOTE_NAME_TTY_HUP), 0u,
                   "absent name: nothing removed");
    TEST_EXPECT_EQ(p->notes->count, 4u, "absent name: count unchanged");

    // kill is never discardable (N-4): queue one, ask, and it must stay.
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_KILL, 0u, NULL, true), 0, "post kill");
    TEST_EXPECT_EQ(p->notes->count, 5u, "kill queued");
    TEST_EXPECT_EQ(notes_discard_name(p, NOTE_NAME_KILL), 0u,
                   "kill: refused, nothing removed");
    TEST_EXPECT_EQ(p->notes->count, 5u, "kill: count unchanged");

    // The purge: BOTH interrupts go (masks are not consulted -- there is no
    // thread to consult), the latch drains with the class, the rest keep order.
    TEST_EXPECT_EQ(notes_discard_name(p, NOTE_NAME_INTERRUPT), 2u,
                   "both interrupts removed");
    TEST_EXPECT_EQ(p->notes->count, 3u, "three remain");
    TEST_ASSERT(!proc_intr_terminate_pending(p),
                "removing the last interrupt drained the latch");
    // The fd-read pop skips kill, so it walks the survivors in FIFO order.
    spin_lock(&p->notes->lock);
    int r1 = notes_dequeue_for_fd_locked(p, NULL, &got);
    u32 a1 = got.arg;
    int r2 = notes_dequeue_for_fd_locked(p, NULL, &got);
    u32 a2 = got.arg;
    int r3 = notes_dequeue_for_fd_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_ASSERT(r1 == 1 && a1 == 1u, "survivor 1 is the child_exit (arg 1)");
    TEST_ASSERT(r2 == 1 && a2 == 3u, "survivor 2 is the tty:winch (arg 3)");
    TEST_EXPECT_EQ(r3, 0, "only the kill is left for the fd path (it skips kill)");
    spin_lock(&p->notes->lock);
    int rk = notes_dequeue_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_ASSERT(rk == 1 && notes_name_is_kill(got.name), "the kill survived the purge");
    TEST_EXPECT_EQ(p->notes->count, 0u, "queue empty");

    // Per-CLASS drain: tty:hup and tty:quit share the TTY terminate latch.
    // Discarding one name leaves the latch armed for the other; discarding
    // the other clears it.
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_TTY_HUP, 0u, NULL, true), 0, "post tty:hup");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_TTY_QUIT, 0u, NULL, true), 0, "post tty:quit");
    TEST_ASSERT((__atomic_load_n(&p->proc_flags, __ATOMIC_ACQUIRE)
                 & PROC_FLAG_TTY_TERMINATE_PENDING) != 0u,
                "the tty pair armed the TTY latch");
    TEST_EXPECT_EQ(notes_discard_name(p, NOTE_NAME_TTY_HUP), 1u, "tty:hup removed");
    TEST_ASSERT((__atomic_load_n(&p->proc_flags, __ATOMIC_ACQUIRE)
                 & PROC_FLAG_TTY_TERMINATE_PENDING) != 0u,
                "TTY latch stays while tty:quit remains (per-class drain)");
    TEST_EXPECT_EQ(notes_discard_name(p, NOTE_NAME_TTY_QUIT), 1u, "tty:quit removed");
    TEST_ASSERT((__atomic_load_n(&p->proc_flags, __ATOMIC_ACQUIRE)
                 & PROC_FLAG_TTY_TERMINATE_PENDING) == 0u,
                "removing the last of the class drained the TTY latch");
    TEST_EXPECT_EQ(p->notes->count, 0u, "queue empty again");

    // Every removal is a real dequeue: a full ring purged of one name has room
    // for that many new posts (a purge that only hid entries would still be
    // full). Sixteen pipes, purge, sixteen more posts land.
    for (u32 i = 0; i < NOTE_QUEUE_DEPTH; i++)
        TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_PIPE, i, NULL, false), 0, "fill");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_CHILD_EXIT, 0u, NULL, false), -(int)T_E_AGAIN,
                   "full: a userspace post is refused");
    TEST_EXPECT_EQ(notes_discard_name(p, NOTE_NAME_PIPE), (u32)NOTE_QUEUE_DEPTH,
                   "all sixteen removed");
    TEST_EXPECT_EQ(p->notes->count, 0u, "purged ring is empty");
    for (u32 i = 0; i < NOTE_QUEUE_DEPTH; i++)
        TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_CHILD_EXIT, i, NULL, false), 0,
                       "refill lands");
    TEST_EXPECT_EQ(notes_discard_name(p, NOTE_NAME_CHILD_EXIT), (u32)NOTE_QUEUE_DEPTH,
                   "refill purged");

    // A NULL / table-less Proc and a NULL name are no-ops, not faults.
    TEST_EXPECT_EQ(notes_discard_name(NULL, NOTE_NAME_PIPE), 0u, "NULL proc");
    TEST_EXPECT_EQ(notes_discard_name(p, NULL), 0u, "NULL name");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// =============================================================================
// #15 / #236: the per-note DEFAULT ACTION table + the SYS_NOTED(NDFLT) dispatch.
// =============================================================================

// The policy half, as a pure lookup. This is a table test rather than a
// spot-check on purpose: the bug #15 fixed was that ALL nine names shared one
// disposition, so a test that asserted only tty:susp would pass on an
// implementation that stopped for everything -- which is the opposite defect
// and just as wrong. Naming every row is what makes the green attributable.
void test_notes_default_action_table(void) {
    // TERMINATE -- the class that was already correct before #15, asserted so
    // the fix is shown not to have moved it.
    TEST_EXPECT_EQ((int)notes_default_action(NOTE_NAME_INTERRUPT),
                   (int)NOTE_DFL_TERMINATE, "interrupt terminates");
    TEST_EXPECT_EQ((int)notes_default_action(NOTE_NAME_KILL),
                   (int)NOTE_DFL_TERMINATE, "kill terminates");
    TEST_EXPECT_EQ((int)notes_default_action(NOTE_NAME_PIPE),
                   (int)NOTE_DFL_TERMINATE, "pipe terminates (POSIX SIGPIPE)");
    TEST_EXPECT_EQ((int)notes_default_action(NOTE_NAME_TTY_QUIT),
                   (int)NOTE_DFL_TERMINATE, "tty:quit terminates");
    TEST_EXPECT_EQ((int)notes_default_action(NOTE_NAME_TTY_HUP),
                   (int)NOTE_DFL_TERMINATE, "tty:hup terminates");
    TEST_EXPECT_EQ((int)notes_default_action(NOTE_NAME_TERM),
                   (int)NOTE_DFL_TERMINATE, "term terminates (POSIX SIGTERM)");

    // STOP -- one row, and the whole reason #15 exists.
    TEST_EXPECT_EQ((int)notes_default_action(NOTE_NAME_TTY_SUSP),
                   (int)NOTE_DFL_STOP, "tty:susp STOPS (SIGTSTP), never dies");

    // IGNORE -- #236. Each of these terminated the Proc before #15.
    TEST_EXPECT_EQ((int)notes_default_action(NOTE_NAME_CHILD_EXIT),
                   (int)NOTE_DFL_IGNORE, "child_exit ignores (SIGCHLD)");
    TEST_EXPECT_EQ((int)notes_default_action(NOTE_NAME_TTY_WINCH),
                   (int)NOTE_DFL_IGNORE, "tty:winch ignores (SIGWINCH)");
    TEST_EXPECT_EQ((int)notes_default_action(NOTE_NAME_TTY_CONT),
                   (int)NOTE_DFL_IGNORE, "tty:cont ignores (resume already done)");

    // An unsupported name falls back to TERMINATE -- both the pre-#15
    // behaviour for every name (so nothing regressed) and the POSIX majority.
    TEST_EXPECT_EQ((int)notes_default_action("no_such_note"),
                   (int)NOTE_DFL_TERMINATE, "unknown name -> terminate");
    TEST_EXPECT_EQ((int)notes_default_action(NULL),
                   (int)NOTE_DFL_TERMINATE, "NULL name -> terminate");

    // The EL0-tail uncaught arm reads the SAME column since #15. Asserted here
    // because the two used to be independent lists, and a future edit that
    // re-splits them would leave this pair disagreeing.
    //
    // #237 resolved pipe's disposition: its default action is TERMINATE and it
    // now carries PROC_FLAG_PIPE_TERMINATE_PENDING, so the EL0 tail default-
    // terminates on an unmasked + unhandled pipe note. Native runtimes mask
    // NOTE_BIT_PIPE at startup to keep EPIPE-not-death; this line is where that
    // resolution lands (it flips the pre-#237 "pipe -> 0" this test asserted).
    TEST_EXPECT_EQ((int)notes_name_terminate_latch_for_test(NOTE_NAME_INTERRUPT),
                   (int)PROC_FLAG_INTR_TERMINATE_PENDING, "interrupt -> INTR latch");
    TEST_EXPECT_EQ((int)notes_name_terminate_latch_for_test(NOTE_NAME_TTY_QUIT),
                   (int)PROC_FLAG_TTY_TERMINATE_PENDING, "tty:quit -> TTY latch");
    TEST_EXPECT_EQ((int)notes_name_terminate_latch_for_test(NOTE_NAME_TTY_HUP),
                   (int)PROC_FLAG_TTY_TERMINATE_PENDING, "tty:hup -> TTY latch");
    TEST_EXPECT_EQ((int)notes_name_terminate_latch_for_test(NOTE_NAME_TTY_SUSP),
                   0, "tty:susp -> NO latch (it stops, it does not die)");
    TEST_EXPECT_EQ((int)notes_name_terminate_latch_for_test(NOTE_NAME_TTY_WINCH),
                   0, "tty:winch -> no latch");
    TEST_EXPECT_EQ((int)notes_name_terminate_latch_for_test(NOTE_NAME_TTY_CONT),
                   0, "tty:cont -> no latch");
    TEST_EXPECT_EQ((int)notes_name_terminate_latch_for_test(NOTE_NAME_CHILD_EXIT),
                   0, "child_exit -> no latch");
    TEST_EXPECT_EQ((int)notes_name_terminate_latch_for_test(NOTE_NAME_KILL),
                   0, "kill -> no latch (N-4 terminates it before the latch)");
    TEST_EXPECT_EQ((int)notes_name_terminate_latch_for_test(NOTE_NAME_PIPE),
                   (int)PROC_FLAG_PIPE_TERMINATE_PENDING,
                   "pipe -> PIPE latch (#237)");
    TEST_EXPECT_EQ((int)notes_name_terminate_latch_for_test(NOTE_NAME_TERM),
                   (int)PROC_FLAG_TERM_TERMINATE_PENDING,
                   "term -> its own latch, never interrupt's");
}

// Drive notes_noted_default through its two RETURNING dispositions. The
// terminating one is not driven here -- exits() is noreturn and would take the
// test harness with it; the table test above pins which names reach it, and
// the pre-existing interrupt_terminate_gate test covers the terminate path.
//
// Both directions of #185 are asserted:
//   (1) SIG_DFL tty:susp STOPS      -- the fix does what it claims;
//   (2) child_exit does NOT stop    -- the fix did not make everything stop.
// Without (2), an implementation that stopped on every NDFLT would pass.
// Count queued notes matching `name` (the test_pts.c pts_note_count shape --
// duplicated rather than shared because the two files have no common test
// header and a one-loop helper is cheaper than inventing one).
static u32 ndflt_note_count(struct Proc *p, const char *name) {
    struct NoteQueue *q = p->notes;
    u32 found = 0;
    spin_lock(&q->lock);
    u32 idx = q->head;
    for (u32 n = 0; n < q->count; n++) {
        int eq = 1;
        for (u32 i = 0; i < NOTE_NAME_MAX; i++) {
            if (q->ring[idx].name[i] != name[i]) { eq = 0; break; }
            if (name[i] == 0) break;
        }
        if (eq) found++;
        idx = (idx + 1) % NOTE_RING_SLOTS;
    }
    spin_unlock(&q->lock);
    return found;
}

static void ndflt_arm_handler(struct Thread *t, struct Proc *m,
                              const char *name) {
    t->magic       = THREAD_MAGIC;
    t->proc        = m;
    t->in_handler  = true;
    for (u32 i = 0; i < NOTE_NAME_MAX; i++) t->note_handling_name[i] = 0;
    for (u32 i = 0; name[i] != 0 && i < NOTE_NAME_MAX - 1; i++)
        t->note_handling_name[i] = name[i];
    // The pre-handler context the restore must reproduce. Distinct sentinels
    // so a restore that silently no-ops is visible.
    for (u32 i = 0; i < 31; i++) t->note_saved_regs[i] = 0xA000u + i;
    t->note_saved_sp_el0 = 0xB000u;
    t->note_saved_elr    = 0xC000u;
    t->note_saved_spsr   = 0u;
}

void test_notes_ndflt_dispatch(void) {
    // The anchored-group shape from the PTY-1f orphan tests: `m` is in its own
    // group, and `leader` -- ALIVE, same session, another group -- is the
    // parent that keeps m's group non-orphaned. Without the anchor the stop is
    // correctly discarded, which would make direction (1) below vacuous.
    struct Proc *leader = proc_alloc();
    TEST_ASSERT(leader != NULL, "proc_alloc leader");
    proc_test_link(leader);
    struct Proc *m = proc_alloc();
    TEST_ASSERT(m != NULL, "proc_alloc m");
    m->sid  = (u32)leader->pid;
    m->pgid = (u32)m->pid;
    proc_test_link_child(leader, m);

    static struct Thread th;            // BSS-zeroed; see test_pts.c's m_th
    static struct exception_context ctx;

    // `m` MUST own an UNMASKED thread, and this is load-bearing rather than
    // decoration. proc_tty_susp_would_stop_locked's last line returns false
    // for "every thread masks the tty family (OR NO THREADS)" -- so a Proc
    // with m->threads == NULL reads as CAUGHT no matter what handler_va says.
    // Leg (2b) below asserts CAUGHT; with a threadless fixture it would assert
    // it for the WRONG REASON and pass even with the handler_va gate deleted.
    // Caught by running that exact sabotage: the leg stayed green until this
    // thread was attached.
    th.magic            = THREAD_MAGIC;
    th.note_mask        = 0;            // unmasked -> the gate can reach `true`
    th.next_in_proc     = NULL;
    th.rendez_blocked_on = NULL;
    m->threads = &th;

    // ---- (1) tty:susp under SIG_DFL: the Proc STOPS. --------------------
    //
    // Note the setup: handler_va is NON-ZERO, because that is the only way a
    // Proc reaches NDFLT at all -- the pre-delivery gate routes a susp to the
    // handler precisely BECAUSE a handler exists. A stop arm that re-consulted
    // catchability would see this and refuse, which is the ignore-not-stop bug
    // #15 removes. Asserting with the handler installed is what makes the test
    // reach the code under test rather than the no-handler path.
    m->handler_va = 0x1000u;
    // #240: ndflt_arm_handler fabricates the DELIVERY half only. In production
    // a susp is posted before it is delivered, and the post is what arms
    // susp_stop_armed -- so the fixture must stand in for that too or the new
    // freshness gate discards this stop and the leg proves nothing. Set
    // explicitly rather than inside the helper: forgetting it here fails LOUD
    // (the leg asserts job_stop_req == 1), whereas a helper doing it silently
    // would also re-arm the cont-discard legs below and hollow them out.
    m->susp_stop_armed = 1u;
    ndflt_arm_handler(&th, m, NOTE_NAME_TTY_SUSP);
    TEST_EXPECT_EQ(notes_noted_default(&ctx, &th), 0, "NDFLT(tty:susp) succeeds");
    TEST_ASSERT(!th.in_handler, "the stop left the handler (restore ran)");
    TEST_EXPECT_EQ((int)(ctx.elr - 0xC000u), 0,
                   "resumes at the INTERRUPTED pc, not inside the handler");
    TEST_EXPECT_EQ((int)(ctx.regs[0] - 0xA000u), 0, "x0 restored");
    TEST_EXPECT_EQ((int)m->job_stop_req, 1, "job_stop_req ARMED -- ^Z stops");
    TEST_ASSERT(m->stop_report_pending,
                "the stop latched the parent's WAIT_UNTRACED report");

    // Idempotent: a second NDFLT(susp) on an already-stopped Proc re-latches
    // nothing (POSIX discards a stop signal for a stopped process).
    m->stop_report_pending = false;
    ndflt_arm_handler(&th, m, NOTE_NAME_TTY_SUSP);
    TEST_EXPECT_EQ(notes_noted_default(&ctx, &th), 0, "second NDFLT succeeds");
    TEST_EXPECT_EQ((int)m->job_stop_req, 1, "still stopped");
    TEST_ASSERT(!m->stop_report_pending, "no re-latch on the idempotent stop");

    // ---- (2) child_exit under SIG_DFL: NOTHING happens. -----------------
    //
    // The direction that fails on a "stop for every NDFLT" implementation, and
    // the direct #236 regression: before the per-note table this call KILLED
    // the Proc via exits("child_exit").
    m->job_stop_req = 0;
    m->stop_report_pending = false;
    ndflt_arm_handler(&th, m, NOTE_NAME_CHILD_EXIT);
    TEST_EXPECT_EQ(notes_noted_default(&ctx, &th), 0, "NDFLT(child_exit) succeeds");
    TEST_ASSERT(!th.in_handler, "ignore left the handler (restore ran)");
    TEST_EXPECT_EQ((int)(ctx.elr - 0xC000u), 0, "ignore resumes where it was");
    TEST_EXPECT_EQ((int)m->job_stop_req, 0, "child_exit did NOT stop the Proc");
    TEST_ASSERT(!m->stop_report_pending, "and latched no stop report");

    // tty:winch is the same class; asserted separately because it reaches the
    // table by a different row and shares NOTE_BIT_TTY with tty:susp, so a
    // dispatch keyed on the MASK BIT rather than the name would stop here.
    ndflt_arm_handler(&th, m, NOTE_NAME_TTY_WINCH);
    TEST_EXPECT_EQ(notes_noted_default(&ctx, &th), 0, "NDFLT(tty:winch) succeeds");
    TEST_EXPECT_EQ((int)m->job_stop_req, 0,
                   "tty:winch did NOT stop -- keyed by NAME, not by family bit");

    // ---- (2b) the CAUGHT-arm -> NDFLT linkage (audit F8). ---------------
    //
    // Legs (1) and (2) fabricate the in-handler state directly, so they prove
    // the NDFLT arm in isolation and say nothing about the composition that
    // makes #15 mean anything: the pts fan must decline to stop a
    // handler-having member (posting the note instead), and the member's own
    // NDFLT must then stop it. Without this, an edit to
    // proc_tty_susp_would_stop_locked that made a handler-having Proc stop at
    // POST time again would leave legs (1) and (2) green while the target got
    // DOUBLE-stopped.
    m->job_stop_req = 0;
    m->stop_report_pending = false;
    TEST_EXPECT_EQ(ndflt_note_count(m, NOTE_NAME_TTY_SUSP), 0u,
                   "precondition: no susp queued");
    // #240: clear the arm BEFORE the fan so the assertion below is a real
    // control. Leg (1) left it at 1; asserting "the post armed it" without
    // this would be satisfied by the stale value and would stay green with
    // notes_arm_susp_stop_locked deleted outright.
    m->susp_stop_armed = 0u;
    TEST_EXPECT_EQ(proc_job_stop_pgrp(m->pgid), 1, "the fan visited m");
    TEST_EXPECT_EQ((int)m->susp_stop_armed, 1,
                   "the POST armed the #240 freshness flag (production path, "
                   "not the fixture)");
    TEST_EXPECT_EQ((int)m->job_stop_req, 0,
                   "CAUGHT (handler_va != 0): the fan posted, did NOT stop");
    TEST_EXPECT_EQ(ndflt_note_count(m, NOTE_NAME_TTY_SUSP), 1u,
                   "the susp note was queued for the handler");

    // Now the target takes the default itself -- and the note must NOT be
    // left pending across the stop (a stale susp surviving a later cont would
    // re-suspend a resumed program). Delivery consumes it; this pins that.
    {
        struct Note drained;
        spin_lock(&m->notes->lock);
        (void)notes_dequeue_locked(m, NULL, &drained);   // stand in for delivery
        spin_unlock(&m->notes->lock);
    }
    ndflt_arm_handler(&th, m, NOTE_NAME_TTY_SUSP);
    TEST_EXPECT_EQ(notes_noted_default(&ctx, &th), 0, "the target NDFLTs");
    TEST_EXPECT_EQ((int)m->job_stop_req, 1, "NOW it stops -- the linkage holds");
    TEST_EXPECT_EQ(ndflt_note_count(m, NOTE_NAME_TTY_SUSP), 0u,
                   "and no susp is left pending across the stop");

    // ---- (3) the orphan rule survives the new path. ---------------------
    //
    // `orphan` is in its own session under kproc, so no ALIVE same-session
    // out-of-group parent anchors its group. POSIX discards a stop there --
    // and it must, because a group already orphaned when it stops never gets
    // the hup+cont rescue a LATER orphaning would fire.
    struct Proc *orphan = proc_alloc();
    TEST_ASSERT(orphan != NULL, "proc_alloc orphan");
    orphan->sid  = (u32)orphan->pid;
    orphan->pgid = (u32)orphan->pid;
    proc_test_link(orphan);
    orphan->handler_va = 0x1000u;
    static struct Thread oth;
    // #240, and this line is the difference between a test and a decoration.
    // The leg asserts job_stop_req == 0 -- a NEGATIVE. A fresh Proc has
    // susp_stop_armed == 0, so the new freshness gate ALSO discards the stop,
    // and the leg would stay green with pgrp_orphaned_locked deleted from
    // proc_job_stop_self entirely: it would be satisfied by the wrong reason.
    // Arming leaves the orphan rule as the only thing that can discard.
    orphan->susp_stop_armed = 1u;
    ndflt_arm_handler(&oth, orphan, NOTE_NAME_TTY_SUSP);
    TEST_EXPECT_EQ(notes_noted_default(&ctx, &oth), 0,
                   "NDFLT(tty:susp) on an orphan still SUCCEEDS (not an error)");
    TEST_ASSERT(!oth.in_handler, "orphan discard still left the handler");
    TEST_EXPECT_EQ((int)orphan->job_stop_req, 0,
                   "orphaned group's stop DISCARDED -- nobody could resume it");

    proc_test_unlink(orphan);
    orphan->state = PROC_STATE_ZOMBIE;
    proc_free(orphan);
    proc_test_unlink(m);
    m->threads = NULL;                  // the static outlives proc_free
    m->state = PROC_STATE_ZOMBIE;
    proc_free(m);
    proc_test_unlink(leader);
    leader->state = PROC_STATE_ZOMBIE;
    proc_free(leader);
}

// #241: a cross-Proc `kill` must TERMINATE a single-thread target, not queue a
// note the target may never consume.
//
// The old code group-terminated only when live_threads > 1 and let a
// single-thread target fall through to notes_post, on the reasoning that the
// EL0-return tail's non-catchable-kill branch would pick it up. `kill` arms no
// terminate latch (notes_name_terminate_latch returns 0 for NOTE_BIT_KILL), so
// the post woke nothing at all -- and a job-stopped target parked in
// el0_return_stop_check leaves only via group_exit_msg / !proc_stop_requested
// (and, until DEBUG-FS-DESIGN 5g, thread_die_pending), none of which a
// latchless queued kill satisfies. The post
// returned SUCCESS and the Proc lived forever.
//
// The park loop is not driven here (it needs a scheduled thread). What is
// asserted is the property that makes the park irrelevant: the kill sets
// group_exit_msg -- the one signal every park and sleep predicate honours --
// instead of parking a note in the queue.
void test_notes_kill_terminates_single_thread(void) {
    struct Proc *parent = proc_alloc();
    TEST_ASSERT(parent != NULL, "proc_alloc parent");
    proc_test_link(parent);

    struct Proc *target = proc_alloc();
    TEST_ASSERT(target != NULL, "proc_alloc target");
    proc_test_link_child(parent, target);

    // ONE thread: this is the whole point of the test, so assert the shape
    // rather than assume it. A target that accidentally had peers would take
    // the pre-existing multi-thread cascade and pass for the wrong reason.
    static struct Thread kth;
    kth.magic            = THREAD_MAGIC;
    kth.proc             = target;
    kth.note_mask        = 0;
    kth.next_in_proc     = NULL;
    kth.rendez_blocked_on = NULL;
    target->threads = &kth;

    TEST_ASSERT(target->group_exit_msg == NULL,
                "precondition: target is not already terminating");

    s64 rc = sys_postnote_cross_for_test(parent, target->pid, NOTE_NAME_KILL);
    TEST_EXPECT_EQ(rc, (s64)0, "the cross-Proc kill post reports success");

    // POSITIVE: the cascade ran. Fails closed -- any fixture defect that makes
    // proc_group_terminate bail early leaves this NULL.
    TEST_ASSERT(target->group_exit_msg != NULL,
                "group_exit_msg SET -- the kill cascades (#241)");

    // NEGATIVE, and the half that is red pre-fix. On its own this would be
    // satisfied by a fixture whose queue is unreadable, so the control below
    // proves the counter can actually SEE a queued note.
    TEST_EXPECT_EQ((int)ndflt_note_count(target, NOTE_NAME_KILL), 0,
                   "no kill QUEUED -- it terminated instead of waiting");

    // The control: same call, same fixture shape, ONE variable changed (the
    // note name). `interrupt` is not routed to the cascade, so it MUST land in
    // the queue -- which is what makes the zero above a measurement.
    struct Proc *ctl = proc_alloc();
    TEST_ASSERT(ctl != NULL, "proc_alloc ctl");
    proc_test_link_child(parent, ctl);
    static struct Thread cth;
    cth.magic            = THREAD_MAGIC;
    cth.proc             = ctl;
    cth.note_mask        = 0;
    cth.next_in_proc     = NULL;
    cth.rendez_blocked_on = NULL;
    ctl->threads = &cth;

    TEST_EXPECT_EQ(sys_postnote_cross_for_test(parent, ctl->pid,
                                               NOTE_NAME_INTERRUPT), (s64)0,
                   "the control post reports success");
    TEST_EXPECT_EQ((int)ndflt_note_count(ctl, NOTE_NAME_INTERRUPT), 1,
                   "control: a non-kill note IS queued (so 0 above means 0)");
    TEST_ASSERT(ctl->group_exit_msg == NULL,
                "control: interrupt did NOT cascade -- only kill does");

    proc_test_unlink(ctl);
    ctl->threads = NULL;                // the static outlives proc_free
    ctl->state = PROC_STATE_ZOMBIE;
    proc_free(ctl);
    proc_test_unlink(target);
    target->threads = NULL;
    target->state = PROC_STATE_ZOMBIE;
    proc_free(target);
    proc_test_unlink(parent);
    parent->state = PROC_STATE_ZOMBIE;
    proc_free(parent);
}

// The signal chunk's S-2: the Linux phenotype's kill/tkill/tgkill (operator vote
// 2026-10-13) reuse SYS_POSTNOTE's gate -- the caller, or a child it spawned --
// and report its refusals as Linux errnos. Drives the REAL shell (viv_kill) over
// linked fixtures; every reading is taken before the release and asserted after.
// Red before S-2: the shell did not exist and the three numbers FORWARDed.
static void kfg_thread(struct Thread *t, struct Proc *p, int tid) {
    t->magic             = THREAD_MAGIC;
    t->proc              = p;
    t->tid               = tid;
    t->note_mask         = 0;
    t->note_direct       = 0;
    t->state             = THREAD_RUNNING;
    t->next_in_proc      = NULL;
    t->rendez_blocked_on = NULL;
    p->threads           = t;
}

void test_notes_kill_family_gate(void) {
    struct Proc *caller = proc_alloc();
    struct Proc *child  = proc_alloc();
    struct Proc *other  = proc_alloc();
    TEST_ASSERT(caller && child && other, "proc_alloc x3");
    proc_test_link(caller);
    proc_test_link_child(caller, child);
    proc_test_link(other);                       // alive, not the caller's child

    // tids far above the live counter, so no real thread can answer for them
    static struct Thread cth, kth, oth;
    kfg_thread(&cth, caller, 0x3f000001);
    kfg_thread(&kth, child,  0x3f000002);
    kfg_thread(&oth, other,  0x3f000003);
    const int NO_PID = 0x3ffffff0, NO_TID = 0x3f0000ff;

    s64 r_post   = viv_kill_for_test(caller, child->pid, 0, VIV_SIGTERM);
    u32 c_term1  = ndflt_note_count(child, NOTE_NAME_TERM);
    s64 r_other  = viv_kill_for_test(caller, other->pid, 0, VIV_SIGTERM);
    u32 o_term   = ndflt_note_count(other, NOTE_NAME_TERM);
    s64 r_nopid  = viv_kill_for_test(caller, NO_PID, 0, VIV_SIGTERM);
    s64 r_group  = viv_kill_for_test(caller, -child->pid, 0, VIV_SIGTERM);
    s64 r_probe  = viv_kill_for_test(caller, child->pid, 0, 0);
    u32 c_term2  = ndflt_note_count(child, NOTE_NAME_TERM);
    s64 r_tty    = viv_kill_for_test(caller, child->pid, 0, VIV_SIGHUP);
    s64 r_xabrt  = viv_kill_for_test(caller, child->pid, 0, VIV_SIGABRT);
    s64 r_sabrt  = viv_kill_for_test(caller, caller->pid, 0, VIV_SIGABRT);
    s64 r_range  = viv_kill_for_test(caller, child->pid, 0, 65);
    u32 const intr_bit = 1u << NOTE_BIT_INTERRUPT;
    s64 r_tkill  = viv_kill_for_test(caller, 0, kth.tid, VIV_SIGINT);
    u32 c_word1  = __atomic_load_n(&kth.note_direct, __ATOMIC_ACQUIRE);
    u32 c_intr   = ndflt_note_count(child, NOTE_NAME_INTERRUPT);
    s64 r_tgbad  = viv_kill_for_test(caller, other->pid, kth.tid, VIV_SIGINT);
    s64 r_tgok   = viv_kill_for_test(caller, child->pid, kth.tid, VIV_SIGINT);
    u32 c_word2  = __atomic_load_n(&kth.note_direct, __ATOMIC_ACQUIRE);
    u32 c_intr2  = ndflt_note_count(child, NOTE_NAME_INTERRUPT);
    s64 r_toth   = viv_kill_for_test(caller, 0, oth.tid, VIV_SIGINT);
    s64 r_notid  = viv_kill_for_test(caller, 0, NO_TID, VIV_SIGINT);
    s64 r_tself  = viv_kill_for_test(caller, 0, cth.tid, VIV_SIGTERM);
    u32 s_word   = __atomic_load_n(&cth.note_direct, __ATOMIC_ACQUIRE);
    u32 s_term   = ndflt_note_count(caller, NOTE_NAME_TERM);
    s64 r_sprobe = viv_kill_for_test(caller, 0, 0, 0);
    child->state = PROC_STATE_ZOMBIE;
    s64 r_dead   = viv_kill_for_test(caller, child->pid, 0, VIV_SIGTERM);
    u32 c_term3  = ndflt_note_count(child, NOTE_NAME_TERM);
    s64 r_dprobe = viv_kill_for_test(caller, child->pid, 0, 0);
    s64 r_dtty   = viv_kill_for_test(caller, child->pid, 0, VIV_SIGHUP);
    s64 r_drange = viv_kill_for_test(caller, child->pid, 0, 65);
    s64 r_ntty   = viv_kill_for_test(caller, NO_PID, 0, VIV_SIGHUP);
    other->state = PROC_STATE_ZOMBIE;
    s64 r_odead  = viv_kill_for_test(caller, other->pid, 0, VIV_SIGTERM);

    proc_test_unlink(other);
    other->threads = NULL;
    other->state   = PROC_STATE_ZOMBIE;
    proc_free(other);
    proc_test_unlink(child);
    child->threads = NULL;
    proc_free(child);
    proc_test_unlink(caller);
    caller->threads = NULL;
    caller->state   = PROC_STATE_ZOMBIE;
    proc_free(caller);

    TEST_EXPECT_EQ(r_post, (s64)0, "kill(child, SIGTERM) posts");
    TEST_EXPECT_EQ(c_term1, 1u, "and the child holds one queued term");
    TEST_EXPECT_EQ(r_other, -(s64)T_E_PERM, "kill(non-child) is EPERM: the parent gate");
    TEST_EXPECT_EQ(o_term, 0u, "and nothing reached it");
    TEST_EXPECT_EQ(r_nopid, -(s64)T_E_SRCH, "kill(no such pid) is ESRCH");
    TEST_EXPECT_EQ(r_group, -(s64)T_E_PERM, "kill(-pgid) is refused: the gate admits no group");
    TEST_EXPECT_EQ(r_probe, (s64)0, "kill(child, 0) passes the gate");
    TEST_EXPECT_EQ(c_term2, 1u, "and signal 0 posts nothing");
    TEST_EXPECT_EQ(r_tty, -(s64)T_E_INVAL, "SIGHUP: tty:* is the kernel's to post");
    TEST_EXPECT_EQ(r_xabrt, -(s64)T_E_INVAL, "SIGABRT to another Proc: a snare death is its own");
    TEST_EXPECT_EQ(r_sabrt, (s64)1, "SIGABRT to itself reaches the snare:abort death");
    TEST_EXPECT_EQ(r_range, -(s64)T_E_INVAL, "signal 65 is out of range");
    TEST_EXPECT_EQ(r_tkill, (s64)0, "tkill(child's tid) posts at that thread");
    TEST_EXPECT_EQ(c_word1, intr_bit, "the named thread holds the interrupt");
    TEST_EXPECT_EQ(c_intr, 0u, "and none of it reached the Proc's queue");
    TEST_EXPECT_EQ(r_tgbad, -(s64)T_E_SRCH, "tgkill with a tgid that does not hold the tid");
    TEST_EXPECT_EQ(r_tgok, (s64)0, "tgkill(child, child's tid) posts");
    TEST_EXPECT_EQ(c_word2, intr_bit, "a second interrupt coalesces, as a standard signal does");
    TEST_EXPECT_EQ(c_intr2, 0u, "and the queue stays empty");
    TEST_EXPECT_EQ(r_toth, -(s64)T_E_PERM, "tkill(a non-child's tid) is EPERM");
    TEST_EXPECT_EQ(r_notid, -(s64)T_E_SRCH, "tkill(no such tid) is ESRCH");
    TEST_EXPECT_EQ(r_tself, (s64)0, "tkill(own tid) is self-directed");
    TEST_EXPECT_EQ(s_word, 1u << NOTE_BIT_TERM, "and the caller's own thread holds the term");
    TEST_EXPECT_EQ(s_term, 0u, "and the queue does not");
    TEST_EXPECT_EQ(r_sprobe, (s64)0, "kill(0, 0) probes the caller");
    TEST_EXPECT_EQ(r_dead, (s64)0, "a dead, unreaped child accepts it, as Linux's zombie does");
    TEST_EXPECT_EQ(c_term3, 1u, "and nothing was queued to it");
    TEST_EXPECT_EQ(r_dprobe, (s64)0, "kill(dead child, 0) succeeds, as for a zombie");
    TEST_EXPECT_EQ(r_dtty, -(s64)T_E_INVAL, "SIGHUP to the dead child: the signal is judged before the walk");
    TEST_EXPECT_EQ(r_drange, -(s64)T_E_INVAL, "signal 65 to the dead child: EINVAL, not the zombie's 0");
    TEST_EXPECT_EQ(r_ntty, -(s64)T_E_INVAL, "SIGHUP to no such pid: EINVAL before ESRCH");
    TEST_EXPECT_EQ(r_odead, -(s64)T_E_PERM, "a dead non-child reads EPERM: the gate is asked first");
}

// ---------------------------------------------------------------------------
// Thread-directed notes (ARCH 7.6.4; specs/caught_target.tla). Two fixture threads
// of one Proc: neither runs, so each read is of what a post left. test_rendez.c
// holds the legs that need a thread actually blocked.
// ---------------------------------------------------------------------------
#define DN_TID_A 0x3f000011
#define DN_TID_B 0x3f000012
#define DN_NO_TID 0x3f0000fe
#define DN_HANDLER_VA 0x1000ull

static struct Thread g_dn_a, g_dn_b;

static struct Proc *dn_proc(void) {
    struct Proc *p = proc_alloc();
    if (!p) return NULL;
    p->state = PROC_STATE_ALIVE;
    kfg_thread(&g_dn_a, p, DN_TID_A);
    kfg_thread(&g_dn_b, p, DN_TID_B);
    g_dn_b.next_in_proc = &g_dn_a;
    return p;
}

static void dn_free(struct Proc *p) {
    p->threads = NULL;
    p->state   = PROC_STATE_ZOMBIE;
    proc_free(p);
}

static u32 dn_word(const struct Thread *t) {
    return __atomic_load_n(&t->note_direct, __ATOMIC_ACQUIRE);
}

static bool dn_same_name(const char *a, const char *b) {
    for (u32 i = 0; i < NOTE_NAME_MAX; i++) {
        if (a[i] != b[i]) return false;
        if (a[i] == 0) return true;
    }
    return true;
}

// The sender the newest queued note of this name recorded, or ~0u with none queued.
static u32 dn_queued_sender(struct Proc *p, const char *name) {
    struct NoteQueue *q = p->notes;
    u32 sender = ~0u;
    spin_lock(&q->lock);
    u32 idx = q->head;
    for (u32 n = 0; n < q->count; n++) {
        if (dn_same_name(q->ring[idx].name, name)) sender = q->ring[idx].sender_pid;
        idx = (idx + 1) % NOTE_RING_SLOTS;
    }
    spin_unlock(&q->lock);
    return sender;
}

static int dn_pop(struct Proc *p, struct Note *out) {
    spin_lock(&p->notes->lock);
    int r = notes_dequeue_locked(p, NULL, out);
    spin_unlock(&p->notes->lock);
    return r;
}

// A bit can stand for one name only because every name a program may post owns its
// family. A row added to the table is checked here without anyone extending a list.
void test_notes_direct_names_own_their_family(void) {
    struct Proc *p = dn_proc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    p->handler_va = DN_HANDLER_VA;
    u32 seen = 0, walked = 0;
    const char *name;
    for (u32 i = 0; (name = notes_known_name_for_test(i)) != NULL; i++) {
        walked++;
        int bit = notes_direct_bit(name);
        bool want = notes_name_user_postable(name) && !notes_name_is_kill(name);
        TEST_EXPECT_EQ(bit >= 0, want, "a name may land on the word iff a program may post it, and it is not kill");
        if (bit < 0) continue;
        TEST_ASSERT((u32)bit < 8u * sizeof(g_dn_a.note_direct), "the family fits note_direct");
        TEST_ASSERT(!(seen & (1u << (u32)bit)), "two postable names share a family: the word could not say which");
        seen |= 1u << (u32)bit;
        // What the word says back is the name that was posted.
        TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, name, NULL), 0, "the post lands");
        struct Note n;
        int direct = -2;
        TEST_EXPECT_EQ(notes_tail_candidate_for_test(p, &g_dn_a, &n, &direct), 1, "and the thread has a candidate");
        TEST_EXPECT_EQ(direct, bit, "the word's bit");
        TEST_ASSERT(dn_same_name(n.name, name), "whose name is the name posted");
        struct Note got;
        TEST_EXPECT_EQ(notes_tail_consume_for_test(p, &g_dn_a, direct, &got), 1, "consumed");
    }
    TEST_ASSERT(walked > 4 && seen != 0, "the walk saw the table");
    TEST_ASSERT(notes_direct_bit(NOTE_NAME_INTERRUPT) == NOTE_BIT_INTERRUPT &&
                notes_direct_bit(NOTE_NAME_TERM) == NOTE_BIT_TERM &&
                notes_direct_bit(NOTE_NAME_PIPE) == NOTE_BIT_PIPE &&
                notes_direct_bit(NOTE_NAME_CHILD_EXIT) == NOTE_BIT_CHILD_EXIT,
                "the four a program can post");
    TEST_ASSERT(notes_direct_bit(NOTE_NAME_KILL) < 0 && notes_direct_bit(NOTE_NAME_TTY_HUP) < 0 &&
                notes_direct_bit(NOTE_NAME_SAK) < 0 && notes_direct_bit("snare:segv") < 0 &&
                notes_direct_bit("nope") < 0, "never kill, a kernel-only name, or nonsense");
    dn_free(p);
}

void test_notes_direct_reaches_only_its_thread(void) {
    struct Proc *p = dn_proc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    p->handler_va = DN_HANDLER_VA;
    u32 const intr = 1u << NOTE_BIT_INTERRUPT, term = 1u << NOTE_BIT_TERM;
    struct Note n, got;
    int direct;

    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_INTERRUPT, NULL), 0, "post at a");
    TEST_EXPECT_EQ(dn_word(&g_dn_a), intr, "a holds it");
    TEST_EXPECT_EQ(dn_word(&g_dn_b), 0u, "b does not");
    TEST_EXPECT_EQ(p->notes->count, 0u, "nor does the Proc's queue");
    TEST_EXPECT_EQ(notes_tail_candidate_for_test(p, &g_dn_b, &n, &direct), 0, "b's tail finds nothing");
    TEST_EXPECT_EQ(notes_tail_candidate_for_test(p, &g_dn_a, &n, &direct), 1, "a's tail finds it");
    TEST_EXPECT_EQ(direct, (int)NOTE_BIT_INTERRUPT, "on the word");

    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_INTERRUPT, NULL), 0, "a second post");
    TEST_EXPECT_EQ(dn_word(&g_dn_a), intr, "coalesces");

    // Masked, it waits on this thread: the peer still cannot have it.
    g_dn_a.note_mask = intr;
    TEST_EXPECT_EQ(notes_tail_candidate_for_test(p, &g_dn_a, &n, &direct), 0, "masked: a's tail passes");
    TEST_EXPECT_EQ(notes_tail_candidate_for_test(p, &g_dn_b, &n, &direct), 0, "and b's still finds nothing");
    TEST_EXPECT_EQ(dn_word(&g_dn_a), intr, "it stays");
    g_dn_a.note_mask = 0;
    TEST_EXPECT_EQ(notes_tail_candidate_for_test(p, &g_dn_a, &n, &direct), 1, "unmasked: a's tail takes it");
    TEST_EXPECT_EQ(notes_tail_consume_for_test(p, &g_dn_a, direct, &got), 1, "consumed");
    TEST_ASSERT(dn_same_name(got.name, NOTE_NAME_INTERRUPT), "the interrupt");
    TEST_EXPECT_EQ(dn_word(&g_dn_a), 0u, "exactly once: the word is clear");
    TEST_EXPECT_EQ(notes_tail_consume_for_test(p, &g_dn_a, direct, &got), 0, "and a second consume finds nothing");

    // The control, one variable away: a queued note reaches the peer's tail, so
    // b's refusals above were the word's doing.
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_INTERRUPT, 0u, NULL, true), 0, "queue an interrupt");
    TEST_EXPECT_EQ(notes_tail_candidate_for_test(p, &g_dn_b, &n, &direct), 1, "control: b can take a queued note");
    TEST_EXPECT_EQ(direct, -1, "from the queue");

    // The thread's own note comes first, as Linux's dequeue takes private before
    // shared. Lowest family first on the word.
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_TERM, NULL), 0, "post term at a");
    TEST_EXPECT_EQ(dn_word(&g_dn_a), term, "a holds term");
    TEST_EXPECT_EQ(notes_tail_candidate_for_test(p, &g_dn_a, &n, &direct), 1, "a's tail");
    TEST_EXPECT_EQ(direct, (int)NOTE_BIT_TERM, "takes its own before the queued interrupt");
    TEST_ASSERT(dn_same_name(n.name, NOTE_NAME_TERM), "the term");

    // A queued kill beats everything: it is the group's end.
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_KILL, 0u, NULL, true), 0, "queue a kill");
    TEST_EXPECT_EQ(notes_tail_candidate_for_test(p, &g_dn_a, &n, &direct), 1, "a's tail");
    TEST_EXPECT_EQ(direct, -1, "takes the kill first");
    TEST_ASSERT(notes_name_is_kill(n.name), "and it is the kill");
    dn_free(p);
}

void test_notes_direct_post_gates(void) {
    struct Proc *p = dn_proc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    p->handler_va = DN_HANDLER_VA;
    u32 const term = 1u << NOTE_BIT_TERM;

    TEST_EXPECT_EQ(proc_note_thread(p, DN_NO_TID, NOTE_NAME_TERM, NULL), -(int)T_E_SRCH, "no such thread");
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_KILL, NULL), -(int)T_E_INVAL, "kill never lands on the word");
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_TTY_HUP, NULL), -(int)T_E_INVAL, "nor a kernel-only name");
    TEST_EXPECT_EQ(proc_note_thread(p, DN_NO_TID, "nope", NULL), -(int)T_E_INVAL, "the name is judged before the tid");
    TEST_EXPECT_EQ(dn_word(&g_dn_a), 0u, "nothing landed");

    // An exiting thread accepts it and nothing receives it, as a dead child does.
    g_dn_a.state = THREAD_EXITING;
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_TERM, NULL), 0, "an exiting thread answers 0");
    TEST_EXPECT_EQ(dn_word(&g_dn_a), 0u, "and holds nothing");
    g_dn_a.state = THREAD_RUNNING;

    p->state = PROC_STATE_ZOMBIE;
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_TERM, NULL), 0, "a dead Proc answers 0");
    TEST_EXPECT_EQ(dn_word(&g_dn_a), 0u, "and holds nothing");
    p->state = PROC_STATE_ALIVE;

    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_TERM, NULL), 0, "control: alive and running, it lands");
    TEST_EXPECT_EQ(dn_word(&g_dn_a), term, "on a");
    __atomic_store_n(&g_dn_a.note_direct, 0, __ATOMIC_RELEASE);

    // SYS_POSTNOTE_TID past its copy: the name, then the tid, then any death.
    TEST_EXPECT_EQ(sys_postnote_tid_for_test(p, DN_TID_A, "nope"), -(s64)T_E_INVAL, "a bad name");
    TEST_EXPECT_EQ(sys_postnote_tid_for_test(p, DN_NO_TID, "nope"), -(s64)T_E_INVAL, "a bad name before a bad tid");
    TEST_EXPECT_EQ(sys_postnote_tid_for_test(p, DN_TID_A, NOTE_NAME_TTY_HUP), -(s64)T_E_INVAL, "a kernel-only name");
    TEST_EXPECT_EQ(sys_postnote_tid_for_test(p, DN_NO_TID, NOTE_NAME_TERM), -(s64)T_E_SRCH, "a thread that is gone");
    TEST_EXPECT_EQ(sys_postnote_tid_for_test(p, DN_NO_TID, NOTE_NAME_KILL), -(s64)T_E_SRCH, "ends nothing, even for kill");
    TEST_EXPECT_EQ(sys_postnote_tid_for_test(p, DN_NO_TID, "snare:segv"), -(s64)T_E_SRCH, "or a snare");
    TEST_EXPECT_EQ(sys_postnote_tid_for_test(p, DN_TID_A, "snare:segv"), (s64)1, "a named thread reaches the snare death");
    TEST_EXPECT_EQ(sys_postnote_tid_for_test(p, DN_TID_B, NOTE_NAME_TERM), (s64)0, "a plain post");
    TEST_EXPECT_EQ(dn_word(&g_dn_b), term, "lands on the thread it named");
    TEST_EXPECT_EQ(dn_word(&g_dn_a), 0u, "and no other");
    dn_free(p);
}

void test_notes_direct_ignored_and_discarded(void) {
    struct Proc *p = dn_proc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    struct viv_sigtab *tab = (struct viv_sigtab *)kzalloc(sizeof(struct viv_sigtab), 0);
    TEST_ASSERT(tab != NULL, "sigtab alloc");
    p->sigtab    = tab;                // freed by proc_free
    p->phenotype = PHENO_LINUX;
    u32 const intr = 1u << NOTE_BIT_INTERRUPT, term = 1u << NOTE_BIT_TERM;

    // Both threads hold an interrupt, a term on a beside it. rt_sigaction ignoring
    // interrupt reaches every thread, and only that family.
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_INTERRUPT, NULL), 0, "post at a");
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_TERM, NULL), 0, "and term at a");
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_B, NOTE_NAME_INTERRUPT, NULL), 0, "post at b");
    TEST_EXPECT_EQ(dn_word(&g_dn_a), intr | term, "a holds both");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_INTERRUPT, 0u, NULL, true), 0, "and one queued");
    TEST_EXPECT_EQ(notes_discard_name(p, NOTE_NAME_INTERRUPT), 3u, "the queue's and both words'");
    TEST_EXPECT_EQ(dn_word(&g_dn_a), term, "a keeps term");
    TEST_EXPECT_EQ(dn_word(&g_dn_b), 0u, "b holds nothing");
    TEST_EXPECT_EQ(p->notes->count, 0u, "the queue is empty");
    TEST_EXPECT_EQ(notes_discard_name(p, NOTE_NAME_INTERRUPT), 0u, "a second discard finds nothing");
    TEST_EXPECT_EQ(notes_discard_name(p, NOTE_NAME_KILL), 0u, "a kill is never discarded");

    // Ignored already: the post refuses it, so it never takes a bit.
    struct viv_ksigaction ign = { .handler = VIV_SIG_IGN, .flags = 0, .restorer = 0, .mask = 0 };
    TEST_ASSERT(viv_sigtab_set(tab, VIV_SIGNOTE_INTERRUPT, &ign), "SIG_IGN interrupt");
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_B, NOTE_NAME_INTERRUPT, NULL), 0, "an ignored post answers 0");
    TEST_EXPECT_EQ(dn_word(&g_dn_b), 0u, "and holds nothing");
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_B, NOTE_NAME_PIPE, NULL), 0, "control: pipe is not ignored");
    TEST_EXPECT_EQ(dn_word(&g_dn_b), 1u << NOTE_BIT_PIPE, "and lands");
    dn_free(p);
}

// The race the candidate + consume split exists for: q->lock does not guard the
// word, so an rt_sigaction on a peer can take a bit between the tail's peek and its
// consume. Then the tail delivers nothing; it must never run the frame it built on
// whatever note came next.
void test_notes_direct_candidate_discarded_delivers_nothing(void) {
    struct Proc *p = dn_proc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    p->handler_va = DN_HANDLER_VA;
    struct Note n, got;
    int direct;
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_CHILD_EXIT, 0u, NULL, true), 0, "a queued note behind it");
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_INTERRUPT, NULL), 0, "post at a");
    TEST_EXPECT_EQ(notes_tail_candidate_for_test(p, &g_dn_a, &n, &direct), 1, "the tail peeks");
    TEST_EXPECT_EQ(direct, (int)NOTE_BIT_INTERRUPT, "the word's");
    TEST_EXPECT_EQ(notes_discard_name(p, NOTE_NAME_INTERRUPT), 1u, "a peer's rt_sigaction takes it");
    TEST_EXPECT_EQ(notes_tail_consume_for_test(p, &g_dn_a, direct, &got), 0, "the consume finds it gone");
    TEST_EXPECT_EQ(p->notes->count, 1u, "and popped nothing else");

    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_INTERRUPT, NULL), 0, "control: post again");
    TEST_EXPECT_EQ(notes_tail_candidate_for_test(p, &g_dn_a, &n, &direct), 1, "peek");
    TEST_EXPECT_EQ(notes_tail_consume_for_test(p, &g_dn_a, direct, &got), 1, "undisturbed, it is consumed");
    TEST_ASSERT(dn_same_name(got.name, NOTE_NAME_INTERRUPT), "the interrupt");
    dn_free(p);
}

// A note whose default ends the Proc, read at the moment of asking, for this thread.
void test_notes_direct_die_predicate(void) {
    struct Proc *p = dn_proc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    u32 const term = 1u << NOTE_BIT_TERM;

    TEST_ASSERT(!thread_die_pending(&g_dn_a), "fresh: a would not die");
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_TERM, NULL), 0, "post term at a, no handler");
    TEST_EXPECT_EQ(p->notes->count, 0u, "the queue holds nothing");
    TEST_ASSERT(thread_die_pending(&g_dn_a), "a dies of it");
    TEST_ASSERT(!thread_die_pending(&g_dn_b), "b, which it does not name, does not");
    TEST_ASSERT(!proc_intr_terminate_pending(p), "and no group latch armed");

    g_dn_a.note_mask = term;
    TEST_ASSERT(!thread_die_pending(&g_dn_a), "masked, it waits behind that thread's mask");
    g_dn_a.note_mask = 0;
    TEST_ASSERT(thread_die_pending(&g_dn_a), "unmasked, it is fatal again");

    // Read at use, not latched at the post: a handler makes it a caught note, and
    // losing the handler makes it fatal again.
    __atomic_store_n(&p->handler_va, DN_HANDLER_VA, __ATOMIC_RELEASE);
    TEST_ASSERT(!thread_die_pending(&g_dn_a), "with a handler it is caught");
    __atomic_store_n(&p->handler_va, 0ull, __ATOMIC_RELEASE);
    TEST_ASSERT(thread_die_pending(&g_dn_a), "without one, fatal");

    // A note whose default is to ignore never kills.
    __atomic_store_n(&g_dn_a.note_direct, 1u << NOTE_BIT_CHILD_EXIT, __ATOMIC_RELEASE);
    TEST_ASSERT(!thread_die_pending(&g_dn_a), "child_exit's default ignores");
    __atomic_store_n(&g_dn_a.note_direct, 0, __ATOMIC_RELEASE);
    dn_free(p);
}

// A caught note unwinds its one taker with no family claim, in a phenotype Proc;
// a native wait rides it out (item 11c).
void test_notes_direct_deliverable_takes_no_claim(void) {
    struct Proc *p = dn_proc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    struct viv_sigtab *tab = (struct viv_sigtab *)kzalloc(sizeof(struct viv_sigtab), 0);
    TEST_ASSERT(tab != NULL, "sigtab alloc");
    p->sigtab    = tab;                // freed by proc_free
    p->phenotype = PHENO_LINUX;
    struct viv_ksigaction hand = { .handler = 0x1000, .flags = 0, .restorer = 0x2000, .mask = 0 };
    TEST_ASSERT(viv_sigtab_set(tab, VIV_SIGNOTE_INTERRUPT, &hand), "catch interrupt");
    g_dn_a.note_interruptible = true;
    g_dn_b.note_interruptible = true;
    u32 const intr = 1u << NOTE_BIT_INTERRUPT;

    TEST_ASSERT(!thread_caught_note_deliverable(&g_dn_a), "fresh: nothing to deliver");
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_INTERRUPT, NULL), 0, "post at a");
    TEST_ASSERT(thread_caught_note_deliverable(&g_dn_a), "a's listed wait would unwind");
    TEST_ASSERT(!thread_caught_note_deliverable(&g_dn_b), "b's would not");
    TEST_ASSERT(!thread_die_pending(&g_dn_a), "and it is caught, not fatal");
    TEST_ASSERT(thread_caught_note_claim(&g_dn_a), "a's unwind test passes");
    TEST_EXPECT_EQ(__atomic_load_n(&g_dn_a.note_claim, __ATOMIC_ACQUIRE), 0u, "holding no claim");
    TEST_EXPECT_EQ(__atomic_load_n(&p->note_caught, __ATOMIC_ACQUIRE), 0u, "and arming no family bit");

    g_dn_a.note_mask = intr;
    TEST_ASSERT(!thread_caught_note_deliverable(&g_dn_a), "masked: not deliverable");
    g_dn_a.note_mask = 0;
    g_dn_a.in_handler = true;
    TEST_ASSERT(!thread_caught_note_deliverable(&g_dn_a), "in a handler: not deliverable (N-3)");
    g_dn_a.in_handler = false;
    g_dn_a.note_interruptible = false;
    TEST_ASSERT(!thread_caught_note_deliverable(&g_dn_a), "a wait a signal may not interrupt rides it out");
    g_dn_a.note_interruptible = true;
    TEST_ASSERT(thread_caught_note_deliverable(&g_dn_a), "control: all three lifted, deliverable");
    g_dn_a.note_interruptible = false;
    g_dn_b.note_interruptible = false;
    dn_free(p);
}

// A Proc reading its notes from an fd has no per-thread reader, and runs no handler.
void test_notes_direct_self_managing_uses_the_queue(void) {
    struct Proc *p = dn_proc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    p->handler_va = DN_HANDLER_VA;
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_TERM, NULL), 0, "post at a");
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_B, NOTE_NAME_INTERRUPT, NULL), 0, "post at b");
    TEST_EXPECT_EQ(p->notes->count, 0u, "before the mark, each on its word");
    __atomic_store_n(&p->handler_va, 0ull, __ATOMIC_RELEASE);

    notes_mark_self_managing(p);
    TEST_EXPECT_EQ(dn_word(&g_dn_a), 0u, "the mark moves a's word");
    TEST_EXPECT_EQ(dn_word(&g_dn_b), 0u, "and b's");
    TEST_EXPECT_EQ(ndflt_note_count(p, NOTE_NAME_TERM), 1u, "to the queue the fd reads");
    TEST_EXPECT_EQ(ndflt_note_count(p, NOTE_NAME_INTERRUPT), 1u, "both of them");
    TEST_EXPECT_EQ(dn_queued_sender(p, NOTE_NAME_TERM), 0u, "a moved note records no sender");

    TEST_ASSERT(p->pid != 0, "a sender is told from none by its pid");
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_PIPE, p), 0, "after the mark");
    TEST_EXPECT_EQ(dn_word(&g_dn_a), 0u, "a post takes no bit");
    TEST_EXPECT_EQ(ndflt_note_count(p, NOTE_NAME_PIPE), 1u, "and joins the queue");
    TEST_EXPECT_EQ(dn_queued_sender(p, NOTE_NAME_PIPE), (u32)p->pid, "with its sender");
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_CHILD_EXIT, NULL), 0, "control: no sender");
    TEST_EXPECT_EQ(dn_queued_sender(p, NOTE_NAME_CHILD_EXIT), 0u, "control: none recorded");
    TEST_EXPECT_EQ(proc_note_thread(p, DN_NO_TID, NOTE_NAME_PIPE, NULL), -(int)T_E_SRCH, "the tid is still judged");
    dn_free(p);
}

// Every way a program reaches the self-managing fallback names who sent the note.
void test_notes_direct_fallback_records_its_sender(void) {
    struct Proc *caller = proc_alloc();
    struct Proc *child  = proc_alloc();
    TEST_ASSERT(caller && child, "proc_alloc x2");
    proc_test_link(caller);
    proc_test_link_child(caller, child);
    static struct Thread cth, kth;
    kfg_thread(&cth, caller, 0x3f000021);
    kfg_thread(&kth, child,  0x3f000022);
    notes_mark_self_managing(caller);
    notes_mark_self_managing(child);

    s64 r_cross = viv_kill_for_test(caller, child->pid, kth.tid, VIV_SIGINT);
    u32 s_cross = dn_queued_sender(child, NOTE_NAME_INTERRUPT);
    s64 r_self  = viv_kill_for_test(caller, 0, cth.tid, VIV_SIGTERM);
    u32 s_self  = dn_queued_sender(caller, NOTE_NAME_TERM);
    s64 r_tid   = sys_postnote_tid_for_test(caller, cth.tid, NOTE_NAME_PIPE);
    u32 s_tid   = dn_queued_sender(caller, NOTE_NAME_PIPE);
    u32 caller_pid = (u32)caller->pid;

    child->state = PROC_STATE_ZOMBIE;
    proc_test_unlink(child);
    child->threads = NULL;
    proc_free(child);
    proc_test_unlink(caller);
    caller->threads = NULL;
    caller->state   = PROC_STATE_ZOMBIE;
    proc_free(caller);

    TEST_ASSERT(caller_pid != 0, "a sender is told from none by its pid");
    TEST_EXPECT_EQ(r_cross, (s64)0, "tgkill at a child's thread posts");
    TEST_EXPECT_EQ(s_cross, caller_pid, "and the child's queue names the parent");
    TEST_EXPECT_EQ(r_self, (s64)0, "tkill at the caller's own thread posts");
    TEST_EXPECT_EQ(s_self, caller_pid, "and names the caller");
    TEST_EXPECT_EQ(r_tid, (s64)0, "SYS_POSTNOTE_TID posts");
    TEST_EXPECT_EQ(s_tid, caller_pid, "and names the caller");
}

// A direct post answered 0 before the mark, so the move to the queue cannot be refused.
// A post is still refused at the depth; each moved family takes a slot past it, and a
// move of a name still queued coalesces, which is why those slots cannot run out.
void test_notes_direct_move_survives_a_full_ring(void) {
    struct Proc *p = dn_proc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    TEST_ASSERT(p->pid != 0, "a post's sender is told from a move's none");
    p->handler_va = DN_HANDLER_VA;
    for (u32 i = 0; i < NOTE_QUEUE_DEPTH; i++)
        TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_INTERRUPT, i, p, false), 0, "fill to the depth");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_INTERRUPT, 99u, p, false), -(int)T_E_AGAIN, "the depth refuses a post");
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_TERM, NULL), 0, "term at a");
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_PIPE, NULL), 0, "pipe at a");
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_CHILD_EXIT, NULL), 0, "child_exit at a");
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_B, NOTE_NAME_INTERRUPT, NULL), 0, "interrupt at b");
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_B, NOTE_NAME_TERM, NULL), 0, "term at b");

    notes_mark_self_managing(p);
    TEST_EXPECT_EQ(dn_word(&g_dn_a), 0u, "the mark emptied a's word");
    TEST_EXPECT_EQ(dn_word(&g_dn_b), 0u, "and b's");
    TEST_EXPECT_EQ(p->notes->count, NOTE_QUEUE_DEPTH + 4u, "each family took a slot past the depth");
    TEST_EXPECT_EQ(ndflt_note_count(p, NOTE_NAME_TERM), 1u, "term from two words is one family");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_TERM, 98u, p, false), -(int)T_E_AGAIN, "past the depth a post is refused");

    // A pop past the depth still has its slot to go back to, as kill's re-enqueue needs.
    struct Note n;
    TEST_EXPECT_EQ(dn_pop(p, &n), 1, "a pop past the depth");
    spin_lock(&p->notes->lock);
    int re = notes_reenqueue_head_locked(p->notes, &n);
    spin_unlock(&p->notes->lock);
    TEST_EXPECT_EQ(re, 0, "and its re-enqueue");
    TEST_EXPECT_EQ(p->notes->count, NOTE_QUEUE_DEPTH + 4u, "leaves the count");

    for (u32 i = 0; i < NOTE_QUEUE_DEPTH; i++) {
        TEST_EXPECT_EQ(dn_pop(p, &n), 1, "the posts first");
        TEST_EXPECT_EQ(n.arg, i, "in the order posted");
        TEST_EXPECT_EQ(n.sender_pid, (u32)p->pid, "with their sender");
    }
    int last_bit = -1;
    for (u32 i = 0; i < 4u; i++) {
        TEST_EXPECT_EQ(dn_pop(p, &n), 1, "then the moved families");
        TEST_ASSERT(notes_direct_bit(n.name) > last_bit, "each once, in bit order");
        last_bit = notes_direct_bit(n.name);
        TEST_EXPECT_EQ(n.sender_pid, 0u, "with no sender");
    }
    TEST_EXPECT_EQ(p->notes->count, 0u, "drained");

    // The next fill crosses the ring's last slot, so the walks below wrap there.
    for (u32 i = 0; i < NOTE_QUEUE_DEPTH; i++)
        TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_INTERRUPT, 100u + i, p, false), 0, "fill again");
    __atomic_and_fetch(&p->proc_flags, ~PROC_FLAG_SELF_MANAGING_NOTES, __ATOMIC_RELEASE);
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_TERM, NULL), 0, "term again");
    notes_mark_self_managing(p);
    TEST_EXPECT_EQ(p->notes->count, NOTE_QUEUE_DEPTH + 1u, "takes a slot past the depth");
    __atomic_and_fetch(&p->proc_flags, ~PROC_FLAG_SELF_MANAGING_NOTES, __ATOMIC_RELEASE);
    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_TERM, NULL), 0, "term once more");
    notes_mark_self_managing(p);
    TEST_EXPECT_EQ(p->notes->count, NOTE_QUEUE_DEPTH + 1u, "coalesces with the one queued");
    TEST_EXPECT_EQ(ndflt_note_count(p, NOTE_NAME_TERM), 1u, "one term");
    for (u32 i = 0; i < NOTE_QUEUE_DEPTH; i++) {
        TEST_EXPECT_EQ(dn_pop(p, &n), 1, "the wrapped fill");
        TEST_EXPECT_EQ(n.arg, 100u + i, "in order across the wrap");
    }
    TEST_EXPECT_EQ(dn_pop(p, &n), 1, "then term");
    TEST_ASSERT(dn_same_name(n.name, NOTE_NAME_TERM), "named term");
    TEST_EXPECT_EQ(p->notes->count, 0u, "drained again");
    dn_free(p);
}

// A consumed direct note goes back when its frame's push faults. If a peer opened the
// note fd while it was out, nothing will ever read a word again, so it goes to the queue.
void test_notes_direct_unconsume_after_the_mark_goes_to_the_queue(void) {
    struct Proc *p = dn_proc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    p->handler_va = DN_HANDLER_VA;
    const u32 term = 1u << (u32)NOTE_BIT_TERM;
    struct Note n;
    int direct = -1;

    TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, NOTE_NAME_TERM, NULL), 0, "post at a");
    TEST_EXPECT_EQ(notes_tail_candidate_for_test(p, &g_dn_a, &n, &direct), 1, "a's tail finds it");
    TEST_EXPECT_EQ(direct, (int)NOTE_BIT_TERM, "on the word");
    TEST_EXPECT_EQ(notes_tail_consume_for_test(p, &g_dn_a, direct, &n), 1, "and consumes it");
    TEST_EXPECT_EQ(dn_word(&g_dn_a), 0u, "the word is empty");
    notes_tail_unconsume_for_test(p, &g_dn_a, direct, &n);
    TEST_EXPECT_EQ(dn_word(&g_dn_a), term, "control: with no mark it goes back on the word");
    TEST_EXPECT_EQ(p->notes->count, 0u, "control: and not to the queue");

    TEST_EXPECT_EQ(notes_tail_consume_for_test(p, &g_dn_a, direct, &n), 1, "consumed again");
    notes_mark_self_managing(p);
    notes_tail_unconsume_for_test(p, &g_dn_a, direct, &n);
    TEST_EXPECT_EQ(dn_word(&g_dn_a), 0u, "after the mark no word holds it");
    TEST_EXPECT_EQ(ndflt_note_count(p, NOTE_NAME_TERM), 1u, "the queue does");
    dn_free(p);
}

// The user-defined notes: each terminates by default and records its own signal, on
// a latch of its own, so blocking one leaves the other fatal. Red before they existed,
// when neither name was a note and the post was refused.
void test_notes_usr_notes_die_apart(void) {
    static const struct {
        const char *name;
        u32 bit, other_bit, latch, other_latch;
        int sig;
    } rows[2] = {
        { NOTE_NAME_USR1, NOTE_BIT_USR1, NOTE_BIT_USR2, PROC_FLAG_USR1_TERMINATE_PENDING,
          PROC_FLAG_USR2_TERMINATE_PENDING, 10 },
        { NOTE_NAME_USR2, NOTE_BIT_USR2, NOTE_BIT_USR1, PROC_FLAG_USR2_TERMINATE_PENDING,
          PROC_FLAG_USR1_TERMINATE_PENDING, 12 },
    };
    for (u32 i = 0; i < 2u; i++) {
        struct Proc *p = proc_alloc();
        TEST_ASSERT(p != NULL, "proc_alloc succeeded");

        struct Thread fake_t;
        fake_t.note_direct         = 0;
        fake_t.proc                = p;
        fake_t.note_mask           = 0u;
        fake_t.exit_close_active   = false;
        fake_t.kthread_join_active = false;

        const char *name = rows[i].name;
        bool fresh = thread_die_pending(&fake_t);
        int  dfl   = (int)notes_default_action(name);
        int  sig   = notes_death_signal(name);
        int  bit   = notes_direct_bit(name);
        int  post  = notes_post(p, name, 0u, NULL, false);
        u32  flags = __atomic_load_n(&p->proc_flags, __ATOMIC_ACQUIRE);
        bool gate  = proc_intr_terminate_pending(p);
        bool open_ = thread_die_pending(&fake_t);
        fake_t.note_mask = (u64)1u << rows[i].other_bit;
        bool other_masked = thread_die_pending(&fake_t);
        fake_t.note_mask = (u64)1u << rows[i].bit;
        bool own_masked = thread_die_pending(&fake_t);
        fake_t.note_mask = 0u;
        struct Note got;
        got.name[0] = 0;
        spin_lock(&p->notes->lock);
        int deq = notes_dequeue_for_fd_locked(p, NULL, &got);
        spin_unlock(&p->notes->lock);
        bool got_it  = dn_same_name(got.name, name);
        bool drained = thread_die_pending(&fake_t);

        p->state = PROC_STATE_ZOMBIE;
        proc_free(p);

        TEST_ASSERT(!fresh, "fresh Proc -> false");
        TEST_EXPECT_EQ(dfl, (int)NOTE_DFL_TERMINATE, "its default action is TERMINATE");
        TEST_EXPECT_EQ(sig, rows[i].sig, "a death by it records its own signal");
        TEST_EXPECT_EQ(bit, (int)rows[i].bit, "a program may aim it at a thread, on its own bit");
        TEST_EXPECT_EQ(post, 0, "a userspace post of it is accepted");
        TEST_ASSERT((flags & rows[i].latch) != 0, "it arms its own latch");
        TEST_ASSERT((flags & rows[i].other_latch) == 0, "and not its twin's");
        TEST_ASSERT(gate, "the wake gate fires on it");
        TEST_ASSERT(open_, "armed + unmasked -> true");
        TEST_ASSERT(other_masked, "its twin masked -> still true (per-family)");
        TEST_ASSERT(!own_masked, "itself masked -> false");
        TEST_ASSERT(deq >= 0 && got_it, "the queued note is it");
        TEST_ASSERT(!drained, "drained -> the latch clears -> false");
    }
}

static u32 fb_latch(const struct Proc *p) {
    return (__atomic_load_n(&p->note_caught, __ATOMIC_ACQUIRE) & CAUGHT_LATCH_MASK) >> CAUGHT_LATCH_SHIFT;
}
static u32 fb_claim(const struct Proc *p) {
    return (__atomic_load_n(&p->note_caught, __ATOMIC_ACQUIRE) & CAUGHT_CLAIM_MASK) >> CAUGHT_CLAIM_SHIFT;
}

// A family bit a byte cannot hold, through every word it passes: the latch, the claim
// half, a thread's claim, its designation, a hand-off, and the direct word's post,
// consume, unconsume, discard and move. Each is a cast a narrower type turns into 0,
// which compiles.
void test_notes_family_past_the_first_byte(void) {
    const u32 usr1 = 1u << NOTE_BIT_USR1, usr2 = 1u << NOTE_BIT_USR2;

    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    p->state     = PROC_STATE_ALIVE;
    p->phenotype = PHENO_LINUX;
    notes_set_handler(p, 0x1000u);

    static struct Thread a, b;          // BSS-zeroed, reset each run
    a.proc = b.proc = p;
    a.note_mask = b.note_mask = 0u;
    a.exit_close_active = b.exit_close_active = false;
    a.in_handler = b.in_handler = false;
    a.note_interruptible = b.note_interruptible = true;
    a.note_claim = b.note_claim = 0u;
    a.note_target = b.note_target = 0u;
    a.note_direct = b.note_direct = 0u;

    int  post   = notes_post(p, NOTE_NAME_USR2, 0u, NULL, true);
    u32  latch  = fb_latch(p);
    bool a1     = thread_caught_note_claim(&a);
    u32  a_rec  = a.note_claim;
    u32  held   = fb_claim(p);
    bool b1     = thread_caught_note_claim(&b);

    // A drain clears its latch bit and leaves the claim half alone.
    struct Note got;
    spin_lock(&p->notes->lock);
    int  drain  = notes_dequeue_for_fd_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    u32  latch0 = fb_latch(p);
    u32  held1  = fb_claim(p);
    notes_release_claims_for_test(&a);
    u32  a_rec0 = a.note_claim;
    u32  held0  = fb_claim(p);

    // A designation of the family becomes the claim when its thread unwinds.
    int  post2  = notes_post(p, NOTE_NAME_USR2, 0u, NULL, true);
    __atomic_or_fetch(&p->note_caught, usr2 << CAUGHT_CLAIM_SHIFT, __ATOMIC_RELEASE);
    b.note_target = (u16)usr2;
    bool b2     = thread_caught_note_claim(&b);
    u32  b_rec  = b.note_claim;
    u32  b_tgt  = b.note_target;
    notes_release_claims_for_test(&b);

    // A hand-off keeps the family it is told to, and gives up the claim bit otherwise.
    __atomic_or_fetch(&p->note_caught, usr2 << CAUGHT_CLAIM_SHIFT, __ATOMIC_RELEASE);
    a.note_target = (u16)usr2;
    notes_handoff_target(&a, (u16)usr2);
    u32  kept   = a.note_target;
    u32  heldk  = fb_claim(p);
    notes_handoff_target(&a, 0u);
    u32  gone   = a.note_target;
    u32  heldg  = fb_claim(p);

    // Two families at once. Giving one up leaves the other's claim alone, a masked
    // designation stays when its twin becomes the claim, and an exit gives up only
    // what the thread holds: a mask cast to 16 bits would clear the whole claim half.
    const u32 both = usr1 | usr2;
    __atomic_or_fetch(&p->note_caught, both << CAUGHT_CLAIM_SHIFT, __ATOMIC_RELEASE);
    a.note_target = (u16)usr1;
    notes_handoff_target(&a, 0u);
    u32  held2  = fb_claim(p);

    int  post3  = notes_post(p, NOTE_NAME_USR1, 0u, NULL, true);
    __atomic_or_fetch(&p->note_caught, both << CAUGHT_CLAIM_SHIFT, __ATOMIC_RELEASE);
    b.note_mask   = usr2;
    b.note_target = (u16)both;
    bool b3     = thread_caught_note_claim(&b);
    u32  b_rec3 = b.note_claim;
    u32  b_tgt3 = b.note_target;
    b.note_mask   = 0u;
    b.note_target = 0u;
    notes_release_claims_for_test(&b);

    __atomic_or_fetch(&p->note_caught, both << CAUGHT_CLAIM_SHIFT, __ATOMIC_RELEASE);
    a.note_claim  = (u16)usr1;
    irq_state_t ts = proc_threads_lock(p);
    notes_exit_drop_locked(&a);
    proc_threads_unlock(p, ts);
    u32  a_rec3 = a.note_claim;
    u32  held3  = fb_claim(p);

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);

    TEST_EXPECT_EQ(post, 0, "a caught usr2 is accepted");
    TEST_EXPECT_EQ(latch, usr2, "its bit in the latch half");
    TEST_ASSERT(a1, "the first sleeper claims it");
    TEST_EXPECT_EQ(a_rec, usr2, "and records it");
    TEST_EXPECT_EQ(held, usr2, "its bit in the claim half");
    TEST_ASSERT(!b1, "a second sleeper is refused: the claim was seen");
    TEST_ASSERT(drain >= 0, "the drain pops it");
    TEST_EXPECT_EQ(latch0, 0u, "the latch bit cleared");
    TEST_EXPECT_EQ(held1, usr2, "the claim survived the drain");
    TEST_EXPECT_EQ(a_rec0, 0u, "the release empties the thread's record");
    TEST_EXPECT_EQ(held0, 0u, "and the claim half");
    TEST_EXPECT_EQ(post2, 0, "a second usr2 is accepted");
    TEST_ASSERT(b2, "the designee unwinds");
    TEST_EXPECT_EQ(b_rec, usr2, "its designation became its claim");
    TEST_EXPECT_EQ(b_tgt, 0u, "and went");
    TEST_EXPECT_EQ(kept, usr2, "a hand-off keeping the family leaves the designation");
    TEST_EXPECT_EQ(heldk, usr2, "and the claim bit");
    TEST_EXPECT_EQ(gone, 0u, "one that does not gives the designation up");
    TEST_EXPECT_EQ(heldg, 0u, "and the claim bit");
    TEST_EXPECT_EQ(held2, usr2, "a hand-off of usr1 leaves usr2's claim");
    TEST_EXPECT_EQ(post3, 0, "a caught usr1 is accepted");
    TEST_ASSERT(b3, "a designee of both, usr2 masked, unwinds");
    TEST_EXPECT_EQ(b_rec3, usr1, "for usr1 alone");
    TEST_EXPECT_EQ(b_tgt3, usr2, "and keeps the masked designation");
    TEST_EXPECT_EQ(a_rec3, 0u, "an exit empties the thread's record");
    TEST_EXPECT_EQ(held3, usr2, "and gives up its own family only");

    struct Proc *d = dn_proc();
    TEST_ASSERT(d != NULL, "proc_alloc succeeded");
    d->handler_va = DN_HANDLER_VA;
    struct Note n;
    int direct = -1;
    TEST_EXPECT_EQ(proc_note_thread(d, DN_TID_A, NOTE_NAME_USR2, NULL), 0, "post at a");
    TEST_EXPECT_EQ(dn_word(&g_dn_a), usr2, "on a's word");
    TEST_EXPECT_EQ(dn_word(&g_dn_b), 0u, "and not b's");
    TEST_EXPECT_EQ(notes_tail_candidate_for_test(d, &g_dn_a, &n, &direct), 1, "a's tail finds it");
    TEST_EXPECT_EQ(direct, (int)NOTE_BIT_USR2, "as this family");
    TEST_ASSERT(dn_same_name(n.name, NOTE_NAME_USR2), "by name");
    TEST_EXPECT_EQ(notes_tail_consume_for_test(d, &g_dn_a, direct, &n), 1, "and consumes it");
    TEST_EXPECT_EQ(dn_word(&g_dn_a), 0u, "the word is empty");
    notes_tail_unconsume_for_test(d, &g_dn_a, direct, &n);
    TEST_EXPECT_EQ(dn_word(&g_dn_a), usr2, "an unconsume puts it back");
    TEST_EXPECT_EQ(notes_discard_name(d, NOTE_NAME_USR2), 1u, "a discard takes it");
    TEST_EXPECT_EQ(dn_word(&g_dn_a), 0u, "off the word");
    TEST_EXPECT_EQ(proc_note_thread(d, DN_TID_A, NOTE_NAME_USR2, NULL), 0, "post again");
    notes_mark_self_managing(d);
    TEST_EXPECT_EQ(dn_word(&g_dn_a), 0u, "the mark empties the word");
    TEST_EXPECT_EQ(ndflt_note_count(d, NOTE_NAME_USR2), 1u, "into the queue");
    dn_free(d);
}

// NOTE_RING_SPARE is one per family, and a full ring must take every family a program
// can aim at a thread: the table is walked, so a new row is counted.
void test_notes_every_direct_family_fits_past_a_full_ring(void) {
    struct Proc *p = dn_proc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");
    p->handler_va = DN_HANDLER_VA;
    for (u32 i = 0; i < NOTE_QUEUE_DEPTH; i++)
        TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_INTERRUPT, i, p, false), 0, "fill to the depth");
    u32 families = 0;
    const char *name;
    for (u32 i = 0; (name = notes_known_name_for_test(i)) != NULL; i++) {
        if (notes_direct_bit(name) < 0) continue;
        families++;
        TEST_EXPECT_EQ(proc_note_thread(p, DN_TID_A, name, NULL), 0, "the post lands");
    }
    TEST_ASSERT(families >= 6u, "interrupt, pipe, child_exit, term, usr1 and usr2 at least");
    notes_mark_self_managing(p);
    TEST_EXPECT_EQ(dn_word(&g_dn_a), 0u, "the mark emptied the word");
    TEST_EXPECT_EQ(p->notes->count, NOTE_QUEUE_DEPTH + families, "every family past the depth");
    TEST_EXPECT_EQ(ndflt_note_count(p, NOTE_NAME_INTERRUPT), NOTE_QUEUE_DEPTH + 1u,
                   "a move coalesces only at sender 0, and the fill's interrupts name theirs");
    for (u32 i = 0; (name = notes_known_name_for_test(i)) != NULL; i++) {
        if (notes_direct_bit(name) < 0 || dn_same_name(name, NOTE_NAME_INTERRUPT)) continue;
        TEST_EXPECT_EQ(ndflt_note_count(p, name), 1u, "and each is queued once");
    }
    dn_free(p);
}

// aux#253: the SELF arm's twin of the above. Round-2 F4 -- a self `kill` used
// to cascade only when live_threads > 1 and otherwise fall through to
// notes_post, so a Proc whose note ring was FULL took the -EAGAIN and SURVIVED
// ITS OWN SIGKILL. N-4 says kill terminates unconditionally; it may not fail
// for want of ring space.
//
// The arm was untestable when F4 landed because sys_postnote_handler resolves
// its target from current_thread(), which in the harness is the harness's own
// thread. It is reachable now because the arm was extracted to postnote_self
// and given sys_postnote_self_for_test -- the sys_postnote_cross_for_test
// convention: the hook drives the REAL arm, not a second copy of the decision.
void test_notes_self_kill_through_full_ring(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    proc_test_link(p);

    // ONE thread -- the whole point. A Proc that accidentally had peers would
    // take the pre-existing multi-thread cascade and pass for the wrong reason.
    static struct Thread th;
    th.magic             = THREAD_MAGIC;
    th.proc              = p;
    th.note_mask         = 0;
    th.next_in_proc      = NULL;
    th.rendez_blocked_on = NULL;
    p->threads = &th;

    TEST_ASSERT(p->group_exit_msg == NULL,
                "precondition: not already terminating");

    // Fill the ring. `synthetic = false` (the userspace shape, and what the
    // self arm itself posts with) never coalesces -- the coalesce block is
    // gated on `synthetic` -- so the count rises straight to NOTE_QUEUE_DEPTH
    // rather than stalling at NOTE_COALESCE_THRESHOLD the way a synthetic
    // poster of one (name, sender) pair would.
    for (u32 i = 0; i < NOTE_QUEUE_DEPTH; i++)
        (void)notes_post(p, NOTE_NAME_CHILD_EXIT, i, NULL, false);

    // THE CONTROL, and it is the load-bearing part of this test. Every
    // assertion below is of the form "the kill worked", which a ring that
    // never filled satisfies perfectly -- and the full ring IS the entire
    // precondition of the defect. Two independent readings of fullness, so a
    // single broken one cannot fake it: the counter, and a live post refused.
    TEST_EXPECT_EQ((int)p->notes->count, (int)NOTE_QUEUE_DEPTH,
                   "control: the ring reached NOTE_QUEUE_DEPTH");
    TEST_ASSERT(notes_post(p, NOTE_NAME_CHILD_EXIT, 99u, NULL, false) != 0,
                "control: the ring is genuinely FULL -- an ordinary post is "
                "now REFUSED, which is the -EAGAIN the old arm returned");

    // Drive the real self arm.
    s64 rc = sys_postnote_self_for_test(p, NOTE_NAME_KILL);

    // ORDER MATTERS HERE. TEST_ASSERT `return`s on failure, so only the FIRST
    // failing assertion is ever observed -- which makes assertion order decide
    // what a sabotage actually proves. The INVARIANT (N-4: the Proc died) goes
    // first, so the sabotage demonstrates the property; the ABI symptom (the
    // syscall's return value) goes second. Written the other way round, the
    // sabotage reddened on the return value and this assertion never executed,
    // leaving the claim "red pre-fix" untested next to a test that looked
    // thorough.
    TEST_ASSERT(p->group_exit_msg != NULL,
                "aux#253: group_exit_msg SET -- the self kill cascaded THROUGH "
                "a full ring instead of failing for want of a queue slot");
    TEST_EXPECT_EQ((int)rc, 0,
                   "and the syscall reports success -- pre-fix it returned the "
                   "-EAGAIN the full ring gave notes_post");

    proc_test_unlink(p);
    p->threads = NULL;                  // the static outlives proc_free
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// #240: a cont that overtakes a susp must CANCEL the stop, not be overwritten
// by it.
//
// The defect #15 introduced. Pre-#15 the uncaught susp set job_stop_req in the
// same g_proc_table_lock critical section that decided it, so "decide" and
// "apply" were one event. #15 split them: a susp with a handler is POSTED, and
// the stop is applied later, when the handler calls SYS_NOTED(NDFLT). A cont
// arriving in between finds job_stop_req still 0 and -- before this fix --
// returned from proc_job_resume_one_locked having done nothing at all, after
// which the NDFLT applied the very stop the cont was cancelling.
//
// That is not a hypothetical interleaving. It is the pts teardown: pts_free
// destroys the registry entry and then posts tty:hup + conts the foreground
// group. The PTY-1 audit close named that hup-then-cont as THE carrier-loss
// guarantee. With the entry already destroyed, the shell's later `fg` gets
// -T_E_NOENT -- so the job is stopped, the rescue that should have prevented
// it ran and did nothing, and nothing can ever resume it.
//
// Two windows, because the note is posted at one time and consumed at another:
// the cont can land after delivery (leg A -- the documented chain) or before
// it (leg B). Both are the same stale decision and both are asserted, because
// a fix anchored at DELIVERY closes only A while looking complete.
void test_notes_ndflt_stop_discarded_after_cont(void) {
    // The anchored-group shape: `leader` is ALIVE, same session, another
    // group, so pgrp_orphaned_locked is false for m's group. Without it the
    // orphan rule discards every stop below and all three legs -- including
    // the POSITIVE control -- pass for the wrong reason.
    struct Proc *leader = proc_alloc();
    TEST_ASSERT(leader != NULL, "proc_alloc leader");
    proc_test_link(leader);
    struct Proc *m = proc_alloc();
    TEST_ASSERT(m != NULL, "proc_alloc m");
    m->sid  = (u32)leader->pid;
    m->pgid = (u32)m->pid;
    proc_test_link_child(leader, m);

    static struct Thread th;
    static struct exception_context ctx;
    // Unmasked, and load-bearing: proc_tty_susp_would_stop_locked reads a
    // threadless Proc as CAUGHT regardless of handler_va, so the fan would
    // post-not-stop for the wrong reason.
    th.magic             = THREAD_MAGIC;
    th.note_mask         = 0;
    th.next_in_proc      = NULL;
    th.rendez_blocked_on = NULL;
    m->threads = &th;
    m->handler_va = 0x1000u;    // a handler exists -> the fan defers to NDFLT

    // ---- POSITIVE CONTROL: no cont, so the stop MUST still land. --------
    //
    // First, and deliberately so. Every other leg here asserts job_stop_req
    // == 0 -- a negative, which a fixture that never stops anything satisfies
    // perfectly. This leg differs from leg A in exactly one step (the cont)
    // and proves the machinery can reach a stop at all.
    m->susp_stop_armed = 0u;
    TEST_EXPECT_EQ(proc_job_stop_pgrp(m->pgid), 1, "control: the fan visits m");
    TEST_EXPECT_EQ((int)m->job_stop_req, 0, "control: CAUGHT -- deferred");
    TEST_EXPECT_EQ((int)m->susp_stop_armed, 1, "control: the post armed");
    ndflt_arm_handler(&th, m, NOTE_NAME_TTY_SUSP);
    TEST_EXPECT_EQ(notes_noted_default(&ctx, &th), 0, "control: NDFLT succeeds");
    TEST_EXPECT_EQ((int)m->job_stop_req, 1,
                   "CONTROL: with no cont the deferred stop DOES apply");

    // ---- LEG A: cont AFTER delivery (the documented step-3 chain). ------
    m->job_stop_req        = 0;
    m->stop_report_pending = false;
    m->cont_report_pending = false;
    m->susp_stop_armed     = 0u;
    TEST_EXPECT_EQ(proc_job_stop_pgrp(m->pgid), 1, "A: the ^Z fan visits m");
    TEST_EXPECT_EQ((int)m->susp_stop_armed, 1, "A: armed");
    ndflt_arm_handler(&th, m, NOTE_NAME_TTY_SUSP);      // delivery

    // The pts teardown's rescue, on a target that has NOT yet stopped. This is
    // the exact call pts_teardown_fan makes.
    TEST_EXPECT_EQ(proc_job_cont_pgrp(m->pgid), 1, "A: the teardown conts");
    TEST_EXPECT_EQ((int)m->job_stop_req, 0,
                   "A: the cont found nothing stopped -- as it always did");
    TEST_EXPECT_EQ((int)m->susp_stop_armed, 0,
                   "A: but it DISARMED the in-flight stop -- the fix");

    TEST_EXPECT_EQ(notes_noted_default(&ctx, &th), 0, "A: the handler NDFLTs");
    TEST_EXPECT_EQ((int)m->job_stop_req, 0,
                   "A: the STALE stop was DISCARDED -- the job is not stranded");
    TEST_ASSERT(!m->stop_report_pending,
                "A: and no WAIT_UNTRACED report was latched for a non-stop");

    // ---- LEG B: cont BEFORE delivery. -----------------------------------
    //
    // The window an at-delivery snapshot cannot see. A target blocked in a
    // syscall is posted a susp, the carrier drops before it returns to EL0,
    // and only then is the note delivered to the handler. Anchoring the
    // freshness at POST is what covers this; the assertion below is the
    // difference between the two designs.
    m->job_stop_req        = 0;
    m->stop_report_pending = false;
    m->cont_report_pending = false;
    m->susp_stop_armed     = 0u;
    TEST_EXPECT_EQ(proc_job_stop_pgrp(m->pgid), 1, "B: the ^Z fan visits m");
    TEST_EXPECT_EQ((int)m->susp_stop_armed, 1, "B: armed");
    TEST_EXPECT_EQ(proc_job_cont_pgrp(m->pgid), 1, "B: cont, still undelivered");
    TEST_EXPECT_EQ((int)m->susp_stop_armed, 0, "B: disarmed before delivery");
    ndflt_arm_handler(&th, m, NOTE_NAME_TTY_SUSP);      // delivery, afterwards
    TEST_EXPECT_EQ(notes_noted_default(&ctx, &th), 0, "B: the handler NDFLTs");
    TEST_EXPECT_EQ((int)m->job_stop_req, 0,
                   "B: a cont that preceded DELIVERY still cancels the stop");

    // ---- LEG C: a susp AFTER a cont re-arms. ----------------------------
    //
    // The flag must not be a one-way latch. A second ^Z on a resumed job is an
    // ordinary suspend and has to work, or the fix trades a stranded job for a
    // job that can never be suspended twice.
    m->job_stop_req        = 0;
    m->stop_report_pending = false;
    TEST_EXPECT_EQ((int)m->susp_stop_armed, 0, "C: starts disarmed (from B)");
    TEST_EXPECT_EQ(proc_job_stop_pgrp(m->pgid), 1, "C: a second ^Z");
    TEST_EXPECT_EQ((int)m->susp_stop_armed, 1, "C: re-armed");
    ndflt_arm_handler(&th, m, NOTE_NAME_TTY_SUSP);
    TEST_EXPECT_EQ(notes_noted_default(&ctx, &th), 0, "C: NDFLT");
    TEST_EXPECT_EQ((int)m->job_stop_req, 1,
                   "C: the susp-after-cont stops normally -- not a dead latch");

    proc_test_unlink(m);
    m->threads = NULL;                  // the static outlives proc_free
    m->state = PROC_STATE_ZOMBIE;
    proc_free(m);
    proc_test_unlink(leader);
    leader->state = PROC_STATE_ZOMBIE;
    proc_free(leader);
}

// Bounded name compare, local to the two tests below (notes.c's own is static).
static bool susp_name_is(const char *got, const char *want) {
    if (!got) return false;
    for (u32 i = 0; i < NOTE_NAME_MAX; i++) {
        if (got[i] != want[i]) return false;
        if (want[i] == 0) return true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// susp_gate_reads_phenotype_sigtab (round-2 F1 / #251)
// ---------------------------------------------------------------------------
//
// The ^Z catchability gate must read a PHENOTYPED Proc's disposition out of its
// sigtab, not out of handler_va. A Linux guest never calls SYS_NOTIFY, so
// handler_va is 0 for it no matter what it asked for -- and the pre-fix gate
// read that as "nothing is catching this" and STOPPED the guest, both when it
// had installed a SIGTSTP handler and when it had explicitly SIG_IGNed it.
//
// The SIG_IGN leg is the sharper one: notes_post's V-6b discard cannot save it,
// because the uncaught arm stops the Proc WITHOUT ever calling notes_post.
void test_notes_susp_gate_reads_phenotype_sigtab(void) {
    // The anchored-group shape (see ndflt_stop_discarded_after_cont): without
    // an ALIVE same-session parent in ANOTHER group, pgrp_orphaned_locked is
    // true, every stop below is discarded by the orphan rule, and all three
    // negative legs pass for entirely the wrong reason.
    struct Proc *leader = proc_alloc();
    TEST_ASSERT(leader != NULL, "proc_alloc leader");
    proc_test_link(leader);
    struct Proc *m = proc_alloc();
    TEST_ASSERT(m != NULL, "proc_alloc m");
    m->sid  = (u32)leader->pid;
    m->pgid = (u32)m->pid;
    proc_test_link_child(leader, m);

    static struct Thread th;
    th.magic             = THREAD_MAGIC;
    th.note_mask         = 0;           // UNMASKED: the gate must reach the
    th.next_in_proc      = NULL;        // disposition question, not short out
    th.rendez_blocked_on = NULL;        // on "every thread masks the family"
    th.proc              = m;
    m->threads    = &th;
    m->handler_va = 0u;                 // the native axis stays silent

    struct viv_sigtab *tab =
        (struct viv_sigtab *)kzalloc(sizeof(struct viv_sigtab), 0);
    TEST_ASSERT(tab != NULL, "sigtab alloc");
    m->sigtab = tab;

    // ---- POSITIVE CONTROL: phenotype + SIG_DFL still stops. --------------
    //
    // First, deliberately. Every leg below asserts job_stop_req == 0, and a
    // fixture that cannot stop anything satisfies all of them. This leg is one
    // sigtab row away from leg A and proves the fan reaches a stop at all.
    m->phenotype = PHENO_LINUX;
    TEST_ASSERT(!viv_sigtab_note_ignored(tab, VIV_SIGNOTE_TTY_SUSP),
                "control precondition: TTY_SUSP is NOT ignored");
    TEST_EXPECT_EQ(proc_job_stop_pgrp(m->pgid), 1, "control: the fan visits m");
    TEST_EXPECT_EQ((int)m->job_stop_req, 1,
                   "CONTROL: phenotype + SIG_DFL takes the default STOP");
    TEST_EXPECT_EQ(m->notes->count, 0u, "control: the stop consumed it, no note");

    // ---- LEG A: a phenotype HANDLER defers the stop. ---------------------
    m->job_stop_req        = 0;
    m->stop_report_pending = false;
    m->susp_stop_armed     = 0u;
    struct viv_ksigaction hand = { .handler = 0x4000u, .flags = 0,
                                   .restorer = 0, .mask = 0 };
    TEST_ASSERT(viv_sigtab_set(tab, VIV_SIGNOTE_TTY_SUSP, &hand),
                "A precondition: the handler row was WRITTEN");
    struct viv_ksigaction probe;
    TEST_ASSERT(viv_sigtab_note_handler(tab, VIV_SIGNOTE_TTY_SUSP, &probe),
                "A precondition: and reads back AS a handler");
    TEST_EXPECT_EQ(proc_job_stop_pgrp(m->pgid), 1, "A: the fan visits m");
    TEST_EXPECT_EQ((int)m->job_stop_req, 0,
                   "A: a sigtab handler is CAUGHT -- handler_va is 0 and "
                   "the pre-fix gate stopped it anyway");
    TEST_EXPECT_EQ(m->notes->count, 1u,
                   "A: and the note was POSTED, so the handler can run");

    // Drain so the next leg reads a clean queue.
    struct Note got;
    spin_lock(&m->notes->lock);
    notes_dequeue_locked(m, NULL, &got);
    spin_unlock(&m->notes->lock);
    TEST_EXPECT_EQ(m->notes->count, 0u, "queue drained");

    // ---- LEG B: a phenotype SIG_IGN suppresses stop AND note. ------------
    m->job_stop_req        = 0;
    m->stop_report_pending = false;
    m->susp_stop_armed     = 0u;
    struct viv_ksigaction ign = { .handler = VIV_SIG_IGN, .flags = 0,
                                  .restorer = 0, .mask = 0 };
    TEST_ASSERT(viv_sigtab_set(tab, VIV_SIGNOTE_TTY_SUSP, &ign),
                "B precondition: the ignore row was WRITTEN");
    TEST_ASSERT(viv_sigtab_note_ignored(tab, VIV_SIGNOTE_TTY_SUSP),
                "B precondition: and reads back AS ignored");
    TEST_ASSERT(!viv_sigtab_note_handler(tab, VIV_SIGNOTE_TTY_SUSP, &probe),
                "B precondition: SIG_IGN is not a handler -- leg A is not "
                "what is being re-tested here");
    TEST_EXPECT_EQ(proc_job_stop_pgrp(m->pgid), 1, "B: the fan visits m");
    TEST_EXPECT_EQ((int)m->job_stop_req, 0,
                   "B: a Proc that IGNORES ^Z is not stopped by it");
    TEST_EXPECT_EQ(m->notes->count, 0u,
                   "B: and notes_post dropped it -- no slot leaked either");

    // ---- LEG C: NATIVE with the same table stops. ------------------------
    //
    // The phenotype is the gate, not the mere presence of a sigtab. Without
    // this, a fix that consulted the table unconditionally would pass A and B
    // while silently changing native behaviour.
    m->job_stop_req        = 0;
    m->stop_report_pending = false;
    m->susp_stop_armed     = 0u;
    m->phenotype = PHENO_NATIVE;
    TEST_ASSERT(viv_sigtab_note_ignored(tab, VIV_SIGNOTE_TTY_SUSP),
                "C precondition: the ignore row is STILL set");
    TEST_EXPECT_EQ(proc_job_stop_pgrp(m->pgid), 1, "C: the fan visits m");
    TEST_EXPECT_EQ((int)m->job_stop_req, 1,
                   "C: a NATIVE Proc ignores the table and takes the STOP");

    proc_test_unlink(m);
    m->threads = NULL;                  // the static outlives proc_free
    m->state = PROC_STATE_ZOMBIE;
    proc_free(m);                       // frees the sigtab too
    proc_test_unlink(leader);
    leader->state = PROC_STATE_ZOMBIE;
    proc_free(leader);
}

// ---------------------------------------------------------------------------
// masked_susp_stops_at_delivery (round-2 F2 / #252)
// ---------------------------------------------------------------------------
//
// When every thread MASKS the tty family, the ^Z fan posts the note instead of
// stopping -- POSIX's "a blocked stop signal becomes pending". Before the fix
// nothing could ever consume that note: the EL0 tail's case analysis only
// handled the terminate class, so the note sat in the ring for the life of the
// Proc and the ^Z was silently lost.
//
// The decision is driven directly rather than through notes_deliver_at_el0_-
// return, which takes no Thread argument (it reads current_thread()). That is
// the same seam notes_terminate_note_name_locked already exists for.
//
// WHAT THIS DOES NOT COVER, stated so the green does not read as more than it
// is: the three lines of WIRING in the EL0 tail -- that it calls this decider
// at all, dequeues on non-NULL, and drops q->lock before proc_job_stop_self.
// Deleting that block leaves every assertion below passing. The terminate
// twin has the identical hole for the identical reason (a unit test cannot
// install a current_thread).
//
// This used to name "the in-guest ^Z E2E that task #238 already tracks" as the
// closer, and that was WRONG -- doubly so, which is why it is worth the
// correction rather than a quiet edit. An ordinary ^Z, from any E2E, cannot
// reach this arm at all: proc_job_stop_pgrp applies the stop at POST time, so
// the note never becomes queued-and-deliverable. #238 landed, passed, and
// covered nothing here. The arm's precondition is a state that must be
// CONSTRUCTED: every thread masks NOTE_BIT_TTY, the fan posts instead of
// stopping, and the mask then lifts.
//
// The closer is jc-probe's `maskstop` rung + /susp-mask-child (boot-fatal, so
// it rides every boot): mask -> absorb a ^Z -> unmask -> the deferred stop
// lands on the EL0 return from the unmask syscall itself. That is also the leg
// that proves the lock order under real contention.
void test_notes_masked_susp_stops_at_delivery(void) {
    struct Proc *leader = proc_alloc();
    struct Proc *m = (leader != NULL) ? proc_alloc() : NULL;
    if (leader == NULL || m == NULL) {
        if (m) proc_free(m);
        if (leader) proc_free(leader);
        TEST_ASSERT(false, "proc_alloc leader + m");
        return;
    }
    proc_test_link(leader);
    m->sid  = (u32)leader->pid;
    m->pgid = (u32)m->pid;
    proc_test_link_child(leader, m);

    static struct Thread th;
    th.magic             = THREAD_MAGIC;
    th.next_in_proc      = NULL;
    th.rendez_blocked_on = NULL;
    th.proc              = m;
    th.note_mask         = 0;
    m->threads    = &th;
    m->handler_va = 0u;

    // ---- OBSERVE FIRST, ASSERT LAST -- the idiom stop_dequeue_picks_its_own_
    // note states below and this test did not follow until now: TEST_ASSERT
    // *returns*, so any assertion placed before the unlink leaves `leader` and
    // `m` alive and linked, and the next test's `while (wait_pid(&st) > 0)`
    // hangs the boot. Every observation is captured; every assertion waits
    // for the teardown. Name-comparisons are guarded on the observation that
    // produced them so a NULL peek reads as a false leg, not a fault.

    // ---- CONTROL: unmasked, the fan stops immediately. ----------------
    int  ctl_visits = proc_job_stop_pgrp(m->pgid);
    int  ctl_stop   = (int)m->job_stop_req;
    u32  ctl_count  = m->notes->count;

    // ---- LEG A: masked -- the fan POSTS, and the stop is pending. --------
    m->job_stop_req        = 0;
    m->stop_report_pending = false;
    m->susp_stop_armed     = 0u;
    th.note_mask = (1u << NOTE_BIT_TTY);
    int  a_visits = proc_job_stop_pgrp(m->pgid);
    int  a_stop   = (int)m->job_stop_req;
    u32  a_count  = m->notes->count;
    int  a_armed  = (int)m->susp_stop_armed;

    spin_lock(&m->notes->lock);
    const char *pending_masked = notes_stop_note_name_locked(m, &th);
    spin_unlock(&m->notes->lock);

    // ---- LEG B: unmask -- the pending stop becomes deliverable. ----------
    th.note_mask = 0;
    spin_lock(&m->notes->lock);
    const char *pending_open = notes_stop_note_name_locked(m, &th);
    spin_unlock(&m->notes->lock);
    bool b_open_is_susp = (pending_open != NULL) &&
                          susp_name_is(pending_open, NOTE_NAME_TTY_SUSP);

    // The tail's action on that decision: consume, then apply via the shared
    // primitive. Observed separately so a fix that answered right and acted
    // wrong is still caught. It calls notes_stop_dequeue_locked because that is
    // what the tail calls -- this leg used to call the general
    // notes_dequeue_locked, mirroring a tail that did the same, and on a
    // ONE-NOTE queue the two are indistinguishable. See
    // notes.stop_dequeue_picks_its_own_note for the queue that tells them
    // apart.
    struct Note consumed = { 0 };
    spin_lock(&m->notes->lock);
    int  b_got = notes_stop_dequeue_locked(m, &th, &consumed);
    spin_unlock(&m->notes->lock);
    bool b_consumed_is_susp = (b_got == 1) &&
                              susp_name_is(consumed.name, NOTE_NAME_TTY_SUSP);
    u32  b_count   = m->notes->count;
    bool b_applied = proc_job_stop_self(m);
    int  b_stop    = (int)m->job_stop_req;

    // ---- LEG C: a cont while masked cancels it (#240 composition). -------
    //
    // The deferred path must inherit the freshness rule, or it re-opens the
    // window #240 closed -- with a longer fuse, since the note can sit masked
    // indefinitely.
    m->job_stop_req        = 0;
    m->stop_report_pending = false;
    m->cont_report_pending = false;
    m->susp_stop_armed     = 0u;
    th.note_mask = (1u << NOTE_BIT_TTY);
    int  c_visits     = proc_job_stop_pgrp(m->pgid);
    int  c_armed      = (int)m->susp_stop_armed;
    int  c_cont       = proc_job_cont_pgrp(m->pgid);
    int  c_disarmed   = (int)m->susp_stop_armed;
    th.note_mask = 0;
    bool c_applied    = proc_job_stop_self(m);
    int  c_stop       = (int)m->job_stop_req;

    // ---- LEG D: a self-managing Proc keeps its note. ---------------------
    //
    // It has an fd reader; consuming the note here would steal it.
    spin_lock(&m->notes->lock);
    while (m->notes->count > 0) {
        struct Note drain;
        notes_dequeue_locked(m, NULL, &drain);
    }
    spin_unlock(&m->notes->lock);
    u32  d_clean = m->notes->count;
    notes_mark_self_managing(m);
    int  d_post  = notes_post(m, NOTE_NAME_TTY_SUSP, 0u, NULL, true);
    u32  d_count = m->notes->count;
    spin_lock(&m->notes->lock);
    const char *self_managed = notes_stop_note_name_locked(m, &th);
    spin_unlock(&m->notes->lock);

    // The same refusal from the CONSUMER, which is what the tail actually
    // calls. Observing only the predicate would leave the exemption verified
    // on a function production no longer reaches -- the exact gap leg B had.
    struct Note must_not_take = { 0 };
    spin_lock(&m->notes->lock);
    int  d_took      = notes_stop_dequeue_locked(m, &th, &must_not_take);
    spin_unlock(&m->notes->lock);
    u32  d_remaining = m->notes->count;

    // ---- TEARDOWN, unconditionally, before the first assertion. ---------
    proc_test_unlink(m);
    m->threads = NULL;                  // the static outlives proc_free
    m->state = PROC_STATE_ZOMBIE;
    proc_free(m);
    proc_test_unlink(leader);
    leader->state = PROC_STATE_ZOMBIE;
    proc_free(leader);

    // ---- CONTROL ---------------------------------------------------------
    TEST_EXPECT_EQ(ctl_visits, 1, "control: the fan visits m");
    TEST_EXPECT_EQ(ctl_stop, 1, "CONTROL: unmasked + uncaught stops at POST time");
    TEST_EXPECT_EQ(ctl_count, 0u, "control: consumed by the stop");

    // ---- LEG A -----------------------------------------------------------
    TEST_EXPECT_EQ(a_visits, 1, "A: the fan visits m");
    TEST_EXPECT_EQ(a_stop, 0, "A: all-masked defers the stop");
    TEST_EXPECT_EQ(a_count, 1u, "A: it was posted instead");
    TEST_EXPECT_EQ(a_armed, 1, "A: and the post armed #240");
    TEST_ASSERT(pending_masked == NULL,
                "A: still MASKED, so not yet deliverable -- a stop applied "
                "here would fire while the guest has it blocked");

    // ---- LEG B -----------------------------------------------------------
    TEST_ASSERT(pending_open != NULL,
                "B: unmasking makes the pending ^Z deliverable -- pre-fix "
                "this was NULL forever and the note leaked its slot");
    TEST_ASSERT(b_open_is_susp,
                "B: and it is the susp, not some other queued note");
    TEST_EXPECT_EQ(b_got, 1, "B: the note dequeues");
    TEST_ASSERT(b_consumed_is_susp,
                "B: and the note CONSUMED is the one the peek named");
    TEST_EXPECT_EQ(b_count, 0u, "B: the slot is RECLAIMED");
    TEST_ASSERT(b_applied, "B: and the deferred stop applies");
    TEST_EXPECT_EQ(b_stop, 1, "B: the guest is stopped");

    // ---- LEG C -----------------------------------------------------------
    TEST_EXPECT_EQ(c_visits, 1, "C: a masked ^Z");
    TEST_EXPECT_EQ(c_armed, 1, "C: armed");
    TEST_EXPECT_EQ(c_cont, 1, "C: a cont arrives");
    TEST_EXPECT_EQ(c_disarmed, 0, "C: which disarmed it");
    TEST_ASSERT(!c_applied,
                "C: the deferred stop is DISCARDED -- superseded by the cont");
    TEST_EXPECT_EQ(c_stop, 0, "C: the job is not stranded");

    // ---- LEG D -----------------------------------------------------------
    TEST_EXPECT_EQ(d_clean, 0u, "D: queue clean before the leg");
    TEST_EXPECT_EQ(d_post, 0, "D: a susp is posted to a self-managing Proc");
    TEST_EXPECT_EQ(d_count, 1u, "D precondition: it IS queued");
    TEST_ASSERT(self_managed == NULL,
                "D: the tail does NOT consume it -- devnotes_read owns it");
    TEST_EXPECT_EQ(d_took, 0, "D: and the CONSUMER refuses it too");
    TEST_EXPECT_EQ(d_remaining, 1u, "D: the note is still there for the fd");
}

// ---------------------------------------------------------------------------
// stop_dequeue_picks_its_own_note
// ---------------------------------------------------------------------------
//
// The EL0 tail's stop arm DECIDES with a class-filtered scan
// (notes_stop_note_name_locked: first deliverable STOP-class note, at any
// index) and used to CONSUME with the general notes_dequeue_locked, which pops
// the first mask-permitted entry in FIFO order regardless of class. On a
// one-note queue those two agree, and every queue any prior test built held
// exactly one note -- so the disagreement had nowhere to show.
//
// Put something in front of the susp and they diverge: the stop applies, the
// child_exit is popped into a stack local nobody reads (destroyed, not
// delivered -- a wait notification silently gone), and the tty:susp stays
// queued to re-fire at the next EL0 return.
//
// Leg A EXECUTES the old behaviour rather than describing it. That is what
// keeps this test honest: if the two selection rules ever converge again, leg
// A fails and says so, instead of leg B quietly becoming a tautology that
// passes for the wrong reason.
void test_notes_stop_dequeue_picks_its_own_note(void) {
    struct Proc *leader = proc_alloc();
    struct Proc *m = (leader != NULL) ? proc_alloc() : NULL;
    if (leader == NULL || m == NULL) {
        if (m) proc_free(m);
        if (leader) proc_free(leader);
        TEST_ASSERT(false, "proc_alloc leader + m");
        return;
    }
    proc_test_link(leader);
    m->sid  = (u32)leader->pid;
    m->pgid = (u32)m->pid;
    proc_test_link_child(leader, m);

    static struct Thread th2;
    th2.magic             = THREAD_MAGIC;
    th2.next_in_proc      = NULL;
    th2.rendez_blocked_on = NULL;
    th2.proc              = m;
    th2.note_mask         = 0;
    m->threads    = &th2;
    m->handler_va = 0u;

    // ---- OBSERVE FIRST, ASSERT LAST. ------------------------------------
    //
    // Every observation below is captured into a local and every assertion is
    // deferred past the teardown, because TEST_ASSERT *returns* on failure --
    // so an assertion placed before the unlink leaves `leader` and `m` ALIVE
    // and linked in the proc table. That is not hypothetical: it was measured.
    // Under the sabotage that reddens leg B, this test returned early, and the
    // next test to run `while (wait_pid(&st) > 0)` (cons.sys_puts_uses_shared_
    // console_path) waited forever on a child that could never exit -- the boot
    // hung at 99% CPU with no further output. A sabotage that HANGS the harness
    // downstream is a different experiment, not a stronger one: it destroys the
    // clean per-assertion verdict the control exists to produce.
    //
    // This is the idiom test_cons.c:2198 already states for the same reason
    // ("Sample + DISARM before asserting ... one broken test reporting as
    // three"); this file did not follow it.
    int  a_post_child = notes_post(m, NOTE_NAME_CHILD_EXIT, 42u, NULL, true);
    int  a_post_susp  = notes_post(m, NOTE_NAME_TTY_SUSP, 0u, NULL, true);
    u32  a_count      = m->notes->count;

    spin_lock(&m->notes->lock);
    const char *decided = notes_stop_note_name_locked(m, &th2);
    spin_unlock(&m->notes->lock);
    bool a_decided_susp = (decided != NULL) &&
                          susp_name_is(decided, NOTE_NAME_TTY_SUSP);

    struct Note blind = { 0 };
    spin_lock(&m->notes->lock);
    int blind_got = notes_dequeue_locked(m, &th2, &blind);
    spin_unlock(&m->notes->lock);
    bool a_blind_is_child = (blind_got == 1) &&
                            susp_name_is(blind.name, NOTE_NAME_CHILD_EXIT);

    spin_lock(&m->notes->lock);
    while (m->notes->count > 0) {
        struct Note drain;
        notes_dequeue_locked(m, NULL, &drain);
    }
    spin_unlock(&m->notes->lock);
    u32 b_reset_count = m->notes->count;

    int b_post_child = notes_post(m, NOTE_NAME_CHILD_EXIT, 42u, NULL, true);
    int b_post_susp  = notes_post(m, NOTE_NAME_TTY_SUSP, 0u, NULL, true);
    u32 b_count      = m->notes->count;

    struct Note picked = { 0 };
    spin_lock(&m->notes->lock);
    int picked_got = notes_stop_dequeue_locked(m, &th2, &picked);
    spin_unlock(&m->notes->lock);
    bool b_picked_susp = (picked_got == 1) &&
                         susp_name_is(picked.name, NOTE_NAME_TTY_SUSP);
    u32 b_remaining = m->notes->count;

    struct Note survivor = { 0 };
    spin_lock(&m->notes->lock);
    int surv_got = notes_dequeue_locked(m, &th2, &survivor);
    spin_unlock(&m->notes->lock);
    bool b_surv_is_child = (surv_got == 1) &&
                           susp_name_is(survivor.name, NOTE_NAME_CHILD_EXIT);

    // ---- TEARDOWN, unconditionally, before the first assertion. ---------
    proc_test_unlink(m);
    m->threads = NULL;                  // the static outlives proc_free
    m->state = PROC_STATE_ZOMBIE;
    proc_free(m);
    proc_test_unlink(leader);
    leader->state = PROC_STATE_ZOMBIE;
    proc_free(leader);

    // ---- LEG A: the CONTROL -- the class-blind pop takes the wrong note. --
    TEST_EXPECT_EQ(a_post_child, 0, "A: a child_exit is queued FIRST");
    TEST_EXPECT_EQ(a_post_susp, 0, "A: then a deferred ^Z lands behind it");
    TEST_EXPECT_EQ(a_count, 2u, "A precondition: TWO notes queued");
    TEST_ASSERT(a_decided_susp,
                "A: the DECISION is about the susp, which is at index 1");
    TEST_EXPECT_EQ(blind_got, 1, "A: the class-blind pop returns a note");
    TEST_ASSERT(a_blind_is_child,
                "A: and it is the CHILD_EXIT -- the head, not the note the "
                "stop decision named. This is the defect, executed.");

    // ---- LEG B: the fix -- the same state, the tail's real consumer. ------
    TEST_EXPECT_EQ(b_reset_count, 0u, "B: queue reset between the legs");
    TEST_EXPECT_EQ(b_post_child, 0, "B: child_exit first again");
    TEST_EXPECT_EQ(b_post_susp, 0, "B: susp behind it again");
    TEST_EXPECT_EQ(b_count, 2u, "B precondition: TWO notes queued");
    TEST_EXPECT_EQ(picked_got, 1, "B: the stop consumer returns a note");
    TEST_ASSERT(b_picked_susp,
                "B: and it is the SUSP -- the note the decision named");

    // Both halves of the conservation claim. Count alone would pass if the
    // consumer had taken the child_exit and left the susp.
    TEST_EXPECT_EQ(b_remaining, 1u, "B: exactly one note remains");
    TEST_EXPECT_EQ(surv_got, 1, "B: the survivor dequeues");
    TEST_ASSERT(b_surv_is_child,
                "B: the child_exit SURVIVED -- it was never the stop's to eat");
}

// ---------------------------------------------------------------------------
// class_scans_read_phenotype_sigtab (the c8ab2744 round, F1)
// ---------------------------------------------------------------------------
//
// The two class scans behind the EL0 tail's uncaught arms -- the TERMINATE
// scan (notes_terminate_note_name_locked) and the STOP scan behind
// notes_stop_dequeue_locked -- answer "is a note of this class deliverable to
// this thread?" at ANY index. The class is a table fact; whether the DEFAULT
// applies is a per-Proc, per-note fact, and for a phenotyped Proc it lives in
// the sigtab, not in handler_va (a Linux guest never calls SYS_NOTIFY). The
// terminate scan gated on handler_va only, so on PHENO_LINUX a CAUGHT tty:hup
// or interrupt queued behind a SIG_DFL candidate was returned, and the tail
// exits()ed a guest with its handler installed. #251 put the per-Proc
// disposition on the post-time decider and on the stop predicates, and not
// here -- the fix that exists on site N stops you asking about site N+1.
//
// The scans are driven directly (they take a Thread; the dispatcher reads
// current_thread()). No Proc is linked and nothing is stopped, so the fixture
// is the unlinked one linux_sigign_discard uses. Observations first, one
// teardown, assertions last.
void test_notes_class_scans_read_phenotype_sigtab(void) {
    struct Proc *m = proc_alloc();
    struct viv_sigtab *tab = (m != NULL)
        ? (struct viv_sigtab *)kzalloc(sizeof(struct viv_sigtab), 0) : NULL;
    if (m == NULL || tab == NULL) {
        if (m) { m->state = PROC_STATE_ZOMBIE; proc_free(m); }
        TEST_ASSERT(false, "proc_alloc m + sigtab alloc");
        return;
    }
    m->sigtab     = tab;                // freed by proc_free
    m->handler_va = 0u;                 // the native axis stays silent
    m->phenotype  = PHENO_LINUX;

    static struct Thread th;
    th.magic             = THREAD_MAGIC;
    th.next_in_proc      = NULL;
    th.rendez_blocked_on = NULL;
    th.proc              = m;
    th.note_mask         = 0;           // UNMASKED: every leg reaches the
                                        // disposition question
    const struct viv_ksigaction dfl  = { .handler = VIV_SIG_DFL, .flags = 0,
                                         .restorer = 0, .mask = 0 };
    const struct viv_ksigaction hand = { .handler = 0x4000u, .flags = 0,
                                         .restorer = 0, .mask = 0 };
    const struct viv_ksigaction ign  = { .handler = VIV_SIG_IGN, .flags = 0,
                                         .restorer = 0, .mask = 0 };

    // ---- POSITIVE CONTROL: phenotype + all-SIG_DFL, [child_exit, tty:hup]
    // -> the scan names the hup, at index 1. Every negative leg below expects
    // NULL, and a scan that always answered NULL would satisfy all of them;
    // this is one sigtab row away from leg B and proves the scan reaches a
    // terminate decision at all, past a leading note of another class.
    int  ctl_post_child = notes_post(m, NOTE_NAME_CHILD_EXIT, 1u, NULL, true);
    int  ctl_post_hup   = notes_post(m, NOTE_NAME_TTY_HUP, 0u, NULL, true);
    u32  ctl_count      = m->notes->count;
    spin_lock(&m->notes->lock);
    const char *ctl_name = notes_terminate_note_name_locked(m, &th);
    spin_unlock(&m->notes->lock);
    bool ctl_is_hup = susp_name_is(ctl_name, NOTE_NAME_TTY_HUP);
    spin_lock(&m->notes->lock);
    while (m->notes->count > 0) {
        struct Note drain;
        notes_dequeue_locked(m, NULL, &drain);
    }
    spin_unlock(&m->notes->lock);

    // ---- LEG A: [tty:susp SIG_DFL, interrupt HANDLER] -- the deferred-^Z
    // shape. The terminate scan must NOT name the caught interrupt, and the
    // stop consumer must take the susp (index 0) and leave the interrupt for
    // the phenotype delivery path. Pre-fix: the guest died of "interrupt"
    // with a SIGINT handler installed.
    bool a_set        = viv_sigtab_set(tab, VIV_SIGNOTE_INTERRUPT, &hand);
    int  a_post_susp  = notes_post(m, NOTE_NAME_TTY_SUSP, 0u, NULL, true);
    int  a_post_int   = notes_post(m, NOTE_NAME_INTERRUPT, 0u, NULL, true);
    u32  a_count      = m->notes->count;
    spin_lock(&m->notes->lock);
    const char *a_name = notes_terminate_note_name_locked(m, &th);
    spin_unlock(&m->notes->lock);
    struct Note a_taken = { 0 };
    spin_lock(&m->notes->lock);
    int  a_got = notes_stop_dequeue_locked(m, &th, &a_taken);
    spin_unlock(&m->notes->lock);
    bool a_took_susp = (a_got == 1) &&
                       susp_name_is(a_taken.name, NOTE_NAME_TTY_SUSP);
    struct Note a_surv = { 0 };
    spin_lock(&m->notes->lock);
    int  a_surv_got = notes_dequeue_locked(m, NULL, &a_surv);
    spin_unlock(&m->notes->lock);
    bool a_surv_is_int = (a_surv_got == 1) &&
                         susp_name_is(a_surv.name, NOTE_NAME_INTERRUPT);
    u32  a_left = m->notes->count;

    // ---- LEG B: the CONTROL's queue with ONE row changed -- SIGHUP caught.
    bool b_set        = viv_sigtab_set(tab, VIV_SIGNOTE_TTY_HUP, &hand);
    int  b_post_child = notes_post(m, NOTE_NAME_CHILD_EXIT, 1u, NULL, true);
    int  b_post_hup   = notes_post(m, NOTE_NAME_TTY_HUP, 0u, NULL, true);
    u32  b_count      = m->notes->count;
    spin_lock(&m->notes->lock);
    const char *b_name = notes_terminate_note_name_locked(m, &th);
    spin_unlock(&m->notes->lock);
    spin_lock(&m->notes->lock);
    while (m->notes->count > 0) {
        struct Note drain;
        notes_dequeue_locked(m, NULL, &drain);
    }
    spin_unlock(&m->notes->lock);
    bool b_reset = viv_sigtab_set(tab, VIV_SIGNOTE_TTY_HUP, &dfl);

    // ---- LEG C: an interrupt queued under SIG_DFL, then SIG_IGN installed.
    // notes_post's V-6b discard cannot see it (it ran before the install);
    // Linux discards a pending signal when its disposition becomes SIG_IGN,
    // and the scan must not terminate the guest on a signal it now ignores.
    bool c_dfl        = viv_sigtab_set(tab, VIV_SIGNOTE_INTERRUPT, &dfl);
    int  c_post_child = notes_post(m, NOTE_NAME_CHILD_EXIT, 1u, NULL, true);
    int  c_post_int   = notes_post(m, NOTE_NAME_INTERRUPT, 0u, NULL, true);
    u32  c_count      = m->notes->count;
    spin_lock(&m->notes->lock);
    const char *c_name_before = notes_terminate_note_name_locked(m, &th);
    spin_unlock(&m->notes->lock);
    bool c_before_is_int = susp_name_is(c_name_before, NOTE_NAME_INTERRUPT);
    bool c_ign        = viv_sigtab_set(tab, VIV_SIGNOTE_INTERRUPT, &ign);
    spin_lock(&m->notes->lock);
    const char *c_name_after = notes_terminate_note_name_locked(m, &th);
    spin_unlock(&m->notes->lock);
    spin_lock(&m->notes->lock);
    while (m->notes->count > 0) {
        struct Note drain;
        notes_dequeue_locked(m, NULL, &drain);
    }
    spin_unlock(&m->notes->lock);

    // ---- LEG D: the STOP scan's per-note gate. [tty:susp] with a SIGTSTP
    // handler: the predicate and the consumer both decline (the phenotype
    // delivery path owns it); flip the row to SIG_DFL and the consumer takes
    // it. Before this round the gate was a fixed-name test outside the scan;
    // it is now per note inside it, and this leg is what a future edit that
    // drops it reddens.
    bool d_hand       = viv_sigtab_set(tab, VIV_SIGNOTE_TTY_SUSP, &hand);
    int  d_post       = notes_post(m, NOTE_NAME_TTY_SUSP, 0u, NULL, true);
    u32  d_count      = m->notes->count;
    spin_lock(&m->notes->lock);
    const char *d_name = notes_stop_note_name_locked(m, &th);
    spin_unlock(&m->notes->lock);
    struct Note d_refused = { 0 };
    spin_lock(&m->notes->lock);
    int  d_took = notes_stop_dequeue_locked(m, &th, &d_refused);
    spin_unlock(&m->notes->lock);
    u32  d_kept = m->notes->count;
    bool d_dfl        = viv_sigtab_set(tab, VIV_SIGNOTE_TTY_SUSP, &dfl);
    struct Note d_taken = { 0 };
    spin_lock(&m->notes->lock);
    int  d_took_dfl = notes_stop_dequeue_locked(m, &th, &d_taken);
    spin_unlock(&m->notes->lock);
    bool d_taken_susp = (d_took_dfl == 1) &&
                        susp_name_is(d_taken.name, NOTE_NAME_TTY_SUSP);
    u32  d_left = m->notes->count;

    // ---- LEG E: NATIVE with the SAME table (SIGINT still SIG_IGN, SIGHUP
    // handler re-armed) names the interrupt. The phenotype is the gate, not
    // the presence of a table: a fix that consulted the table unconditionally
    // would pass A-D while changing native behaviour.
    bool e_hand       = viv_sigtab_set(tab, VIV_SIGNOTE_TTY_HUP, &hand);
    m->phenotype = PHENO_NATIVE;
    int  e_post_child = notes_post(m, NOTE_NAME_CHILD_EXIT, 1u, NULL, true);
    int  e_post_int   = notes_post(m, NOTE_NAME_INTERRUPT, 0u, NULL, true);
    u32  e_count      = m->notes->count;
    spin_lock(&m->notes->lock);
    const char *e_name = notes_terminate_note_name_locked(m, &th);
    spin_unlock(&m->notes->lock);
    bool e_is_int = susp_name_is(e_name, NOTE_NAME_INTERRUPT);
    bool e_ign_still = viv_sigtab_note_ignored(tab, VIV_SIGNOTE_INTERRUPT);

    // ---- TEARDOWN, unconditionally, before the first assertion. ---------
    m->threads = NULL;
    m->state = PROC_STATE_ZOMBIE;
    proc_free(m);                       // frees the sigtab too

    // ---- CONTROL ---------------------------------------------------------
    TEST_EXPECT_EQ(ctl_post_child, 0, "control: child_exit queued first");
    TEST_EXPECT_EQ(ctl_post_hup, 0, "control: tty:hup queued behind it");
    TEST_EXPECT_EQ(ctl_count, 2u, "control precondition: TWO notes queued");
    TEST_ASSERT(ctl_name != NULL,
                "CONTROL: phenotype + SIG_DFL -- the scan reaches a terminate "
                "decision (a scan that always said NULL would pass every "
                "leg below)");
    TEST_ASSERT(ctl_is_hup, "control: and it names the hup, at index 1");

    // ---- LEG A -----------------------------------------------------------
    TEST_ASSERT(a_set, "A precondition: the SIGINT handler row was WRITTEN");
    TEST_EXPECT_EQ(a_post_susp, 0, "A: a deferred susp is queued first");
    TEST_EXPECT_EQ(a_post_int, 0, "A: a caught interrupt lands behind it");
    TEST_EXPECT_EQ(a_count, 2u, "A precondition: TWO notes queued");
    TEST_ASSERT(a_name == NULL,
                "A: the terminate scan does NOT name the CAUGHT interrupt -- "
                "pre-fix the guest died of it with its SIGINT handler "
                "installed");
    TEST_EXPECT_EQ(a_got, 1, "A: the stop consumer returns a note");
    TEST_ASSERT(a_took_susp, "A: and it is the SUSP");
    TEST_EXPECT_EQ(a_surv_got, 1, "A: the survivor dequeues");
    TEST_ASSERT(a_surv_is_int,
                "A: the caught interrupt SURVIVED for the phenotype delivery");
    TEST_EXPECT_EQ(a_left, 0u, "A: nothing else was created or destroyed");

    // ---- LEG B -----------------------------------------------------------
    TEST_ASSERT(b_set, "B precondition: the SIGHUP handler row was WRITTEN");
    TEST_EXPECT_EQ(b_post_child, 0, "B: the control's child_exit again");
    TEST_EXPECT_EQ(b_post_hup, 0, "B: the control's tty:hup again");
    TEST_EXPECT_EQ(b_count, 2u, "B precondition: TWO notes queued");
    TEST_ASSERT(b_name == NULL,
                "B: ONE row changed from the control -- SIGHUP caught -- and "
                "the scan no longer names it");
    TEST_ASSERT(b_reset, "B: the row was reset for the legs after");

    // ---- LEG C -----------------------------------------------------------
    TEST_ASSERT(c_dfl, "C precondition: SIGINT back to SIG_DFL");
    TEST_EXPECT_EQ(c_post_child, 0, "C: child_exit first");
    TEST_EXPECT_EQ(c_post_int, 0, "C: an interrupt queued under SIG_DFL");
    TEST_EXPECT_EQ(c_count, 2u, "C precondition: TWO notes queued");
    TEST_ASSERT(c_before_is_int,
                "C control: while SIG_DFL, the scan names the interrupt");
    TEST_ASSERT(c_ign, "C precondition: SIG_IGN installed AFTER the post");
    TEST_ASSERT(c_name_after == NULL,
                "C: a pending interrupt the guest now IGNORES is not a reason "
                "to terminate it");

    // ---- LEG D -----------------------------------------------------------
    TEST_ASSERT(d_hand, "D precondition: the SIGTSTP handler row was WRITTEN");
    TEST_EXPECT_EQ(d_post, 0, "D: a susp is queued");
    TEST_EXPECT_EQ(d_count, 1u, "D precondition: it IS queued");
    TEST_ASSERT(d_name == NULL, "D: the stop PREDICATE declines a caught susp");
    TEST_EXPECT_EQ(d_took, 0, "D: and the CONSUMER declines it too");
    TEST_EXPECT_EQ(d_kept, 1u, "D: the note is kept for the phenotype delivery");
    TEST_ASSERT(d_dfl, "D precondition: SIGTSTP back to SIG_DFL");
    TEST_EXPECT_EQ(d_took_dfl, 1, "D: now the consumer takes it");
    TEST_ASSERT(d_taken_susp, "D: and it is the susp");
    TEST_EXPECT_EQ(d_left, 0u, "D: consumed");

    // ---- LEG E -----------------------------------------------------------
    TEST_ASSERT(e_hand, "E precondition: the SIGHUP handler row is set again");
    TEST_ASSERT(e_ign_still, "E precondition: SIGINT is STILL SIG_IGN in the table");
    TEST_EXPECT_EQ(e_post_child, 0, "E: child_exit first");
    TEST_EXPECT_EQ(e_post_int, 0, "E: interrupt behind it");
    TEST_EXPECT_EQ(e_count, 2u, "E precondition: TWO notes queued");
    TEST_ASSERT(e_is_int,
                "E: a NATIVE Proc ignores the table and the scan names the "
                "interrupt -- the phenotype is the gate");
}

// -----------------------------------------------------------------------------
// notes.phenotype_sigreturn_restores_mask -- the mask half of a phenotype
// sigreturn (Linux restores uc_sigmask; here the kernel-side note_saved_mask
// the Linux delivery path wrote beside the registers), and its NATIVE control:
// a native noted leaves the mask exactly as the handler left it (the as-built
// rule; the field is not written for it). Both legs use the SAME thread
// contents, so an unconditional restore fails the control and a missing
// restore fails the phenotype leg. vivarium.handler_mask covers the value the
// delivery path stores; the probe's L237-L244 cover the wiring end to end.
// -----------------------------------------------------------------------------
void test_notes_phenotype_sigreturn_restores_mask(void) {
    struct Proc *m = proc_alloc();
    if (m == NULL) {
        TEST_ASSERT(false, "proc_alloc m");
        return;
    }
    static struct Thread th;            // BSS-zeroed
    static struct exception_context ctx;
    const u64 PRE  = 0x08u;             // the pre-handler mask (child_exit)
    const u64 HAND = 0x2Du;             // handler-time: pre | sa_mask | sig | own

    // ---- LEG A: PHENO_LINUX -- the restore puts PRE back. ---------------
    m->phenotype = PHENO_LINUX;
    th.magic             = THREAD_MAGIC;
    th.proc              = m;
    th.next_in_proc      = NULL;
    th.rendez_blocked_on = NULL;
    th.in_handler        = true;
    for (u32 i = 0; i < 31; i++) th.note_saved_regs[i] = 0xA000u + i;
    th.note_saved_sp_el0 = 0xB000u;
    th.note_saved_elr    = 0xC000u;
    th.note_saved_spsr   = 0u;
    th.note_saved_mask   = PRE;
    th.note_mask         = HAND;
    int  a_rc      = notes_noted_restore(&ctx, &th);
    u64  a_mask    = th.note_mask;
    bool a_left    = !th.in_handler;
    u64  a_elr     = ctx.elr;

    // ---- LEG B: NATIVE, same contents -- the mask is left alone. -------
    m->phenotype = PHENO_NATIVE;
    th.in_handler      = true;
    th.note_saved_mask = PRE;
    th.note_mask       = HAND;
    int  b_rc   = notes_noted_restore(&ctx, &th);
    u64  b_mask = th.note_mask;
    bool b_left = !th.in_handler;

    // ---- LEG C: not in a handler -> refused, nothing touched. ------------
    th.note_mask = HAND;
    int  c_rc   = notes_noted_restore(&ctx, &th);
    u64  c_mask = th.note_mask;

    // ---- TEARDOWN before the first assertion. --------------------------
    th.proc  = NULL;
    m->state = PROC_STATE_ZOMBIE;
    proc_free(m);

    TEST_EXPECT_EQ(a_rc, 0, "A: the phenotype restore succeeds");
    TEST_ASSERT(a_left, "A: the handler is left");
    TEST_EXPECT_EQ((int)(a_elr - 0xC000u), 0, "A: the pc is restored (the register half still runs)");
    TEST_ASSERT(a_mask == PRE, "A: PHENO_LINUX sigreturn restores the PRE-handler mask");
    TEST_EXPECT_EQ(b_rc, 0, "B: the native restore succeeds");
    TEST_ASSERT(b_left, "B: the handler is left");
    TEST_ASSERT(b_mask == HAND, "B: a NATIVE noted leaves the mask as the handler left it (control)");
    TEST_EXPECT_EQ(c_rc, -1, "C: not in a handler -> refused");
    TEST_ASSERT(c_mask == HAND, "C: a refused restore touches nothing");
}

// The EL0 fault arm's latch act: a fault taken while a terminate latch is
// pending ends the thread with that note at the fault, because the tail it
// would otherwise wait for defers notes inside a handler or on a bad SP, and
// the faulting instruction would re-execute forever. Driven from an rfork'd
// child, since the helper acts on the calling thread. Returning is the defect
// in the live case: the child then leaves with 7, not the note's 1.
static volatile int g_fdl_pending;

static void fdl_live_entry(void *arg) {
    (void)arg;
    struct Thread *t = current_thread();
    (void)notes_post(t->proc, "interrupt", 0u, NULL, true);
    g_fdl_pending = thread_die_pending(t) ? 1 : 0;
    notes_fault_dying_latch(t);
    exits_code(7, "returned");
}

static bool fdl_str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// The status alone cannot tell the note's death from a snare:* one (both reap
// 1), so the message is read from the ZOMBIE, which the reap frees. NULL when
// the child is not a ZOMBIE inside the budget.
static const char *fdl_zombie_msg(int pid) {
    struct Proc *c = proc_find_by_pid(pid);
    if (!c) return NULL;
    u64 deadline = timer_now_ns() + 2000000000ull;
    for (;;) {
        irq_state_t s = proc_table_lock_acquire();
        const char *m = (c->state == PROC_STATE_ZOMBIE) ? c->exit_msg : NULL;
        proc_table_lock_release(s);
        if (m || timer_now_ns() >= deadline) return m;
        sched();
    }
}

void test_notes_fault_dying_latch_live_note_exits(void) {
    g_fdl_pending = -1;
    int pid = rfork(RFPROC, fdl_live_entry, NULL);
    const char *msg = pid > 0 ? fdl_zombie_msg(pid) : NULL;
    int st = -1;
    int got = pid > 0 ? wait_pid_for(pid, 0, &st) : -1;
    TEST_ASSERT(pid > 0, "rfork");
    TEST_EXPECT_EQ(got, pid, "reaped the child");
    TEST_EXPECT_EQ(g_fdl_pending, 1, "the interrupt armed a latch the child would die of");
    TEST_EXPECT_EQ(st, 1, "the child died of the note (1), not by returning (7)");
    TEST_ASSERT(msg != NULL && fdl_str_eq(msg, NOTE_NAME_INTERRUPT),
                "the child's exit message is the note's name, not a snare:*");
}

// The stale half: a latch whose class has no default-terminating note queued
// is cleared; one whose note is live but masked by THIS thread is kept, since a
// peer may still die of it -- and the call returns.
static volatile int g_fdl_returned;
static volatile u32 g_fdl_flags_after;

static void fdl_stale_entry(void *arg) {
    (void)arg;
    struct Thread *t = current_thread();
    struct Proc *p = t->proc;
    t->note_mask |= (1u << NOTE_BIT_INTERRUPT);
    (void)notes_post(p, "interrupt", 0u, NULL, true);
    __atomic_or_fetch(&p->proc_flags, PROC_FLAG_PIPE_TERMINATE_PENDING, __ATOMIC_RELEASE);
    notes_fault_dying_latch(t);
    g_fdl_returned = 1;
    g_fdl_flags_after = __atomic_load_n(&p->proc_flags, __ATOMIC_ACQUIRE);
    exits_code(0, "ok");
}

void test_notes_fault_dying_latch_clears_only_stale(void) {
    g_fdl_returned = 0;
    g_fdl_flags_after = 0;
    int pid = rfork(RFPROC, fdl_stale_entry, NULL);
    int st = -1;
    int got = pid > 0 ? wait_pid_for(pid, 0, &st) : -1;
    TEST_ASSERT(pid > 0, "rfork");
    TEST_EXPECT_EQ(got, pid, "reaped the child");
    TEST_EXPECT_EQ(g_fdl_returned, 1, "no unmasked live note: the call returned");
    TEST_EXPECT_EQ(st, 0, "the child left by its own exit");
    TEST_ASSERT((g_fdl_flags_after & PROC_FLAG_PIPE_TERMINATE_PENDING) == 0,
                "the stale pipe latch (no pipe note queued) was cleared");
    TEST_ASSERT((g_fdl_flags_after & PROC_FLAG_INTR_TERMINATE_PENDING) != 0,
                "the live interrupt latch this thread masks was kept");
}

// gcpark-b: the signal each death by note records, row by row (the same reason
// the default-action test names every row: a lookup that returned one signal for
// everything would pass a spot-check). Then the two pins: a known note kills iff
// it has a signal, and the phenotype's own map agrees on every note it carries.
void test_notes_death_signal_table(void) {
    TEST_EXPECT_EQ(notes_death_signal(NOTE_NAME_INTERRUPT), NOTE_SIG_INT,  "interrupt -> SIGINT");
    TEST_EXPECT_EQ(notes_death_signal(NOTE_NAME_KILL),      NOTE_SIG_KILL, "kill -> SIGKILL");
    TEST_EXPECT_EQ(notes_death_signal(NOTE_NAME_PIPE),      NOTE_SIG_PIPE, "pipe -> SIGPIPE");
    TEST_EXPECT_EQ(notes_death_signal(NOTE_NAME_TTY_QUIT),  NOTE_SIG_QUIT, "tty:quit -> SIGQUIT");
    TEST_EXPECT_EQ(notes_death_signal(NOTE_NAME_TTY_HUP),   NOTE_SIG_HUP,  "tty:hup -> SIGHUP");

    TEST_EXPECT_EQ(notes_death_signal(NOTE_NAME_SNARE_SEGV),  NOTE_SIG_SEGV, "snare:segv -> SIGSEGV");
    TEST_EXPECT_EQ(notes_death_signal(NOTE_NAME_SNARE_BUS),   NOTE_SIG_BUS,  "snare:bus -> SIGBUS");
    TEST_EXPECT_EQ(notes_death_signal(NOTE_NAME_SNARE_ALIGN), NOTE_SIG_BUS,  "snare:align -> SIGBUS");
    TEST_EXPECT_EQ(notes_death_signal(NOTE_NAME_SNARE_BTI),   NOTE_SIG_ILL,  "snare:bti -> SIGILL");
    TEST_EXPECT_EQ(notes_death_signal(NOTE_NAME_SNARE_BRK),   NOTE_SIG_TRAP, "snare:brk -> SIGTRAP");
    TEST_EXPECT_EQ(notes_death_signal(NOTE_NAME_SNARE_ILL),   NOTE_SIG_ILL,  "snare:ill -> SIGILL");
    TEST_EXPECT_EQ(notes_death_signal(NOTE_NAME_SNARE_FPE),   NOTE_SIG_FPE,  "snare:fpe -> SIGFPE");
    TEST_EXPECT_EQ(notes_death_signal(NOTE_NAME_SNARE_ABORT), NOTE_SIG_ABRT, "snare:abort -> SIGABRT");

    // Notes that cannot kill, and strings that are no note: no signal.
    TEST_EXPECT_EQ(notes_death_signal(NOTE_NAME_CHILD_EXIT), 0, "child_exit has no death signal");
    TEST_EXPECT_EQ(notes_death_signal(NOTE_NAME_TTY_WINCH),  0, "tty:winch has no death signal");
    TEST_EXPECT_EQ(notes_death_signal(NOTE_NAME_TTY_SUSP),   0, "tty:susp stops; no death signal");
    TEST_EXPECT_EQ(notes_death_signal(NOTE_NAME_TTY_CONT),   0, "tty:cont has no death signal");
    TEST_EXPECT_EQ(notes_death_signal(NOTE_NAME_SAK),        0, "sak has no death signal");
    TEST_EXPECT_EQ(notes_death_signal("killed"),             0, "\"killed\" is a message, not a note");
    TEST_EXPECT_EQ(notes_death_signal("unknown"),            0, "\"unknown\" is not a note");
    TEST_EXPECT_EQ(notes_death_signal("snare:"),             0, "the bare prefix is not a note");
    TEST_EXPECT_EQ(notes_death_signal(NULL),                 0, "NULL -> 0");

    // Every row of both tables, walked from the tables themselves: a known note
    // kills iff it records a signal, and every snare death records one.
    u32 known = 0, snares = 0;
    for (const char *name; (name = notes_known_name_for_test(known)) != NULL; known++) {
        bool kills = notes_default_action(name) == NOTE_DFL_TERMINATE;
        TEST_ASSERT(kills == (notes_death_signal(name) != 0),
                    "a known note records a death signal exactly when it terminates");
    }
    for (const char *name; (name = notes_snare_death_name_for_test(snares)) != NULL; snares++)
        TEST_ASSERT(notes_death_signal(name) != 0, "every snare death records a signal");
    TEST_ASSERT(known > 0 && snares > 0, "both tables were walked");

    // The phenotype translates signals to these notes through its own table; a
    // guest's waitpid must see the signal that table maps back from the note.
    // Only the rows it names compare: its snare rows have no name, a snare never
    // being posted, and their numbers are pinned by the VIV_SIG* _Static_assert
    // in notes.c. The count is derived, so a row added either side is counted.
    u32 compared = 0, terminating = 0;
    for (u32 v = 1; v < VIV_SIGNOTE_COUNT; v++) {
        const char *name = viv_signote_note_name((enum viv_signote)v);
        if (!name) continue;
        enum note_default dfl = notes_default_action(name);
        if (dfl == NOTE_DFL_TERMINATE) terminating++;
        // The phenotype answers a SIG_DFL guest from its own two default
        // tables; I-19 allows one default per note, so both must say what
        // g_known_notes says. `kill` is the terminate table's one omission: the
        // dispatcher's kill branch ends the Proc before any disposition is read.
        if (v != VIV_SIGNOTE_KILL)
            TEST_EXPECT_EQ((u64)viv_signote_default_is_terminate((enum viv_signote)v),
                           (u64)(dfl == NOTE_DFL_TERMINATE),
                           "the phenotype's terminate table is g_known_notes' dfl");
        TEST_EXPECT_EQ((u64)viv_signote_default_is_ignore((enum viv_signote)v),
                       (u64)(dfl == NOTE_DFL_IGNORE),
                       "the phenotype's ignore table is g_known_notes' dfl");
        int sig = notes_death_signal(name);
        if (sig == 0) continue;
        TEST_EXPECT_EQ((u64)sig, viv_signote_to_signal((enum viv_signote)v),
                       "a death reports the signal the phenotype maps its note from");
        compared++;
    }
    TEST_ASSERT(terminating > 0 && compared == terminating,
                "the phenotype cross-check compared every terminating note it names");
}

// SYS_POSTNOTE answers each refusal with its own errno (operator vote
// 2026-10-14), driven through the syscall's two arms. A bare -1 used to answer
// all of them, and Pouch's seam reads -1 as EIO.
void test_notes_postnote_errnos(void) {
    struct Proc *caller = proc_alloc();
    struct Proc *child  = proc_alloc();
    struct Proc *other  = proc_alloc();
    TEST_ASSERT(caller && child && other, "proc_alloc x3");
    proc_test_link(caller);
    proc_test_link_child(caller, child);
    proc_test_link(other);                       // alive, not the caller's child

    static struct Thread cth, kth, oth;
    kfg_thread(&cth, caller, 0x3e000001);
    kfg_thread(&kth, child,  0x3e000002);
    kfg_thread(&oth, other,  0x3e000003);
    const int NO_PID = 0x3ffffff0;

    s64 r_post  = sys_postnote_cross_for_test(caller, child->pid, NOTE_NAME_TERM);
    u32 c_term1 = ndflt_note_count(child, NOTE_NAME_TERM);
    s64 r_other = sys_postnote_cross_for_test(caller, other->pid, NOTE_NAME_TERM);
    u32 o_term  = ndflt_note_count(other, NOTE_NAME_TERM);
    s64 r_obogus = sys_postnote_cross_for_test(caller, other->pid, "bogus");
    s64 r_nopid = sys_postnote_cross_for_test(caller, NO_PID, NOTE_NAME_TERM);
    s64 r_nbogus = sys_postnote_cross_for_test(caller, NO_PID, "bogus");
    s64 r_bogus = sys_postnote_cross_for_test(caller, child->pid, "bogus");
    s64 r_tty   = sys_postnote_cross_for_test(caller, child->pid, NOTE_NAME_TTY_SUSP);
    s64 r_sak   = sys_postnote_cross_for_test(caller, child->pid, NOTE_NAME_SAK);
    s64 r_snare = sys_postnote_cross_for_test(caller, child->pid, NOTE_NAME_SNARE_SEGV);
    u32 c_count = child->notes->count;

    // Fill the child's ring with posts that arm nothing, then one more.
    u32 c_fill_bad = 0;
    while (child->notes->count < NOTE_QUEUE_DEPTH) {
        if (sys_postnote_cross_for_test(caller, child->pid, NOTE_NAME_CHILD_EXIT) != 0) {
            c_fill_bad++;
            break;
        }
    }
    s64 r_full  = sys_postnote_cross_for_test(caller, child->pid, NOTE_NAME_TERM);

    // The self arm: its own refusals, and a snare it never queues.
    s64 s_tty   = sys_postnote_self_for_test(caller, NOTE_NAME_TTY_SUSP);
    s64 s_snare = sys_postnote_self_for_test(caller, NOTE_NAME_SNARE_SEGV);
    u32 s_count = caller->notes->count;
    u32 s_fill_bad = 0;
    while (caller->notes->count < NOTE_QUEUE_DEPTH) {
        if (sys_postnote_self_for_test(caller, NOTE_NAME_CHILD_EXIT) != 0) {
            s_fill_bad++;
            break;
        }
    }
    s64 s_full  = sys_postnote_self_for_test(caller, NOTE_NAME_TERM);

    // A dead child is still a process until it is reaped; a dead non-child is
    // still not the caller's to signal.
    child->state = PROC_STATE_ZOMBIE;
    u32 c_before = child->notes->count;
    s64 r_dead  = sys_postnote_cross_for_test(caller, child->pid, NOTE_NAME_INTERRUPT);
    s64 r_dbogus = sys_postnote_cross_for_test(caller, child->pid, "bogus");
    s64 r_dsnare = sys_postnote_cross_for_test(caller, child->pid, NOTE_NAME_SNARE_SEGV);
    s64 r_dtty   = sys_postnote_cross_for_test(caller, child->pid, NOTE_NAME_TTY_CONT);
    u32 c_after = child->notes->count;
    other->state = PROC_STATE_ZOMBIE;
    s64 r_odead = sys_postnote_cross_for_test(caller, other->pid, NOTE_NAME_TERM);
    s64 r_odbogus = sys_postnote_cross_for_test(caller, other->pid, "bogus");

    proc_test_unlink(other);
    other->threads = NULL;
    proc_free(other);
    proc_test_unlink(child);
    child->threads = NULL;
    proc_free(child);
    proc_test_unlink(caller);
    caller->threads = NULL;
    caller->state   = PROC_STATE_ZOMBIE;
    proc_free(caller);

    TEST_EXPECT_EQ(r_post, (s64)0, "post(child, term) succeeds");
    TEST_EXPECT_EQ(c_term1, 1u, "and the child holds one queued term");
    TEST_EXPECT_EQ(r_other, -(s64)T_E_ACCES, "a non-child is EACCES: the parent gate, never -1");
    TEST_EXPECT_EQ(o_term, 0u, "and nothing reached it");
    TEST_EXPECT_EQ(r_obogus, -(s64)T_E_INVAL, "a non-child with a refused name: the name is judged before the gate");
    TEST_EXPECT_EQ(r_nopid, -(s64)T_E_SRCH, "no such pid is ESRCH");
    TEST_EXPECT_EQ(r_nbogus, -(s64)T_E_INVAL, "the name is judged before the pid is looked for");
    TEST_EXPECT_EQ(r_bogus, -(s64)T_E_INVAL, "a name that is no note is EINVAL");
    TEST_EXPECT_EQ(r_tty, -(s64)T_E_INVAL, "tty:susp is the kernel's to post: EINVAL");
    TEST_EXPECT_EQ(r_sak, -(s64)T_E_INVAL, "sak is the kernel's to post: EINVAL");
    TEST_EXPECT_EQ(r_snare, -(s64)T_E_INVAL, "a snare to another Proc is EINVAL: a fault is its own");
    TEST_EXPECT_EQ(c_count, 1u, "no refused post reached the child's ring");
    TEST_EXPECT_EQ(c_fill_bad, 0u, "the fill posts all landed");
    TEST_EXPECT_EQ(r_full, -(s64)T_E_AGAIN, "a full ring is EAGAIN");
    TEST_EXPECT_EQ(s_tty, -(s64)T_E_INVAL, "self: tty:susp is EINVAL");
    TEST_EXPECT_EQ(s_snare, -(s64)T_E_INVAL, "self arm: a snare is never queued (the entry ends the caller)");
    TEST_EXPECT_EQ(s_count, 0u, "and the caller's ring stayed empty");
    TEST_EXPECT_EQ(s_fill_bad, 0u, "the self fill posts all landed");
    TEST_EXPECT_EQ(s_full, -(s64)T_E_AGAIN, "self: a full ring is EAGAIN");
    TEST_EXPECT_EQ(r_dead, (s64)0, "a dead, unreaped child accepts it");
    TEST_EXPECT_EQ(r_dbogus, -(s64)T_E_INVAL, "but not a name that is no note");
    TEST_EXPECT_EQ(r_dsnare, -(s64)T_E_INVAL, "nor a snare: a dead child is still another Proc");
    TEST_EXPECT_EQ(r_dtty, -(s64)T_E_INVAL, "nor a tty name");
    TEST_EXPECT_EQ(c_after, c_before, "and nothing was queued to it");
    TEST_EXPECT_EQ(r_odead, -(s64)T_E_ACCES, "a dead non-child reads EACCES: the gate is asked first");
    TEST_EXPECT_EQ(r_odbogus, -(s64)T_E_INVAL, "and a refused name to it reads EINVAL: the name before the gate");
}

// The snares a Proc may post to itself are exactly the ones the phenotype's
// kill ends its caller with: one set, compared row by row against the death
// table rather than against a list a second change could forget.
void test_notes_self_death_snare(void) {
    u32 rows = 0, phen = 0, native = 0;
    for (u32 i = 0; notes_snare_death_name_for_test(i); i++) {
        const char *row = notes_snare_death_name_for_test(i);
        rows++;
        bool by_phenotype = false;
        for (u64 sig = 1; sig <= VIV_NSIG; sig++) {
            const char *name = NULL;
            if (vivarium_kill_decide(sig, &name) == VIV_KILL_SELF_DEATH &&
                name && fdl_str_eq(name, row)) {
                by_phenotype = true;
            }
        }
        const char *got = notes_self_death_snare(row);
        if (by_phenotype) phen++;
        if (got) native++;
        TEST_ASSERT((got != NULL) == by_phenotype,
                    "a snare is self-postable natively iff the phenotype's kill ends its caller with it");
        TEST_ASSERT(!got || fdl_str_eq(got, row), "and it answers with that snare's own name");
    }
    TEST_ASSERT(rows > 0 && phen > 0, "the walk saw the death table and the phenotype's self deaths");
    TEST_EXPECT_EQ(native, phen, "the two sets have the same size");

    TEST_ASSERT(notes_self_death_snare(NOTE_NAME_SNARE_ALIGN) == NULL, "snare:align stays the kernel's");
    TEST_ASSERT(notes_self_death_snare(NOTE_NAME_SNARE_BTI) == NULL, "snare:bti stays the kernel's");
    TEST_ASSERT(notes_self_death_snare(NOTE_NAME_SNARE_BRK) == NULL, "snare:brk stays the kernel's");
    TEST_ASSERT(notes_self_death_snare("snare:") == NULL, "the bare prefix is no snare");
    TEST_ASSERT(notes_self_death_snare("snare:segvx") == NULL, "nor is a longer name");
    TEST_ASSERT(notes_self_death_snare(NOTE_NAME_TERM) == NULL, "an ordinary note is not a death");
    TEST_ASSERT(notes_self_death_snare(NULL) == NULL, "NULL is refused");
}
