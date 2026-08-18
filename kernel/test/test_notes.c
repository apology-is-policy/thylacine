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
//   notes.snare_forge_rejected        — user-path snare:* post → -1 (ABI)
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

// #15: test-support hooks + the harness's Proc-table linkage (see the
// "Test support" blocks in kernel/notes.c and kernel/proc.c -- deliberately
// absent from the headers, so every consumer extern-declares them).
extern u32  notes_name_terminate_latch_for_test(const char *name);
extern int  notes_noted_default(struct exception_context *ctx, struct Thread *t);
extern int  notes_noted_restore(struct exception_context *ctx, struct Thread *t);
extern void proc_test_link(struct Proc *p);
extern void proc_test_link_child(struct Proc *parent, struct Proc *p);
extern void proc_test_unlink(struct Proc *p);
extern int  sys_postnote_cross_for_test(struct Proc *caller, int target_pid,
                                        const char *name);   // #241
extern s64  sys_postnote_self_for_test(struct Proc *caller,
                                       const char *name);    // aux#253

void test_notes_queue_alloc_free_smoke(void);
void test_notes_post_dequeue_smoke(void);
void test_notes_post_ordering(void);
void test_notes_unknown_name_rejected(void);
void test_notes_snare_forge_rejected(void);
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
void test_notes_intr_latch_lifecycle(void);
void test_notes_die_pending_predicate(void);
void test_notes_caught_note_latch_lifecycle(void);
void test_notes_caught_note_deliverable_predicate(void);
void test_notes_caught_note_stop_dequeue_drains(void);
void test_notes_fstat_reports_chr(void);
void test_notes_linux_sigign_discard(void);
void test_notes_default_action_table(void);
void test_notes_ndflt_dispatch(void);
void test_notes_kill_terminates_single_thread(void);
void test_notes_ndflt_stop_discarded_after_cont(void);
void test_notes_susp_gate_reads_phenotype_sigtab(void);   // #251
void test_notes_masked_susp_stops_at_delivery(void);      // #252
void test_notes_stop_dequeue_picks_its_own_note(void);     // the class-blind pop
void test_notes_class_scans_read_phenotype_sigtab(void);   // c8ab2744 F1
void test_notes_discard_name_purges_pending(void);        // the install-time SIG_IGN discard
void test_notes_self_kill_through_full_ring(void);        // aux#253

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
// snare_forge_rejected
// ---------------------------------------------------------------------------

void test_notes_snare_forge_rejected(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc succeeded");

    // The ERRORS.md ABI commitment: the snare: prefix is reserved for
    // kernel-synthetic posters. A userspace-path post (synthetic=false,
    // the SYS_POSTNOTE shape) of ANY snare:* name must be rejected —
    // independent of whether snare:* ever joins the supported name set.
    TEST_EXPECT_EQ(notes_post(p, "snare:segv", 0u, NULL, false), -1,
                   "user-path post(snare:segv) rejected");
    TEST_EXPECT_EQ(notes_post(p, "snare:bus", 0u, NULL, false), -1,
                   "user-path post(snare:bus) rejected");
    TEST_EXPECT_EQ(notes_post(p, "snare:", 0u, NULL, false), -1,
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

    // The predicate reads t->proc + t->note_mask + t->exit_close_active
    // (the LS-5b fake-thread idiom, plus the proc binding; #68 F1 added the
    // exit-close gate, so the flag must be explicitly initialized here).
    struct Thread fake_t;
    fake_t.proc              = p;
    fake_t.note_mask         = 0u;
    fake_t.exit_close_active = false;

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

// ---------------------------------------------------------------------------
// caught_note_latch_lifecycle (item 11, ARCH §8.8.3)
// ---------------------------------------------------------------------------
//
// The PROC_FLAG_CAUGHT_NOTE_MASK sub-field: armed by notes_post's caught arm
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
    fake_t.proc              = p;
    fake_t.note_mask         = 0u;
    fake_t.exit_close_active = false;

    TEST_ASSERT(!thread_caught_note_deliverable(NULL), "NULL thread -> false");
    TEST_ASSERT(!thread_caught_note_deliverable(&fake_t), "fresh Proc -> false");

    // A caught (self-managing) note -> deliverable to an unmasked thread;
    // masked -> false (the thread defers, exactly as the terminate latch).
    proc_mark_self_managing_notes(p);
    TEST_EXPECT_EQ(notes_post(p, "interrupt", 0u, NULL, true), 0, "post");
    TEST_ASSERT(thread_caught_note_deliverable(&fake_t),
                "caught + unmasked -> true");
    fake_t.note_mask = (1u << NOTE_BIT_INTERRUPT);
    TEST_ASSERT(!thread_caught_note_deliverable(&fake_t),
                "caught + MASKED -> false (the thread defers)");
    fake_t.note_mask = 0u;

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
    __atomic_or_fetch(&kproc()->proc_flags, PROC_FLAG_CAUGHT_NOTE_MASK,
                      __ATOMIC_RELEASE);
    TEST_ASSERT(!thread_caught_note_deliverable(&fake_t),
                "kproc guarded even when the caught sub-field is forced");
    __atomic_and_fetch(&kproc()->proc_flags, ~PROC_FLAG_CAUGHT_NOTE_MASK,
                       __ATOMIC_RELEASE);

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
    __atomic_or_fetch(&p->proc_flags,
                      (1u << (PROC_CAUGHT_NOTE_SHIFT + NOTE_BIT_TTY)),
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

    // [child_exit, interrupt, pipe, interrupt]: two interrupts under SIG_DFL
    // (no handler, not self-managing) arm the interrupt terminate latch.
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_CHILD_EXIT, 1u, NULL, true), 0, "post child_exit");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_INTERRUPT, 2u, NULL, true), 0, "post interrupt 1");
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_PIPE, 3u, NULL, true), 0, "post pipe");
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
    TEST_ASSERT(r2 == 1 && a2 == 3u, "survivor 2 is the pipe (arg 3)");
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
    TEST_EXPECT_EQ(notes_post(p, NOTE_NAME_CHILD_EXIT, 0u, NULL, false), -1,
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
    // pipe's absence from the latch set is DELIBERATE and OPEN (task #237):
    // its default action is TERMINATE, but it carries no latch, so the tail
    // never default-terminates on it. Asserted as it stands so that whichever
    // way #237 resolves, it has to come through this line.
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
                   0, "pipe -> no latch (task #237, preserved deliberately)");
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
        idx = (idx + 1) % NOTE_QUEUE_DEPTH;
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
// el0_return_stop_check leaves only via group_exit_msg / !proc_stop_requested /
// thread_die_pending, none of which a latchless queued kill satisfies. The post
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

    int rc = sys_postnote_cross_for_test(parent, target->pid, NOTE_NAME_KILL);
    TEST_EXPECT_EQ(rc, 1, "the cross-Proc kill post reports success");

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
                                               NOTE_NAME_INTERRUPT), 1,
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
void test_notes_phenotype_sigreturn_restores_mask(void);
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
