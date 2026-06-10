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
//   notes.unknown_name_rejected       — notes_post with unknown name → -1
//   notes.queue_full_returns_minus1   — non-synthetic posts fail at full
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

#include <thylacine/notes.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/spinlock.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../../arch/arm64/timer.h"

void test_notes_queue_alloc_free_smoke(void);
void test_notes_post_dequeue_smoke(void);
void test_notes_post_ordering(void);
void test_notes_unknown_name_rejected(void);
void test_notes_queue_full_returns_minus1(void);
void test_notes_coalesce_synthetic(void);
void test_notes_mask_defers(void);
void test_notes_kill_dequeue_smoke(void);
void test_notes_kill_bypasses_mask(void);
void test_notes_reenqueue_head_smoke(void);
void test_notes_fd_read_skips_kill(void);
void test_notes_fd_peek_skips_kill(void);
void test_notes_post_child_exit_helper(void);
void test_notes_post_pipe_helper(void);
void test_notes_proc_lifecycle(void);
void test_notes_peek_does_not_pop(void);
void test_notes_interrupt_terminate_gate(void);
void test_notes_self_managing_flag(void);

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

    TEST_EXPECT_EQ(notes_post(p, "alarm",      0u, NULL, true), -1,
                   "post(alarm) rejected (deferred at v1.0)");
    TEST_EXPECT_EQ(notes_post(p, "garbage",    0u, NULL, true), -1,
                   "post(garbage) rejected");
    TEST_EXPECT_EQ(notes_post(p, "",           0u, NULL, true), -1,
                   "post(empty) rejected");
    TEST_EXPECT_EQ(p->notes->count, 0u, "no rejected post landed in queue");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// queue_full_returns_minus1
// ---------------------------------------------------------------------------

void test_notes_queue_full_returns_minus1(void) {
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
    TEST_EXPECT_EQ(notes_post(p, "interrupt", 99u, NULL, false), -1,
                   "post at full → -1");

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

    // (d) Unregistering does not retro-arm; the NEXT post re-evaluates.
    notes_set_handler(p, 0u);
    TEST_ASSERT(!proc_intr_terminate_pending(p),
                "unregister does not retro-arm");
    TEST_EXPECT_EQ(notes_post(p, "interrupt", 0u, NULL, true), 0, "post 3");
    TEST_ASSERT(proc_intr_terminate_pending(p),
                "post after unregister re-arms");

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

    // (g) Non-interrupt names never arm (fresh Proc -- p carries the
    // self-managing mark from (f)).
    struct Proc *p2 = proc_alloc();
    TEST_ASSERT(p2 != NULL, "proc_alloc p2 succeeded");
    TEST_EXPECT_EQ(notes_post(p2, "child_exit", 0u, NULL, true), 0,
                   "post child_exit");
    TEST_EXPECT_EQ(notes_post(p2, "pipe", 0u, NULL, true), 0, "post pipe");
    TEST_ASSERT(!proc_intr_terminate_pending(p2),
                "non-interrupt posts never arm");

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

    // The predicate reads only t->proc + t->note_mask (the LS-5b fake-
    // thread idiom, plus the proc binding).
    struct Thread fake_t;
    fake_t.proc      = p;
    fake_t.note_mask = 0u;

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
    __atomic_store_n(&p->group_exit_msg, (const char *)NULL, __ATOMIC_RELEASE);
    fake_t.note_mask = 0u;

    // Drain the interrupt -> the latch clears -> false again.
    struct Note got;
    spin_lock(&p->notes->lock);
    (void)notes_dequeue_for_fd_locked(p, NULL, &got);
    spin_unlock(&p->notes->lock);
    TEST_ASSERT(!thread_die_pending(&fake_t), "drained -> false");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}
