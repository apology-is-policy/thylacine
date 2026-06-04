// /dev/cons console RX tests (A-4c-1).
//
// The integration harness cannot inject UART RX bytes (-serial mon:stdio run
// with < /dev/null; one PL011; no QMP serial channel) without touching the
// boot-banner test ABI -- IDENTITY-DESIGN.md section 9.8 test note. So these
// tests drive the console layer SYNTHETICALLY: cons_rx_input simulates the RX
// IRQ handler's per-byte hand-off, and the devcons.read vtable slot + the
// proc_console_* path are exercised directly. The real PL011 RX IRQ wiring
// (gic_attach + IMSC.RXIM unmask) is validated by boot survival + the
// interactive Ctrl-A b BREAK path.
//
// IMPORTANT: devcons.read BLOCKS on an empty ring (a real blocking read). Every
// test seeds ring data via cons_rx_input BEFORE calling devcons.read, so the
// drain path returns immediately and never sleeps in the harness.
//
//   cons.blocking_read_wakeup — a parked reader is woken by cons_rx_input (I-9)
//   cons.ring_fill_drain    — pushed data bytes drain in order
//   cons.ring_overflow_drop — a full ring drops excess; no corruption/overflow
//   cons.ctrlc_consumed     — Ctrl-C (0x03) sets intr-pending, is NOT ring data
//   cons.break_sets_sak     — a BREAK sets sak-pending (A-4c-2), is NOT ring data
//   cons.read_busy_guard    — a 2nd reader (busy flag) returns -1, not data
//   cons.read_bad_args      — NULL buf / n<0 -> -1; n==0 -> 0 (no block)
//   cons.console_owner_intr — proc_console_post_interrupt posts to the owner;
//                             NULL/zombie owner is a no-op
//   proc.revoke_console_attached — the atomic clear (A-4c-2); idempotent
//   cons.sak_revoke_regrant — SAK revokes from owner (+ note) + re-grants to trusted
//   cons.sak_failsafe_revoke_only — SAK with no trusted Proc clears the owner
//   cons.sak_idempotent_flood — SAK when trusted already owns is a no-op (no note)

#include "test.h"

#include <thylacine/cons.h>
#include <thylacine/dev.h>
#include <thylacine/handle.h>   // A-5a: struct Handle / handle_get / KOBJ_SPOOR / RIGHT_*
#include <thylacine/notes.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/spoor.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

void test_cons_blocking_read_wakeup(void);
void test_cons_ring_fill_drain(void);
void test_cons_ring_overflow_drop(void);
void test_cons_ctrlc_consumed(void);
void test_cons_break_sets_sak(void);
void test_cons_read_busy_guard(void);
void test_cons_read_bad_args(void);
void test_cons_console_owner_intr(void);
void test_proc_revoke_console_attached(void);
void test_cons_sak_revoke_regrant(void);
void test_cons_sak_failsafe_revoke_only(void);
void test_cons_sak_idempotent_flood(void);
void test_cons_sak_via_console_mgr(void);
void test_proc_console_relinquish(void);              // A-5a (I-27)
void test_proc_console_relinquish_other_owner(void);  // A-5a (self-only)
void test_cons_console_open(void);                    // A-5a (SYS_CONSOLE_OPEN)

// cons.c test hooks + the extern Dev (read slot ignores the Spoor arg, so the
// tests pass NULL). proc.c test helpers (the test_proc.c / test_devproc.c pattern).
extern struct Dev devcons;
extern void proc_test_link(struct Proc *p);
extern void proc_test_unlink(struct Proc *p);
extern struct Proc *proc_test_console_owner(void);   // A-4c-2 SAK assertions

// A-5a: the SYS_CONSOLE_OPEN core + the shared read-via-handle helper (the
// test_sys_pipe.c pattern). devcons_read ignores the Spoor and drains the
// global ring, so the opened handle is a valid console reader.
extern hidx_t sys_console_open_for_proc(struct Proc *p);
extern s64 sys_read_for_proc(struct Proc *p, hidx_t h, u8 *kbuf, u64 len);

// 16-byte-bounded name equality against a literal.
static bool name_eq(const char *name, const char *lit) {
    for (u32 i = 0; i < NOTE_NAME_MAX; i++) {
        if (name[i] != lit[i]) return false;
        if (lit[i] == '\0')    return true;
    }
    return true;
}

