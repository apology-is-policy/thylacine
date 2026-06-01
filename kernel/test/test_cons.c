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
//   cons.break_discarded    — a BREAK entry is discarded (A-4c-1; SAK is A-4c-2)
//   cons.read_busy_guard    — a 2nd reader (busy flag) returns -1, not data
//   cons.read_bad_args      — NULL buf / n<0 -> -1; n==0 -> 0 (no block)
//   cons.console_owner_intr — proc_console_post_interrupt posts to the owner;
//                             NULL/zombie owner is a no-op

#include "test.h"

#include <thylacine/cons.h>
#include <thylacine/dev.h>
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
void test_cons_break_discarded(void);
void test_cons_read_busy_guard(void);
void test_cons_read_bad_args(void);
void test_cons_console_owner_intr(void);

// cons.c test hooks + the extern Dev (read slot ignores the Spoor arg, so the
// tests pass NULL). proc.c test helpers (the test_proc.c / test_devproc.c pattern).
extern struct Dev devcons;
extern void proc_test_link(struct Proc *p);
extern void proc_test_unlink(struct Proc *p);

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

void test_cons_break_discarded(void) {
    cons_test_reset();
    cons_rx_input(0x00u, true);           // BREAK: A-4c-1 discards (SAK is A-4c-2)
    cons_rx_input((u8)'y', false);
    TEST_ASSERT(!cons_test_intr_pending(), "a BREAK is not a Ctrl-C (no intr-pending)");

    u8 buf[8];
    long got = devcons.read(NULL, buf, (long)sizeof(buf), 0);
    TEST_EXPECT_EQ(got, 1L, "only the data byte is in the ring (BREAK discarded)");
    TEST_EXPECT_EQ((long)buf[0], (long)'y', "the data byte is 'y'");
    cons_test_reset();
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