// F3 (audit close): the BLOCKING-read path + the I-9 wakeup pairing. A consumer
// kthread parks in devcons_read on an empty ring; the main thread feeds a byte
// via cons_rx_input (which wakes the data Rendez) and asserts the consumer wakes
// and returns it. Mirrors the deterministic two-thread pattern in test_tsleep.c
// (explicit sched() yields; the consumer is the sole runnable thread). A LOST
// wakeup would hang the boot here (the consumer would never resume) -- so this is
// a real regression test for the cons I-9 pairing, not just the no-sleep drain.
//
// Registered FIRST among the cons.* tests so the console_mgr kthread is cleanly
// SLEEPING (a later cons test -- ctrlc_consumed -- wakes it via cons_rx_input(0x03),
// which would leave it RUNNABLE and perturb the single-runnable sched() dance).
// This test feeds only DATA (wakes g_cons_data_rendez, not the mgr Rendez).
static volatile int  g_cbr_ran;     // 0 -> 1 (pre-read) -> 2 (post-read)
static volatile long g_cbr_ret;     // devcons_read return value
static volatile int  g_cbr_byte;    // first byte read, or -1

static void cbr_consumer_entry(void) {
    g_cbr_ran = 1;
    u8 buf[4];
    long got = devcons.read(NULL, buf, (long)sizeof(buf), 0);   // empty ring -> parks
    g_cbr_ret  = got;
    g_cbr_byte = (got > 0) ? (int)buf[0] : -1;
    g_cbr_ran  = 2;
    for (;;) sched();   // park; the main thread frees us after observing ran == 2
}

void test_cons_blocking_read_wakeup(void) {
    cons_test_reset();
    g_cbr_ran = 0; g_cbr_ret = -1; g_cbr_byte = -1;
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty at test entry");

    struct Thread *consumer = thread_create(kproc(), cbr_consumer_entry);
    TEST_ASSERT(consumer != NULL, "thread_create(consumer)");
    ready(consumer);

    // Yield: consumer runs, sets ran=1, calls devcons.read on the empty ring,
    // parks on g_cons_data_rendez (SLEEPING); sched picks the main thread back.
    sched();
    TEST_EXPECT_EQ(g_cbr_ran, 1, "consumer ran + parked in devcons_read");
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING, "consumer SLEEPING inside devcons_read");

    // Producer: feed one byte. cons_rx_input enqueues it + wakeup()s the data
    // Rendez -> the consumer becomes RUNNABLE (a lost wakeup would leave it SLEEPING).
    cons_rx_input((u8)'q', false);
    TEST_EXPECT_EQ(consumer->state, THREAD_RUNNABLE, "consumer woken by cons_rx_input");

    // Yield: consumer resumes inside devcons_read, drains 'q', returns 1, parks.
    sched();
    TEST_EXPECT_EQ(g_cbr_ran, 2, "consumer resumed post-wake");
    TEST_EXPECT_EQ(g_cbr_ret, 1L, "devcons_read returned exactly 1 byte");
    TEST_EXPECT_EQ((long)g_cbr_byte, (long)'q', "the woken read returned 'q'");

    thread_free(consumer);
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after consumer freed");
    cons_test_reset();
}

void test_cons_ring_fill_drain(void) {
    cons_test_reset();
    const char *s = "hello";
    for (int i = 0; i < 5; i++) cons_rx_input((u8)s[i], false);

    u8 buf[16];
    long got = devcons.read(NULL, buf, (long)sizeof(buf), 0);
    TEST_EXPECT_EQ(got, 5L, "drained all 5 buffered bytes");
    bool match = (buf[0]=='h' && buf[1]=='e' && buf[2]=='l' && buf[3]=='l' && buf[4]=='o');
    TEST_ASSERT(match, "bytes drained in FIFO order");
    cons_test_reset();
}

void test_cons_ring_overflow_drop(void) {
    cons_test_reset();
    // Push past capacity (256 = CONS_RING_SIZE; push 256 + 10). The fill bytes
    // MUST be non-control (>= 0x80) -- a 0x03 would be cooked-consumed as Ctrl-C,
    // not enqueued, perturbing the fill. The byte value encodes the push index
    // mod 0x80, so drop-newest + FIFO order are checkable: the first 256 pushed
    // (i = 0..255) are retained, the last 10 (i = 256..265) are dropped.
    for (int i = 0; i < 266; i++) cons_rx_input((u8)(0x80u | (i & 0x7fu)), false);
    TEST_ASSERT(!cons_test_intr_pending(), "no Ctrl-C among the >= 0x80 fill bytes");

    static u8 buf[512];
    long got = devcons.read(NULL, buf, (long)sizeof(buf), 0);
    TEST_EXPECT_EQ(got, 256L, "drains exactly the ring capacity (excess dropped)");
    TEST_EXPECT_EQ((long)buf[0],   (long)(0x80u | 0u),     "first retained = first pushed");
    TEST_EXPECT_EQ((long)buf[255], (long)(0x80u | 0x7fu),  "last retained = 256th pushed (drop-newest)");
    cons_test_reset();
}

void test_cons_ctrlc_consumed(void) {
    cons_test_reset();
    cons_rx_input(0x03u, false);          // Ctrl-C: cooked-consumed, NOT ring data
    cons_rx_input((u8)'x', false);        // a following data byte
    TEST_ASSERT(cons_test_intr_pending(), "Ctrl-C set intr-pending");

    u8 buf[8];
    long got = devcons.read(NULL, buf, (long)sizeof(buf), 0);
    TEST_EXPECT_EQ(got, 1L, "only the data byte is in the ring (Ctrl-C consumed)");
    TEST_EXPECT_EQ((long)buf[0], (long)'x', "the data byte is 'x', not 0x03");
    cons_test_reset();
}

void test_cons_break_sets_sak(void) {
    cons_test_reset();
    cons_rx_input(0x00u, true);           // BREAK: A-4c-2 SAK -> sak-pending (NOT ring data)
    cons_rx_input((u8)'y', false);
    TEST_ASSERT(cons_test_sak_pending(), "a BREAK set sak-pending (A-4c-2 SAK)");
    TEST_ASSERT(!cons_test_intr_pending(), "a BREAK is not a Ctrl-C (no intr-pending)");

    u8 buf[8];
    long got = devcons.read(NULL, buf, (long)sizeof(buf), 0);
    TEST_EXPECT_EQ(got, 1L, "only the data byte is in the ring (BREAK is not data)");
    TEST_EXPECT_EQ((long)buf[0], (long)'y', "the data byte is 'y', not the BREAK's 0x00");
    cons_test_reset();                    // clears sak-pending before console_mgr acts

    // The BREAK woke the boot console_mgr kthread (wake-only); with sak-pending
    // now cleared, drain it deterministically: sched() lets it re-observe the
    // false cond and return to SLEEPING (rather than leaving it runnable for a
    // later test to trip over -- A-4c-2 audit F2). g_console_owner is NULL here
    // (pre-joey), so even if it ran proc_console_sak it would be a fail-safe no-op.
    sched();
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "console_mgr drained back to SLEEPING");
}

void test_cons_read_busy_guard(void) {
    cons_test_reset();
    cons_test_set_reader_busy(true);
    cons_rx_input((u8)'z', false);        // data present, but a reader is parked

    u8 buf[8];
    long got = devcons.read(NULL, buf, (long)sizeof(buf), 0);
    TEST_EXPECT_EQ(got, -1L, "a 2nd concurrent reader returns -1 (single-reader guard)");

    cons_test_set_reader_busy(false);     // free the slot; the byte is still buffered
    got = devcons.read(NULL, buf, (long)sizeof(buf), 0);
    TEST_EXPECT_EQ(got, 1L, "once free, the buffered byte drains");
    TEST_EXPECT_EQ((long)buf[0], (long)'z', "the byte is 'z'");
    cons_test_reset();
}

void test_cons_read_bad_args(void) {
    cons_test_reset();
    u8 buf[8];
    TEST_EXPECT_EQ(devcons.read(NULL, NULL, 8, 0), -1L, "NULL buf -> -1");
    TEST_EXPECT_EQ(devcons.read(NULL, buf, -1, 0), -1L, "n < 0 -> -1");
    TEST_EXPECT_EQ(devcons.read(NULL, buf, 0, 0), 0L, "n == 0 -> 0 (no block)");
    cons_test_reset();
}

void test_cons_console_owner_intr(void) {
    struct Proc *owner = proc_alloc();
    TEST_ASSERT(owner != NULL, "proc_alloc owner");
    TEST_ASSERT(owner->notes != NULL, "owner has a note queue");
    owner->principal_id = 0x0C0FFEEu;
    owner->state        = PROC_STATE_ALIVE;

    // Live owner: proc_console_post_interrupt posts the `interrupt` note.
    proc_set_console_owner(owner);
    proc_console_post_interrupt();
    TEST_EXPECT_EQ(owner->notes->count, 1u, "interrupt note posted to the owner");

    struct Note got;
    spin_lock(&owner->notes->lock);
    int popped = notes_dequeue_locked(owner, NULL, &got);
    spin_unlock(&owner->notes->lock);
    TEST_EXPECT_EQ(popped, 1, "dequeued the posted note");
    TEST_ASSERT(name_eq(got.name, "interrupt"), "the posted note is `interrupt`");

    // No owner (the A-4c-2 fail-safe revoke-only state): a no-op.
    proc_set_console_owner(NULL);
    proc_console_post_interrupt();
    TEST_EXPECT_EQ(owner->notes->count, 0u, "no owner -> no post");

    // A zombie owner: also a no-op (the post guards on state == ALIVE).
    proc_set_console_owner(owner);
    owner->state = PROC_STATE_ZOMBIE;
    proc_console_post_interrupt();
    TEST_EXPECT_EQ(owner->notes->count, 0u, "zombie owner -> no post");

    // Clear the owner BEFORE freeing so g_console_owner never dangles (in
    // production proc_become_zombie_locked does this; proc_free here does not
    // route through that chokepoint).
    proc_set_console_owner(NULL);
    proc_free(owner);
}

// A-4c-2: the atomic console-attach clear (the unset side the SAK needs).
void test_proc_revoke_console_attached(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    p->state = PROC_STATE_ALIVE;

    proc_mark_console_attached(p);
    TEST_ASSERT(proc_is_console_attached(p), "marked console-attached");
    proc_revoke_console_attached(p);
    TEST_ASSERT(!proc_is_console_attached(p), "revoke cleared the bit");
    proc_revoke_console_attached(p);                     // idempotent
    TEST_ASSERT(!proc_is_console_attached(p), "revoke is idempotent");
    proc_revoke_console_attached(NULL);                  // fail-closed no-op (must not crash)

    p->state = PROC_STATE_ZOMBIE;                         // proc_free requires ZOMBIE
    proc_free(p);
}

// A-5a (I-27): proc_console_relinquish clears the caller's OWN console-attach
// AND, when the caller is the current owner, clears the owner pointer. joey calls
// this at the bringup->session boundary so corvus becomes the SOLE attached Proc
// during a session. Idempotent + fail-closed on NULL.
void test_proc_console_relinquish(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    p->state = PROC_STATE_ALIVE;

    proc_mark_console_attached(p);
    proc_set_console_owner(p);
    TEST_ASSERT(proc_is_console_attached(p), "p marked console-attached");
    TEST_ASSERT(proc_test_console_owner() == p, "p is the console owner");

    proc_console_relinquish(p);
    TEST_ASSERT(!proc_is_console_attached(p), "relinquish cleared p's attach bit");
    TEST_ASSERT(proc_test_console_owner() == NULL, "relinquish cleared the owner (was p)");

    proc_console_relinquish(p);                           // idempotent
    TEST_ASSERT(!proc_is_console_attached(p), "relinquish is idempotent");
    proc_console_relinquish(NULL);                        // fail-closed no-op (no crash)

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// A-5a (self-only): relinquish clears ONLY the caller's attach; it must NOT clear
// the owner pointer when a DIFFERENT Proc owns the console (the session has moved
// ownership on). Guards the "joey relinquish must not disturb a live session".
void test_proc_console_relinquish_other_owner(void) {
    struct Proc *owner = proc_alloc();
    struct Proc *p     = proc_alloc();
    TEST_ASSERT(owner != NULL && p != NULL, "proc_alloc x2");
    owner->state = PROC_STATE_ALIVE;
    p->state     = PROC_STATE_ALIVE;

    proc_set_console_owner(owner);
    proc_mark_console_attached(p);
    proc_console_relinquish(p);                           // p is attached but NOT the owner
    TEST_ASSERT(!proc_is_console_attached(p), "p's attach cleared");
    TEST_ASSERT(proc_test_console_owner() == owner, "owner pointer untouched (p != owner)");

    proc_set_console_owner(NULL);                         // clear the static before free
    owner->state = PROC_STATE_ZOMBIE; proc_free(owner);
    p->state     = PROC_STATE_ZOMBIE; proc_free(p);
}

// A-5a (SYS_CONSOLE_OPEN): the open core installs a R|W KOBJ_SPOOR handle on
// /dev/cons, and a read through it drains the RX ring -- the getty hands this to
// /sbin/login as fd 0/1/2 (the Unix login-reads-the-tty model). Proves the
// open -> handle -> devcons_read path end-to-end.
void test_cons_console_open(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    p->state = PROC_STATE_ALIVE;

    hidx_t fd = sys_console_open_for_proc(p);
    TEST_ASSERT(fd >= 0, "sys_console_open_for_proc returned a valid fd");

    struct Handle h;
    TEST_ASSERT(handle_get(p, fd, &h) == 0, "console handle installed");
    TEST_EXPECT_EQ((int)h.kind, (int)KOBJ_SPOOR, "console handle is KOBJ_SPOOR");
    TEST_EXPECT_EQ(h.rights, (rights_t)(RIGHT_READ | RIGHT_WRITE),
        "console handle rights are R|W");
    handle_put(&h);

    // Seed the RX ring + read through the handle (the login-reads-the-tty path).
    cons_test_reset();
    cons_rx_input((u8)'k', false);
    u8 buf[4] = { 0 };
    s64 got = sys_read_for_proc(p, fd, buf, sizeof(buf));
    TEST_EXPECT_EQ(got, 1L, "read through the console handle drained 1 byte");
    TEST_EXPECT_EQ((long)buf[0], (long)'k', "the byte read back is 'k'");

    handle_close(p, fd);
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// A-4c-2 SAK core: a recognized BREAK revokes the console from the current owner
// (+ notifies it) and re-grants it to the trusted login authority, which becomes
// the new owner. proc_console_sak is invoked DIRECTLY (the console_mgr dispatch
// is straight-line; the cons_rx_input -> sak-pending half is cons.break_sets_sak)
// so the transition is deterministic under the UP-like test scheduler.
void test_cons_sak_revoke_regrant(void) {
    struct Proc *owner   = proc_alloc();
    struct Proc *trusted = proc_alloc();
    TEST_ASSERT(owner != NULL && trusted != NULL, "proc_alloc owner + trusted");
    TEST_ASSERT(owner->notes != NULL, "owner has a note queue");
    owner->state   = PROC_STATE_ALIVE;
    trusted->state = PROC_STATE_ALIVE;

    // Owner holds the console; trusted is the designated re-grant target but is
    // NOT yet console-attached (mirrors corvus pre-SAK -- the SAK is what grants
    // it the bit).
    proc_mark_console_attached(owner);
    TEST_ASSERT(!proc_is_console_attached(trusted), "trusted not console-attached pre-SAK");
    proc_set_console_owner(owner);
    proc_set_console_trusted(trusted);

    proc_console_sak();

    TEST_ASSERT(!proc_is_console_attached(owner), "SAK revoked the old owner's console bit");
    TEST_ASSERT(proc_is_console_attached(trusted), "SAK granted the trusted Proc the console bit");
    TEST_EXPECT_EQ(proc_test_console_owner(), trusted, "trusted is the new console owner");
    TEST_EXPECT_EQ(owner->notes->count, 1u, "the revoked owner got a notify note");

    struct Note got;
    spin_lock(&owner->notes->lock);
    int popped = notes_dequeue_locked(owner, NULL, &got);
    spin_unlock(&owner->notes->lock);
    TEST_EXPECT_EQ(popped, 1, "dequeued the notify note");
    TEST_ASSERT(name_eq(got.name, "interrupt"), "the notify note is `interrupt`");

    // Clear the pointers BEFORE freeing so neither dangles.
    proc_set_console_owner(NULL);
    proc_set_console_trusted(NULL);
    owner->state   = PROC_STATE_ZOMBIE;                  // proc_free requires ZOMBIE
    trusted->state = PROC_STATE_ZOMBIE;
    proc_free(owner);
    proc_free(trusted);
}

// A-4c-2 SAK fail-safe: with no trusted Proc registered, the SAK is revoke-only
// (the owner is cleared to NULL) -- no Proc can then redeem elevation until a
// trusted login claims the console.
void test_cons_sak_failsafe_revoke_only(void) {
    struct Proc *owner = proc_alloc();
    TEST_ASSERT(owner != NULL, "proc_alloc owner");
    TEST_ASSERT(owner->notes != NULL, "owner has a note queue");
    owner->state = PROC_STATE_ALIVE;

    proc_mark_console_attached(owner);
    proc_set_console_owner(owner);
    proc_set_console_trusted(NULL);            // no trusted authority alive

    proc_console_sak();

    TEST_ASSERT(!proc_is_console_attached(owner), "SAK revoked the owner's console bit");
    TEST_EXPECT_EQ(proc_test_console_owner(), (struct Proc *)NULL,
                   "fail-safe: owner cleared to NULL (revoke-only)");
    TEST_EXPECT_EQ(owner->notes->count, 1u, "the revoked owner got a notify note");

    proc_set_console_owner(NULL);
    owner->state = PROC_STATE_ZOMBIE;                    // proc_free requires ZOMBIE
    proc_free(owner);
}

// A-4c-2 SAK idempotency: if the trusted authority already holds + owns the
// console, a SAK (and a BREAK flood) is a no-op -- no spurious revoke / re-grant
// / note.
void test_cons_sak_idempotent_flood(void) {
    struct Proc *trusted = proc_alloc();
    TEST_ASSERT(trusted != NULL, "proc_alloc trusted");
    TEST_ASSERT(trusted->notes != NULL, "trusted has a note queue");
    trusted->state = PROC_STATE_ALIVE;

    proc_mark_console_attached(trusted);
    proc_set_console_owner(trusted);           // trusted already owns the console
    proc_set_console_trusted(trusted);

    proc_console_sak();                         // owner == trusted -> no-op
    proc_console_sak();                         // flood: still a no-op

    TEST_ASSERT(proc_is_console_attached(trusted), "trusted retains the console (idempotent)");
    TEST_EXPECT_EQ(proc_test_console_owner(), trusted, "trusted remains the console owner");
    TEST_EXPECT_EQ(trusted->notes->count, 0u, "no spurious notify note on a no-op SAK");

    proc_set_console_owner(NULL);
    proc_set_console_trusted(NULL);
    trusted->state = PROC_STATE_ZOMBIE;                  // proc_free requires ZOMBIE
    proc_free(trusted);
}

// A-4c-2 end-to-end: the full BREAK -> sak-pending -> console_mgr -> proc_console_sak
// path, driven through the REAL boot `console_mgr` kthread (not a direct call). Closes
// the dispatch-arm coverage gap (console_mgr_main's `if (do_sak)` line; A-4c-2 audit F3).
void test_cons_sak_via_console_mgr(void) {
    // The boot console_mgr must be SLEEPING at entry: drain any stale wake left
    // by an earlier cons test (cons_test_reset clears the conds; sched() lets it
    // re-observe + re-sleep). A LOST wakeup would hang the boot here.
    cons_test_reset();
    sched();
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "console_mgr SLEEPING at entry");

    struct Proc *owner   = proc_alloc();
    struct Proc *trusted = proc_alloc();
    TEST_ASSERT(owner != NULL && trusted != NULL, "proc_alloc owner + trusted");
    TEST_ASSERT(owner->notes != NULL, "owner has a note queue");
    owner->state   = PROC_STATE_ALIVE;
    trusted->state = PROC_STATE_ALIVE;
    proc_mark_console_attached(owner);
    proc_set_console_owner(owner);
    proc_set_console_trusted(trusted);

    // Drive the IRQ-side half: a BREAK sets sak-pending + wakes console_mgr.
    cons_rx_input(0x00u, true);
    TEST_ASSERT(cons_test_sak_pending(), "BREAK set sak-pending + woke console_mgr");
    TEST_EXPECT_EQ(sched_runnable_count(), 1u, "console_mgr is RUNNABLE post-BREAK");

    // Yield: console_mgr resumes, clears sak-pending, runs proc_console_sak (the
    // transition), then loops back to sleep on a now-false cond (re-SLEEPING).
    sched();
    TEST_ASSERT(!cons_test_sak_pending(), "console_mgr consumed sak-pending");
    TEST_ASSERT(!proc_is_console_attached(owner), "console_mgr SAK revoked the owner");
    TEST_ASSERT(proc_is_console_attached(trusted), "console_mgr SAK re-granted the trusted Proc");
    TEST_EXPECT_EQ(proc_test_console_owner(), trusted, "trusted is the new console owner");
    TEST_EXPECT_EQ(owner->notes->count, 1u, "the revoked owner got a notify note");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "console_mgr re-SLEEPING after the SAK");

    proc_set_console_owner(NULL);
    proc_set_console_trusted(NULL);
    owner->state   = PROC_STATE_ZOMBIE;
    trusted->state = PROC_STATE_ZOMBIE;
    proc_free(owner);
    proc_free(trusted);
}
