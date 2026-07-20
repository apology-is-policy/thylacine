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
//   cons.sak_revoke_regrant — SAK revokes the owner's attach (NO note) + grants the attach to trusted; owner -> NULL
//   cons.sak_failsafe_revoke_only — SAK with no trusted Proc clears the owner, grants no attach
//   cons.sak_idempotent_flood — SAK when trusted is the sole attach holder with no owner is a no-op
//   cons.sak_does_not_terminate_trusted — RW-7 R2-F1/F2: SAK then Ctrl-C terminates neither the old owner nor trusted
//   cons.sak_attaches_from_relinquished_state — first SAK (owner NULL, trusted unattached) attaches trusted

#include "test.h"

#include <thylacine/cons.h>
#include <thylacine/dev.h>
#include <thylacine/handle.h>   // A-5a: struct Handle / handle_get / KOBJ_SPOOR / RIGHT_*
#include <thylacine/notes.h>
#include <thylacine/poll.h>     // LS-8a: cons_poll + poll_waiter
#include <thylacine/proc.h>
#include <thylacine/rendez.h>   // LS-8a: Rendez for the poll_waiter
#include <thylacine/sched.h>
#include <thylacine/spoor.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

void test_cons_blocking_read_wakeup(void);
void test_cons_tx_role_serializes_writers(void);
void test_cons_ring_fill_drain(void);
void test_cons_ring_overflow_drop(void);
void test_cons_rx_can_accept_boundary(void);
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
void test_cons_sak_does_not_terminate_trusted(void);
void test_cons_sak_attaches_from_relinquished_state(void);
void test_proc_console_relinquish(void);              // A-5a (I-27)
void test_proc_console_relinquish_other_owner(void);  // A-5a (self-only)
void test_cons_console_open(void);                    // A-5a (SYS_CONSOLE_OPEN)
void test_uart_rx_path_enabled(void);                 // #943 console-RX guard
void test_uart_putc_tx_bounded(void);                 // #67 bounded TX spin
void test_cons_poll_readiness(void);                  // LS-8a (POLLIN/POLLOUT sample)
void test_cons_poll_deferred_wake(void);              // LS-8a (the I-9 deferred relay)
void test_cons_termios_default(void);                 // LS-8b (default == pre-LS-8b)
void test_cons_cook_canonical_line(void);             // LS-8b (assemble + erase + deliver)
void test_cons_cook_echo_off_no_output(void);         // LS-8b (the ECHO-off hard guarantee)
void test_cons_cook_isig_toggle(void);                // LS-8b (Ctrl-C note vs data byte)
void test_cons_cook_icrnl(void);                      // LS-8b (input CR -> NL)
void test_cons_cook_onlcr_output(void);               // LS-8b (output NL -> CR NL)
void test_cons_consctl_parse(void);                   // LS-8b (+/-flag parse + malformed)
void test_cons_consctl_render(void);                  // LS-8b (read-back render)
void test_cons_cook_line_overflow(void);              // LS-8b (bounded line buffer)
void test_cons_cook_mode_flip_fresh_line(void);       // LS-8b audit F1 (consctl flip discards the fragment)
void test_cons_cook_canonical_poll_edge(void);        // LS-8b audit F2a (whole-line poll edge)

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

// #943: the PL011 RX path lives in arch/arm64/uart.c.
extern bool uart_rx_path_enabled(void);

// #67: uart_putc's bounded-TXFF-spin self-test lives in arch/arm64/uart.c
// (needs the static PL011 base + register offsets).
extern bool uart_selftest_tx_bounded(void);

// #943 regression: the PL011 RX path must be live after boot (CR.UARTEN|RXE).
// QEMU's PL011 resets with UARTEN clear, so this FAILS on the pre-fix kernel
// where uart_rx_init never set it -- the bug that made the console silently
// never receive a keystroke (interactive login impossible). uart_rx_init ran
// during boot; this reads the real PL011 CR directly.
void test_uart_rx_path_enabled(void) {
    TEST_ASSERT(uart_rx_path_enabled(), "PL011 RX path live (CR.UARTEN|RXE)");
}

// #67 regression: uart_putc must BOUND its TXFF spin. A stalled host serial
// consumer leaves the PL011 TX FIFO full, and the original `while(TXFF){}` was
// unbounded -> the CPU goes interrupt-dead (a soundness hazard on the print /
// crash-dump path -- the Halls dump runs IRQ-masked, and #66 proved a spin here
// inside an IRQ dispatch manufactures a seconds-long INTID stall). The helper
// (in uart.c) points the driver at a scratch region with FR stuck-full, calls
// uart_putc, and proves it RETURNED (an unbounded spin would hang the boot here)
// and DROPPED the byte. Revert uart_putc to `while(TXFF){}` and this hangs.
void test_uart_putc_tx_bounded(void) {
    TEST_ASSERT(uart_selftest_tx_bounded(),
                "uart_putc bounds the TXFF spin and drops on a stuck TX FIFO");
}

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
static volatile bool g_cbr_exited;  // #109: terminal-park reap handshake

static void cbr_consumer_entry(void) {
    g_cbr_ran = 1;
    u8 buf[4];
    long got = devcons.read(NULL, buf, (long)sizeof(buf), 0);   // empty ring -> parks
    g_cbr_ret  = got;
    g_cbr_byte = (got > 0) ? (int)buf[0] : -1;
    g_cbr_ran  = 2;
    test_kthread_park_terminal(&g_cbr_exited);   // #109: EXITING park (was for(;;)sched())
}

void test_cons_blocking_read_wakeup(void) {
    cons_test_reset();
    g_cbr_ran = 0; g_cbr_ret = -1; g_cbr_byte = -1; g_cbr_exited = false;
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

    test_kthread_join_free(consumer, &g_cbr_exited);
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

// #174 backpressure predicate: cons_rx_can_accept() is what the PL011 RX drain
// checks BEFORE reading a byte out of the FIFO -- on false it leaves the byte in
// the FIFO and masks RX (no loss) rather than letting cons_ring_push drop it.
// Must be true up to and including the 255->256 fill, false exactly at capacity.
void test_cons_rx_can_accept_boundary(void) {
    cons_test_reset();
    TEST_ASSERT(cons_rx_can_accept(), "empty ring accepts");
    for (int i = 0; i < 255; i++) {
        TEST_ASSERT(cons_rx_can_accept(), "ring below capacity accepts");
        cons_rx_input((u8)(0x80u | (i & 0x7fu)), false);
    }
    TEST_ASSERT(cons_rx_can_accept(), "255 bytes -> still room for the 256th (the boundary)");
    cons_rx_input((u8)(0x80u | 0x7fu), false);   // the 256th byte fills the ring
    TEST_ASSERT(!cons_rx_can_accept(), "full ring (256) refuses -> the drain pauses RX, no drop");
    cons_test_reset();
    TEST_ASSERT(cons_rx_can_accept(), "reset frees the ring");
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

// A-4c-2 SAK core (RW-7 R2-F1/F2 update): a recognized BREAK revokes the console
// ATTACH from the current owner and re-grants the ATTACH to the trusted login
// authority -- WITHOUT making it the owner and WITHOUT posting a note. owner
// (the Ctrl-C target) and attach (the elevation authority) are distinct roles
// post-LS-5. proc_console_sak is invoked DIRECTLY (the console_mgr dispatch is
// straight-line; the cons_rx_input -> sak-pending half is cons.break_sets_sak)
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
    TEST_ASSERT(proc_is_console_attached(trusted), "SAK granted the trusted Proc the console attach");
    // RW-7 R2-F1: the trusted Proc is attach-only -- NOT the console owner.
    TEST_EXPECT_EQ(proc_test_console_owner(), (struct Proc *)NULL,
                   "SAK leaves the owner NULL (trusted is the elevation authority, not a Ctrl-C target)");
    // RW-7 R2-F2: SAK posts NO note -- `interrupt` is now a terminate note, so
    // the old benign courtesy post would kill a non-self-managing owner.
    TEST_EXPECT_EQ(owner->notes->count, 0u, "SAK posts no note to the revoked owner");

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
    TEST_EXPECT_EQ(owner->notes->count, 0u, "RW-7 R2-F2: SAK posts no note to the revoked owner");

    proc_set_console_owner(NULL);
    owner->state = PROC_STATE_ZOMBIE;                    // proc_free requires ZOMBIE
    proc_free(owner);
}

// A-4c-2 SAK idempotency (RW-7 R2-F1 update): once the trusted login authority
// is the sole console authority (attached) and no owner remains to revoke, a SAK
// (and a BREAK flood) is a no-op -- no spurious re-grant / revoke / note. (The
// pre-fix premise "trusted already OWNS the console" is gone: post-R2-F1 trusted
// is attach-only and is never the owner.)
void test_cons_sak_idempotent_flood(void) {
    struct Proc *trusted = proc_alloc();
    TEST_ASSERT(trusted != NULL, "proc_alloc trusted");
    TEST_ASSERT(trusted->notes != NULL, "trusted has a note queue");
    trusted->state = PROC_STATE_ALIVE;

    // The post-SAK steady state: trusted is attach-only, no console owner.
    proc_mark_console_attached(trusted);
    proc_set_console_owner(NULL);
    proc_set_console_trusted(trusted);

    proc_console_sak();                         // already in the idempotent state -> no-op
    proc_console_sak();                         // flood: still a no-op

    TEST_ASSERT(proc_is_console_attached(trusted), "trusted retains the console attach (idempotent)");
    TEST_EXPECT_EQ(proc_test_console_owner(), (struct Proc *)NULL,
                   "owner stays NULL (trusted is attach-only, never the Ctrl-C owner)");
    TEST_EXPECT_EQ(trusted->notes->count, 0u, "no spurious note on a no-op SAK");

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
    TEST_EXPECT_EQ(proc_test_console_owner(), (struct Proc *)NULL,
                   "RW-7 R2-F1: SAK leaves the owner NULL (trusted is attach-only)");
    TEST_EXPECT_EQ(owner->notes->count, 0u, "RW-7 R2-F2: SAK posts no note");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "console_mgr re-SLEEPING after the SAK");

    proc_set_console_owner(NULL);
    proc_set_console_trusted(NULL);
    owner->state   = PROC_STATE_ZOMBIE;
    trusted->state = PROC_STATE_ZOMBIE;
    proc_free(owner);
    proc_free(trusted);
}

// RW-7 R2: read the LS-5 terminate latch (the `interrupt`-default-terminate
// disposition arms PROC_FLAG_INTR_TERMINATE_PENDING; proc.c's
// proc_intr_terminate_pending reads the same bit).
static bool intr_latch(struct Proc *p) {
    return (__atomic_load_n(&p->proc_flags, __ATOMIC_RELAXED)
            & PROC_FLAG_INTR_TERMINATE_PENDING) != 0;
}

// RW-7 R2-F1/F2 regression (the trusted-login-authority survival test): SAK
// separates the console OWNER (the Ctrl-C target) from the trusted login
// authority (the elevation/attach role). Pre-fix, SAK made the trusted Proc the
// OWNER and posted it `interrupt`, so (F2) the SAK armed the OLD owner's LS-5
// terminate latch, and (F1) a Ctrl-C AFTER the SAK posted `interrupt` to the
// trusted login authority (corvus), arming ITS latch -> the trusted path died
// until reboot. Post-fix the SAK grants only the ATTACH, leaves the owner NULL,
// and posts no note -- so neither the old owner nor the trusted authority is
// terminated.
void test_cons_sak_does_not_terminate_trusted(void) {
    struct Proc *owner   = proc_alloc();
    struct Proc *trusted = proc_alloc();
    struct Proc *control = proc_alloc();   // latch-mechanism positive control
    TEST_ASSERT(owner && trusted && control, "proc_alloc owner + trusted + control");
    TEST_ASSERT(owner->notes != NULL, "owner has a note queue");
    owner->state = trusted->state = control->state = PROC_STATE_ALIVE;

    // Positive control: a bare `interrupt` post to a non-self-managing Proc with
    // no handler arms the LS-5 terminate latch -- proving the latch mechanism is
    // LIVE in this harness, so the "no latch" assertions below are non-vacuous.
    notes_post(control, "interrupt", 0u, NULL, true);
    TEST_ASSERT(intr_latch(control),
                "control: interrupt to a bare Proc arms the terminate latch");

    proc_mark_console_attached(owner);
    proc_set_console_owner(owner);
    proc_set_console_trusted(trusted);

    proc_console_sak();

    // R2-F1: trusted gets the ATTACH but is NOT the console owner.
    TEST_ASSERT(proc_is_console_attached(trusted), "SAK granted trusted the console attach");
    TEST_EXPECT_EQ(proc_test_console_owner(), (struct Proc *)NULL,
                   "SAK leaves the owner NULL (trusted is attach-only)");
    // R2-F2: the SAK posted no `interrupt`, so the old owner is NOT terminated.
    TEST_ASSERT(!intr_latch(owner), "SAK did not arm the OLD owner's terminate latch");
    TEST_EXPECT_EQ(owner->notes->count, 0u, "SAK posted no note to the owner");

    // R2-F1 crux: a Ctrl-C after SAK targets g_console_owner (now NULL) -- NOT
    // corvus -- so the trusted login authority survives.
    proc_console_post_interrupt();
    TEST_ASSERT(!intr_latch(trusted),
                "Ctrl-C after SAK does NOT terminate the trusted login authority");

    proc_set_console_owner(NULL);
    proc_set_console_trusted(NULL);
    owner->state = trusted->state = control->state = PROC_STATE_ZOMBIE;
    proc_free(owner);
    proc_free(trusted);
    proc_free(control);
}

// RW-7 round-2 F4: the production-typical FIRST SAK fires from {owner == NULL
// (joey already relinquished its boot console), trusted == corvus alive but NOT
// yet attached}. The repurposed idempotency guard (proc_console_sak) must NOT
// no-op this state -- it must PROCEED to attach corvus -- and is saved only by
// its `proc_is_console_attached(trusted)` conjunct being false here. No other SAK
// test drives owner==NULL + unattached-trusted, so a future guard simplification
// that dropped the attach conjunct would no-op every real first SAK (the trusted
// path unreachable) and the suite would stay green. This pins it.
void test_cons_sak_attaches_from_relinquished_state(void) {
    struct Proc *trusted = proc_alloc();
    TEST_ASSERT(trusted != NULL, "proc_alloc trusted");
    trusted->state = PROC_STATE_ALIVE;

    // The relinquished state: no console owner; trusted designated but UNATTACHED.
    proc_set_console_owner(NULL);
    proc_set_console_trusted(trusted);
    TEST_ASSERT(!proc_is_console_attached(trusted), "trusted unattached pre-SAK");

    proc_console_sak();

    TEST_ASSERT(proc_is_console_attached(trusted),
                "first SAK from the relinquished state ATTACHES the trusted authority");
    TEST_EXPECT_EQ(proc_test_console_owner(), (struct Proc *)NULL,
                   "owner stays NULL (trusted is attach-only)");

    proc_set_console_owner(NULL);
    proc_set_console_trusted(NULL);
    trusted->state = PROC_STATE_ZOMBIE;
    proc_free(trusted);
}

// LS-8a: cons_poll readiness sampling. POLLIN iff the RX ring holds data;
// POLLOUT always (the UART never blocks -- so a poller MUST request POLLIN to
// wait for input, never POLLIN|POLLOUT). pw == NULL == sample-only (no hook).
void test_cons_poll_readiness(void) {
    cons_test_reset();
    sched();                                            // drain any stale mgr wake
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "console_mgr SLEEPING at entry");

    // Empty ring: not POLLIN-ready; always POLLOUT-ready.
    TEST_EXPECT_EQ((int)(cons_poll(POLLIN, NULL) & POLLIN), 0,
                   "empty cons: not POLLIN-ready");
    TEST_ASSERT((cons_poll(POLLOUT, NULL) & POLLOUT) != 0,
                "cons is always POLLOUT-ready (UART never blocks)");
    short both = cons_poll(POLLIN | POLLOUT, NULL);
    TEST_ASSERT((both & POLLOUT) != 0 && (both & POLLIN) == 0,
                "empty cons: POLLOUT ready, POLLIN not");

    // Seed a byte (the 0->1 edge wakes console_mgr): POLLIN ready on re-sample.
    cons_rx_input((u8)'r', false);
    TEST_ASSERT((cons_poll(POLLIN, NULL) & POLLIN) != 0,
                "buffered data -> POLLIN ready");

    cons_test_reset();                                  // clears poll_wake_pending
    sched();                                            // drain the woken mgr back to SLEEPING
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "console_mgr drained to SLEEPING");
}

// LS-8a: the I-9 DEFERRED poll-wake relay, driven through the REAL boot
// console_mgr kthread (cons_poll.tla). A poller registers a hook; a data byte
// arrives in IRQ context (cons_rx_input) and sets poll_wake_pending but does NOT
// walk the hook list (poll_waiter_list_wake is not IRQ-safe) -- so the hook stays
// not-ready until console_mgr runs in process context and walks it. A LOST relay
// (the cons_poll.tla NoMissedConsPoll violation) would leave pw.ready false and
// hang a real poller forever. Mirrors test_cons_sak_via_console_mgr's
// single-runnable sched() dance.
void test_cons_poll_deferred_wake(void) {
    cons_test_reset();
    sched();                                            // console_mgr to SLEEPING
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "console_mgr SLEEPING at entry");

    // Poller side: register a hook via cons_poll on the empty ring (register-
    // then-observe). No POLLIN yet; the hook is not ready.
    struct Rendez r; rendez_init(&r);
    struct poll_waiter pw; poll_waiter_init(&pw, &r);
    short rev = cons_poll(POLLIN, &pw);
    TEST_EXPECT_EQ((int)(rev & POLLIN), 0, "empty cons: no POLLIN at register");
    TEST_ASSERT(!pw.ready, "poll_waiter not ready before any data");

    // Producer (IRQ side): a data byte on the empty->non-empty edge arms
    // poll_wake_pending + wakes console_mgr (RUNNABLE) -- but does NOT walk the
    // hook list (deferred). The hook MUST still be not-ready here.
    cons_rx_input((u8)'p', false);
    TEST_ASSERT(cons_test_pollwake_pending(), "data byte armed poll_wake_pending");
    TEST_ASSERT(!pw.ready, "the IRQ producer did NOT wake the poll hook (deferred)");
    TEST_EXPECT_EQ(sched_runnable_count(), 1u, "console_mgr RUNNABLE post-byte");

    // Yield: console_mgr runs cons_service_deferred -> drains poll_wake_pending ->
    // poll_waiter_list_wake walks the list -> pw.ready = true, then re-sleeps.
    sched();
    TEST_ASSERT(pw.ready, "console_mgr's deferred walk set the poll hook ready");
    TEST_ASSERT(!cons_test_pollwake_pending(), "console_mgr consumed poll_wake_pending");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "console_mgr re-SLEEPING after the walk");

    // The ring now holds the byte -> POLLIN ready on re-sample.
    TEST_ASSERT((cons_poll(POLLIN, NULL) & POLLIN) != 0,
                "buffered data -> POLLIN ready on re-sample");

    poll_waiter_list_unregister(&pw);                   // NoStaleHook (stack waiter)
    cons_test_reset();
}

// =============================================================================
// LS-8b: the line discipline (termios / consctl). The cooking runs in
// cons_rx_input (IRQ context); these tests drive it synthetically (the harness
// cannot inject UART RX) and observe echo via the test capture sink (cons_emit
// buffers instead of writing the UART when capture is on).
// =============================================================================

// Drain the byte ring into `buf` (the devcons.read path ignores the Spoor).
static long cons_drain(u8 *buf, long n) {
    return devcons.read(NULL, buf, n, 0);
}

// Settle the console_mgr back to SLEEPING: a canonical line delivery / Ctrl-C
// arms a deferred flag + wakes the mgr; reset clears the flags, sched() lets it
// re-observe the false cond and re-sleep (the cons.sak_via_console_mgr pattern).
static void cons_settle_mgr(void) {
    cons_test_echo_capture(false);
    cons_test_reset();
    sched();
}

// LS-8b: the boot default is CONS_ISIG only -- byte-at-a-time, Ctrl-C cooked, no
// echo, no translation == EXACTLY the pre-LS-8b behavior (the no-breakage
// guarantee). A data byte goes straight to the ring; 0x03 is the interrupt note.
void test_cons_termios_default(void) {
    cons_test_reset();
    sched();
    TEST_EXPECT_EQ((long)cons_test_termios(), (long)CONS_TERMIOS_DEFAULT,
                   "reset -> termios is the boot default");
    TEST_EXPECT_EQ((long)cons_test_termios(), (long)CONS_ISIG,
                   "boot default == ISIG only");

    // No echo at default (ECHO clear): capture stays empty across a data byte.
    cons_test_echo_capture(true);
    cons_rx_input((u8)'q', false);
    u8 cap[16];
    TEST_EXPECT_EQ((long)cons_test_echo_captured(cap, sizeof(cap)), 0L,
                   "default ECHO-clear: a data byte echoes nothing");
    cons_test_echo_capture(false);

    u8 buf[8];
    TEST_EXPECT_EQ(cons_drain(buf, sizeof(buf)), 1L, "raw byte-at-a-time to the ring");
    TEST_EXPECT_EQ((long)buf[0], (long)'q', "the data byte is 'q'");

    // ISIG default: Ctrl-C is the interrupt note, not ring data.
    cons_rx_input(0x03u, false);
    TEST_ASSERT(cons_test_intr_pending(), "default ISIG: Ctrl-C -> interrupt note");
    cons_settle_mgr();
}

// LS-8b: canonical mode assembles a line, handles erase (backspace), and
// delivers the whole line + NL on Enter. With ECHO the typed chars + the erase
// "\b \b" + the NL are echoed; the ring sees ONLY the edited line.
void test_cons_cook_canonical_line(void) {
    cons_test_reset();
    sched();
    cons_test_set_termios(CONS_ICANON | CONS_ECHO);   // cooked + echo, no ONLCR
    cons_test_echo_capture(true);

    // Type "ab", backspace (erase 'b'), "c", Enter.
    cons_rx_input((u8)'a', false);
    cons_rx_input((u8)'b', false);
    cons_rx_input(0x08u, false);                      // BS erases 'b'
    cons_rx_input((u8)'c', false);
    cons_rx_input((u8)'\n', false);                   // deliver "ac\n"

    // Echo = 'a' 'b' ['\b' ' ' '\b'] 'c' '\n' = 7 bytes (no ONLCR -> bare NL).
    u8 cap[32];
    u32 got = cons_test_echo_captured(cap, sizeof(cap));
    TEST_EXPECT_EQ((long)got, 7L, "echo: a,b,erase(3),c,NL");
    const u8 want_echo[7] = { 'a','b','\b',' ','\b','c','\n' };
    bool echo_ok = true;
    for (int i = 0; i < 7; i++) if (cap[i] != want_echo[i]) echo_ok = false;
    TEST_ASSERT(echo_ok, "echo bytes match a,b,\\b,space,\\b,c,NL");
    cons_test_echo_capture(false);

    // The ring holds the EDITED line including the terminating newline.
    u8 buf[16];
    long n = cons_drain(buf, sizeof(buf));
    TEST_EXPECT_EQ(n, 3L, "canonical delivers the edited line + NL");
    TEST_ASSERT(buf[0]=='a' && buf[1]=='c' && buf[2]=='\n', "line is \"ac\\n\" (b erased)");
    cons_settle_mgr();
}

// LS-8b: the ECHO-off HARD guarantee -- with ECHO clear, NO input byte reaches
// console output (the password mask). The line still assembles + delivers; only
// the echo is suppressed.
void test_cons_cook_echo_off_no_output(void) {
    cons_test_reset();
    sched();
    cons_test_set_termios(CONS_ICANON);               // cooked, ECHO CLEAR
    cons_test_echo_capture(true);

    const char *secret = "hunter2";
    for (int i = 0; secret[i]; i++) cons_rx_input((u8)secret[i], false);
    cons_rx_input((u8)'\n', false);

    u8 cap[32];
    TEST_EXPECT_EQ((long)cons_test_echo_captured(cap, sizeof(cap)), 0L,
                   "ECHO-off: NOT ONE byte reaches the output (password mask)");
    cons_test_echo_capture(false);

    // The masked line is still delivered to the reader.
    u8 buf[16];
    long n = cons_drain(buf, sizeof(buf));
    TEST_EXPECT_EQ(n, 8L, "the masked line still delivers (hunter2 + NL)");
    bool ok = (buf[0]=='h' && buf[6]=='2' && buf[7]=='\n');
    TEST_ASSERT(ok, "delivered bytes are the secret + NL");
    cons_settle_mgr();
}

// LS-8b: ISIG gates the Ctrl-C cooking. Set -> 0x03 is the interrupt note (not
// ring data). Clear -> 0x03 is an ordinary data byte (no note).
void test_cons_cook_isig_toggle(void) {
    cons_test_reset();
    sched();

    // ISIG set (raw + ISIG): 0x03 cooked to the note, not enqueued.
    cons_test_set_termios(CONS_ISIG);
    cons_rx_input(0x03u, false);
    cons_rx_input((u8)'x', false);
    TEST_ASSERT(cons_test_intr_pending(), "ISIG set: Ctrl-C -> interrupt note");
    u8 buf[8];
    long n = cons_drain(buf, sizeof(buf));
    TEST_EXPECT_EQ(n, 1L, "ISIG set: only the data byte 'x' in the ring");
    TEST_EXPECT_EQ((long)buf[0], (long)'x', "ring byte is 'x', not 0x03");
    cons_settle_mgr();

    // ISIG clear (fully raw): 0x03 is a data byte, no note.
    cons_test_set_termios(0u);
    cons_rx_input(0x03u, false);
    TEST_ASSERT(!cons_test_intr_pending(), "ISIG clear: Ctrl-C is NOT a note");
    n = cons_drain(buf, sizeof(buf));
    TEST_EXPECT_EQ(n, 1L, "ISIG clear: 0x03 is enqueued as data");
    TEST_EXPECT_EQ((long)buf[0], 3L, "the ring byte is the literal 0x03");
    cons_settle_mgr();
}

// LS-8b: ICRNL translates an input CR (0x0d) to NL (0x0a). Tested in raw mode so
// the translated byte lands directly in the ring.
void test_cons_cook_icrnl(void) {
    cons_test_reset();
    sched();

    cons_test_set_termios(CONS_ICRNL);                // raw + ICRNL
    cons_rx_input((u8)'\r', false);
    u8 buf[8];
    long n = cons_drain(buf, sizeof(buf));
    TEST_EXPECT_EQ(n, 1L, "ICRNL set: CR enqueued");
    TEST_EXPECT_EQ((long)buf[0], (long)'\n', "ICRNL translated CR -> NL");
    cons_settle_mgr();

    cons_test_set_termios(0u);                         // raw, ICRNL clear
    cons_rx_input((u8)'\r', false);
    n = cons_drain(buf, sizeof(buf));
    TEST_EXPECT_EQ(n, 1L, "ICRNL clear: CR enqueued verbatim");
    TEST_EXPECT_EQ((long)buf[0], (long)'\r', "ICRNL clear: byte stays CR");
    cons_settle_mgr();
}

// LS-8b: ONLCR translates an OUTPUT NL to CR NL (cons_output_write). Default
// clear -> bare LF forwarded (the pre-LS-8b behavior).
void test_cons_cook_onlcr_output(void) {
    cons_test_reset();
    sched();

    // ONLCR set: "a\nb" -> "a\r\nb".
    cons_test_set_termios(CONS_ONLCR);
    cons_test_echo_capture(true);
    TEST_EXPECT_EQ(cons_output_write("a\nb", 3), 3L, "write returns the input count");
    u8 cap[16];
    u32 got = cons_test_echo_captured(cap, sizeof(cap));
    TEST_EXPECT_EQ((long)got, 4L, "ONLCR set: NL expands to CR NL (a,CR,NL,b)");
    bool ok = (cap[0]=='a' && cap[1]=='\r' && cap[2]=='\n' && cap[3]=='b');
    TEST_ASSERT(ok, "output is a,\\r,\\n,b");

    // ONLCR clear: bare LF forwarded.
    cons_test_set_termios(0u);
    cons_test_echo_capture(true);
    TEST_EXPECT_EQ(cons_output_write("a\nb", 3), 3L, "write returns the input count");
    got = cons_test_echo_captured(cap, sizeof(cap));
    TEST_EXPECT_EQ((long)got, 3L, "ONLCR clear: bare LF (a,NL,b)");
    ok = (cap[0]=='a' && cap[1]=='\n' && cap[2]=='b');
    TEST_ASSERT(ok, "output is a,\\n,b");
    cons_test_echo_capture(false);
    cons_settle_mgr();
}

// LS-8b: the /dev/consctl parse. "+name"/"-name" tokens set/clear a flag; a
// malformed token rejects the whole write (-1) with no change.
void test_cons_consctl_parse(void) {
    cons_test_reset();
    TEST_EXPECT_EQ((long)cons_test_termios(), (long)CONS_ISIG, "start at the default");

    TEST_EXPECT_EQ(cons_set_mode_cmd("+echo", 5), 5L, "+echo accepted");
    TEST_EXPECT_EQ((long)cons_test_termios(), (long)(CONS_ISIG | CONS_ECHO), "+echo set ECHO");

    TEST_EXPECT_EQ(cons_set_mode_cmd("-isig", 5), 5L, "-isig accepted");
    TEST_EXPECT_EQ((long)cons_test_termios(), (long)CONS_ECHO, "-isig cleared ISIG");

    TEST_EXPECT_EQ(cons_set_mode_cmd("+icanon +echo", 13), 13L, "two tokens accepted");
    TEST_EXPECT_EQ((long)cons_test_termios(), (long)(CONS_ICANON | CONS_ECHO),
                   "atomic multi-flag set");

    // Malformed commands reject (-1) and leave the mode unchanged.
    u32 before = cons_test_termios();
    TEST_EXPECT_EQ(cons_set_mode_cmd("+bogus", 6), -1L, "unknown name -> -1");
    TEST_EXPECT_EQ(cons_set_mode_cmd("echo", 4), -1L, "missing +/- sign -> -1");
    TEST_EXPECT_EQ(cons_set_mode_cmd("+", 1), -1L, "empty name -> -1");
    TEST_EXPECT_EQ(cons_set_mode_cmd("", 0), -1L, "empty command -> -1");
    TEST_EXPECT_EQ(cons_set_mode_cmd("+echo +bad", 10), -1L, "one bad token rejects the batch");
    TEST_EXPECT_EQ((long)cons_test_termios(), (long)before,
                   "a rejected command leaves the mode unchanged");
    cons_test_reset();
}

// LS-8b: the /dev/consctl read-back render. Symmetric grammar with the write:
// five "+name"/"-name" tokens + '\n'.
void test_cons_consctl_render(void) {
    cons_test_reset();                                 // default = ISIG only
    char buf[40];
    long n = cons_render_mode(buf, (long)sizeof(buf));
    const char *want_default = "-icanon -echo +isig -icrnl -onlcr\n";
    TEST_EXPECT_EQ(n, 34L, "default render length");
    bool ok = (n == 34);
    for (long i = 0; ok && i < n; i++) if (buf[i] != want_default[i]) ok = false;
    TEST_ASSERT(ok, "default renders -icanon -echo +isig -icrnl -onlcr");

    cons_test_set_termios(CONS_TERMIOS_ALL);
    n = cons_render_mode(buf, (long)sizeof(buf));
    const char *want_all = "+icanon +echo +isig +icrnl +onlcr\n";
    ok = (n == 34);
    for (long i = 0; ok && i < n; i++) if (buf[i] != want_all[i]) ok = false;
    TEST_ASSERT(ok, "all-set renders every flag with '+'");

    // A too-small buffer renders nothing (never a partial line).
    TEST_EXPECT_EQ(cons_render_mode(buf, 10), 0L, "too-small buffer -> 0");
    cons_test_reset();
}

// LS-8b: the canonical line buffer is BOUNDED -- a pathologically long line
// (CONS_LINE_MAX + extra) drops the overflow, never corrupting memory. Enter
// still delivers what fits (the ring then caps it too).
void test_cons_cook_line_overflow(void) {
    cons_test_reset();
    sched();
    cons_test_set_termios(CONS_ICANON);               // cooked, no echo

    for (int i = 0; i < 300; i++) cons_rx_input((u8)'A', false);   // > CONS_LINE_MAX (256)
    cons_rx_input((u8)'\n', false);                                // deliver

    // The ring caps at its capacity (256); every delivered byte is 'A' (the line
    // buffer never overflowed past CONS_LINE_MAX, and the ring never overflowed).
    static u8 buf[512];
    long n = cons_drain(buf, (long)sizeof(buf));
    TEST_ASSERT(n > 0 && n <= 256, "bounded delivery (<= ring capacity)");
    bool all_a = true;
    for (long i = 0; i < n; i++) if (buf[i] != 'A') all_a = false;
    TEST_ASSERT(all_a, "every delivered byte is 'A' -- no overflow corruption");
    cons_settle_mgr();
}

// LS-8b audit F1: a consctl mode change starts a FRESH canonical line (the
// TCSAFLUSH discipline) -- a half-assembled line[] is DISCARDED by any
// cons_set_mode_cmd write, so a flip can never strand a fragment that then
// prepends the next line. Drives the PRODUCTION cons_set_mode_cmd (NOT the
// cons_test_set_termios hook), the path the cooking tests otherwise never take:
// pre-fix the fragment survived (this delivered "abc\n", n == 4); post-fix only
// the post-flip line delivers (n == 1).
void test_cons_cook_mode_flip_fresh_line(void) {
    cons_test_reset();
    sched();
    cons_test_set_termios(CONS_ICANON | CONS_ISIG);   // cooked
    cons_test_echo_capture(true);                     // swallow the +echo NL echo (no stray UART byte)

    // Buffer a partial line (no Enter): "abc" sits in line[], the ring is empty.
    cons_rx_input((u8)'a', false);
    cons_rx_input((u8)'b', false);
    cons_rx_input((u8)'c', false);

    // A production consctl write (turns ECHO on + stays canonical) MUST discard
    // the fragment -- the flip itself is what resets the line, regardless of flags.
    TEST_EXPECT_EQ(cons_set_mode_cmd("+echo", 5), 5L, "consctl +echo accepted");

    // Deliver: only the bare NL arrives -- the "abc" fragment was discarded by
    // the mode change (pre-fix it would prepend, delivering "abc\n").
    cons_rx_input((u8)'\n', false);
    u8 buf[8];
    long n = cons_drain(buf, sizeof(buf));
    TEST_EXPECT_EQ(n, 1L, "mode flip discarded the fragment: only the NL delivered");
    TEST_EXPECT_EQ((long)buf[0], (long)'\n', "the delivered byte is the bare NL");
    cons_settle_mgr();
}

// LS-8b audit F2a: the canonical WHOLE-LINE poll edge. Ordinary chars buffer in
// line[] with the ring EMPTY (no poll edge while the line assembles); Enter
// flushes the whole line to the ring in ONE cons_rx_input call, arming the
// empty->non-empty edge exactly once + waking console_mgr, whose deferred walk
// makes the hook ready (the cons_poll.tla I-9 relay, driven by a multi-byte
// flush rather than the single-byte 8a path).
void test_cons_cook_canonical_poll_edge(void) {
    cons_test_reset();
    sched();                                            // console_mgr to SLEEPING
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "console_mgr SLEEPING at entry");
    cons_test_set_termios(CONS_ICANON | CONS_ISIG);     // cooked, no echo

    struct Rendez r; rendez_init(&r);
    struct poll_waiter pw; poll_waiter_init(&pw, &r);
    short rev = cons_poll(POLLIN, &pw);
    TEST_EXPECT_EQ((int)(rev & POLLIN), 0, "empty cons: no POLLIN at register");
    TEST_ASSERT(!pw.ready, "poll_waiter not ready before any line");

    // Buffer "hi": canonical mode holds it in line[]; the ring stays EMPTY, so
    // there is NO poll edge and console_mgr is NOT woken (the bytes have not
    // entered the ring -- POSIX canonical: a poller waits for a full line).
    cons_rx_input((u8)'h', false);
    cons_rx_input((u8)'i', false);
    TEST_ASSERT(!cons_test_pollwake_pending(), "buffered chars: no ring edge yet");
    TEST_ASSERT(!pw.ready, "no poll wake while the line is still assembling");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "console_mgr still SLEEPING (no edge)");

    // Enter: the whole line ("hi" + NL) flushes to the ring in ONE call, arming
    // the empty->non-empty edge once + waking console_mgr (deferred -- the IRQ
    // producer does NOT walk the hook).
    cons_rx_input((u8)'\n', false);
    TEST_ASSERT(cons_test_pollwake_pending(), "Enter flushed the line -> poll edge armed");
    TEST_ASSERT(!pw.ready, "the IRQ producer did NOT walk the hook (deferred)");
    TEST_EXPECT_EQ(sched_runnable_count(), 1u, "console_mgr RUNNABLE post-line");

    sched();                                            // mgr walks the hook list
    TEST_ASSERT(pw.ready, "console_mgr's deferred walk set the hook ready");
    TEST_ASSERT(!cons_test_pollwake_pending(), "console_mgr consumed poll_wake_pending");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "console_mgr re-SLEEPING after the walk");

    // The ring holds the whole delivered line.
    u8 buf[8];
    long n = cons_drain(buf, sizeof(buf));
    TEST_EXPECT_EQ(n, 3L, "the ring holds the delivered line hi+NL");
    TEST_ASSERT(buf[0]=='h' && buf[1]=='i' && buf[2]=='\n', "line is hi\\n");

    poll_waiter_list_unregister(&pw);                   // NoStaleHook (stack waiter)
    cons_settle_mgr();
}

// =============================================================================
// G-4: the console-renderer drain/feed backend (TAPESTRY.md section 18.7).
// The drain taps cons_emit (program output + echo); the feed injects bytes
// into the SAME line discipline as UART RX. Driven synthetically with echo
// capture on (UART suppressed) so the tests assert both sinks exactly.
// =============================================================================

// The tap mirrors program output into the drain while the UART sink (here:
// the capture buffer) stays byte-identical -- the tee property. Disarmed, the
// tap is inert; armed, every cons_emit byte lands in both.
void test_cons_drain_tap_mirrors_output(void) {
    cons_test_reset();
    cons_test_echo_capture(true);

    // Disarmed (boot state): output reaches the UART sink only.
    TEST_EXPECT_EQ(cons_output_write("pre", 3), 3L, "write accepted disarmed");
    TEST_EXPECT_EQ(cons_test_drain_count(), 0u, "disarmed drain captured nothing");

    TEST_EXPECT_EQ(cons_drain_open(), 0, "drain arms");
    TEST_EXPECT_EQ(cons_drain_open(), -1, "second concurrent open refused (single-open)");

    TEST_EXPECT_EQ(cons_output_write("hi\n", 3), 3L, "write accepted armed");
    TEST_EXPECT_EQ(cons_test_drain_count(), 3u, "drain captured the 3 bytes");

    u8 dbuf[8];
    long dn = cons_drain_read(dbuf, sizeof(dbuf));
    TEST_EXPECT_EQ(dn, 3L, "drain read returns the buffered bytes");
    TEST_ASSERT(dbuf[0]=='h' && dbuf[1]=='i' && dbuf[2]=='\n', "drain bytes == output");

    // The tee: the UART capture holds pre + hi\n -- the serial side unchanged.
    u8 cap[16];
    u32 got = cons_test_echo_captured(cap, sizeof(cap));
    TEST_EXPECT_EQ(got, 6u, "UART sink saw all 6 bytes (tee, not switch)");
    TEST_ASSERT(cap[3]=='h' && cap[4]=='i' && cap[5]=='\n', "UART bytes unchanged");

    cons_drain_close();
    cons_test_echo_capture(false);
    cons_test_reset();
}

// The feed runs the EXISTING line discipline: canonical assembly + echo (the
// echo landing in BOTH sinks -- the renderer paints its own echo) + ISIG (a
// graphical Ctrl-C posts `interrupt` exactly like a serial one). is_break is
// structurally unreachable from the feed (I-27: no SAK forgery).
void test_cons_drain_feed_runs_discipline(void) {
    cons_test_reset();
    cons_test_set_termios(CONS_ICANON | CONS_ECHO | CONS_ISIG);
    cons_test_echo_capture(true);
    TEST_EXPECT_EQ(cons_drain_open(), 0, "drain arms");

    // Feed a canonical line: assembled, delivered on NL, echoed to both sinks.
    TEST_EXPECT_EQ(cons_feed_write("ab\n", 3), 3L, "feed accepted");
    u8 in[8];
    long n = cons_input_read(in, sizeof(in));
    TEST_EXPECT_EQ(n, 3L, "the cooked line reached the input ring");
    TEST_ASSERT(in[0]=='a' && in[1]=='b' && in[2]=='\n', "line is ab\\n");

    u8 cap[8];
    u32 got = cons_test_echo_captured(cap, sizeof(cap));
    TEST_EXPECT_EQ(got, 3u, "echo emitted to the UART sink");
    TEST_EXPECT_EQ(cons_test_drain_count(), 3u, "echo ALSO mirrored into the drain");
    u8 dbuf[8];
    TEST_EXPECT_EQ(cons_drain_read(dbuf, sizeof(dbuf)), 3L, "drain read gets the echo");
    TEST_ASSERT(dbuf[0]=='a' && dbuf[1]=='b' && dbuf[2]=='\n', "drain echo == ab\\n");

    // The graphical Ctrl-C: ISIG cooks a fed 0x03 into the deferred interrupt.
    TEST_ASSERT(!cons_test_intr_pending(), "no interrupt pending before");
    TEST_EXPECT_EQ(cons_feed_write("\x03", 1), 1L, "Ctrl-C fed");
    TEST_ASSERT(cons_test_intr_pending(), "fed Ctrl-C cooked to the interrupt (ISIG)");

    cons_drain_close();
    cons_test_echo_capture(false);
    cons_settle_mgr();
}

// Bounded drain: overflow drops OLDEST so the newest output (the prompt the
// user needs to see) survives; writers never block.
void test_cons_drain_overflow_drops_oldest(void) {
    cons_test_reset();
    cons_test_echo_capture(true);
    TEST_EXPECT_EQ(cons_drain_open(), 0, "drain arms");

    // Fill exactly + 8 more. The ring is 8192; feed 8200 'a's then a tail
    // marker so the drop-oldest is observable at the read side.
    u8 chunk[64];
    for (u32 i = 0; i < sizeof(chunk); i++) chunk[i] = (u8)'a';
    for (int k = 0; k < 128; k++)                        // 8192 'a's
        TEST_EXPECT_EQ(cons_output_write(chunk, sizeof(chunk)), (long)sizeof(chunk),
                       "fill chunk accepted");
    TEST_EXPECT_EQ(cons_test_drain_overflow(), 0u, "exactly-full: no drop yet");
    TEST_EXPECT_EQ(cons_output_write("XYZ", 3), 3L, "overflow write accepted (never blocks)");
    TEST_EXPECT_EQ(cons_test_drain_overflow(), 3u, "3 oldest bytes dropped");
    TEST_EXPECT_EQ(cons_test_drain_count(), 8192u, "count stays at capacity");

    // Drain fully: the LAST 3 bytes must be the newest ("XYZ").
    static u8 big[8192];
    long total = 0;
    while (total < 8192) {
        long r = cons_drain_read(big + total, 8192 - total);
        TEST_ASSERT(r > 0, "drain read progresses");
        total += r;
    }
    TEST_EXPECT_EQ(total, 8192L, "full capacity drained");
    TEST_ASSERT(big[8189]=='X' && big[8190]=='Y' && big[8191]=='Z',
                "newest bytes survived the drop-oldest");

    cons_drain_close();
    cons_test_echo_capture(false);
    cons_test_reset();
}

// Close semantics: disarm stops the tap; a fresh open starts a FRESH epoch
// (stale bytes discarded); a read attempt on a closed drain is refused.
void test_cons_drain_close_and_reopen_epoch(void) {
    cons_test_reset();
    cons_test_echo_capture(true);
    TEST_EXPECT_EQ(cons_drain_open(), 0, "drain arms");
    TEST_EXPECT_EQ(cons_output_write("old", 3), 3L, "bytes into epoch 1");
    cons_drain_close();

    u8 b[4];
    TEST_EXPECT_EQ(cons_drain_read(b, sizeof(b)), -1L, "read on a closed drain refused");
    TEST_EXPECT_EQ(cons_output_write("gone", 4), 4L, "closed: write still accepted");

    TEST_EXPECT_EQ(cons_drain_open(), 0, "re-open");
    TEST_EXPECT_EQ(cons_test_drain_count(), 0u, "fresh epoch: stale bytes discarded");
    cons_drain_close();
    cons_test_echo_capture(false);
    cons_test_reset();
}

// The drain's POLLIN readiness + the deferred-wake relay (the LS-8a second
// instance): a drain byte's empty->non-empty edge arms drain poll_wake_pending
// + wakes console_mgr; the hook is walked in process context only. A disarmed
// drain is POLLIN-ready (EOF is readable).
void test_cons_drain_poll_deferred_wake(void) {
    cons_test_reset();
    cons_test_echo_capture(true);
    sched();
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "console_mgr SLEEPING at entry");
    TEST_EXPECT_EQ(cons_drain_open(), 0, "drain arms");

    struct Rendez r; rendez_init(&r);
    struct poll_waiter pw; poll_waiter_init(&pw, &r);
    short rev = cons_drain_poll(POLLIN, &pw);
    TEST_EXPECT_EQ((int)(rev & POLLIN), 0, "empty drain: no POLLIN at register");
    TEST_ASSERT(!pw.ready, "hook not ready before any byte");

    cons_output_write("k", 1);
    TEST_ASSERT(cons_test_drain_pollwake_pending(), "byte armed the drain poll edge");
    TEST_ASSERT(!pw.ready, "producer did NOT walk the hook (deferred)");
    TEST_EXPECT_EQ(sched_runnable_count(), 1u, "console_mgr RUNNABLE post-byte");

    sched();
    TEST_ASSERT(pw.ready, "console_mgr's deferred walk set the drain hook ready");
    TEST_ASSERT(!cons_test_drain_pollwake_pending(), "drain poll edge consumed");
    TEST_ASSERT((cons_drain_poll(POLLIN, NULL) & POLLIN) != 0,
                "buffered drain byte -> POLLIN on re-sample");

    poll_waiter_list_unregister(&pw);

    // Disarmed -> POLLIN (EOF readable), so a parked poller never strands.
    cons_drain_close();
    TEST_ASSERT((cons_drain_poll(POLLIN, NULL) & POLLIN) != 0,
                "closed drain reads EOF -> POLLIN-ready");

    cons_test_echo_capture(false);
    cons_settle_mgr();
}

// ---------------------------------------------------------------------------
// cons.tx_role_serializes_writers (#75)
// ---------------------------------------------------------------------------
//
// THE #75 REGRESSION. cons_output_write must be atomic against other console
// writers (ARCH section 23.5.2): while one writer holds the TX writer role, a
// second must PARK rather than emit. Pre-P1-F the loop held no lock at all --
// cons_output_write walked byte-by-byte into a lock-free uart_putc -- so two
// CPUs interleaved at BYTE granularity, shredding multi-byte glyphs (the 3-byte
// U+22A2) and SGR escapes in 10 of 40 gate boots.
//
// The RING deliberately has no test of its own: every byte of console output on
// every boot flows through it, so a ring bug means no boot at all. The ROLE is
// what needs pinning -- its absence is SILENT until two CPUs happen to race.
//
// NON-VACUOUS: with cons_tx_role_acquire() removed from cons_output_write the
// writer emits immediately, so g_txr_ran reaches 2 and the capture buffer holds
// "BBBB" after the first sched() -- both asserts below fail.
//
// Echo capture is on so the writer's bytes land in an observable buffer instead
// of the real UART, letting the test assert on EXACTLY what was emitted and
// when. Capture short-circuits cons_emit_wait AFTER the role is taken, so the
// role -- the thing under test -- is exercised unchanged.

static volatile u32  g_txr_ran;
static volatile long g_txr_ret;
static volatile bool g_txr_exited;

static void txr_writer(void) {
    g_txr_ran++;
    g_txr_ret = cons_output_write("BBBB", 4);
    g_txr_ran++;
    test_kthread_park_terminal(&g_txr_exited);
}

void test_cons_tx_role_serializes_writers(void) {
    u8  got[16];
    u32 n;

    g_txr_ran = 0;
    g_txr_ret = -999;
    g_txr_exited = false;

    cons_test_echo_capture(true);

    // Hold the role from the test thread, standing in for a first writer that
    // is mid-call (a real one parks in the room-wait; the observable state is
    // identical -- the role is held).
    cons_test_tx_role_hold();
    TEST_ASSERT(cons_test_tx_role_held(), "role held before the contender runs");

    struct Thread *w = thread_create(kproc(), txr_writer);
    TEST_ASSERT(w != NULL, "thread_create");
    ready(w);

    // Let the writer run until it parks on the role. SMP placement means one
    // sched() does not guarantee it ran (the #77 lesson) -- wait on the
    // OBSERVABLE, bounded.
    for (int spins = 0; g_txr_ran < 1u && spins < 10000; spins++) sched();
    TEST_EXPECT_EQ(g_txr_ran, 1u, "contender entered cons_output_write");

    // THE PROPERTY: it must NOT have emitted anything while the role is held.
    for (int spins = 0; spins < 100; spins++) sched();
    TEST_EXPECT_EQ(g_txr_ran, 1u, "contender is PARKED on the role, not emitting");
    n = cons_test_echo_captured(got, sizeof got);
    TEST_EXPECT_EQ(n, 0u, "no byte of the contender's write reached the console");

    // Release: the contender must wake, complete, and emit its bytes CONTIGUOUSLY.
    cons_test_tx_role_drop();
    for (int spins = 0; g_txr_ran < 2u && spins < 10000; spins++) sched();
    TEST_EXPECT_EQ(g_txr_ran, 2u, "contender resumed after the role freed");
    TEST_EXPECT_EQ(g_txr_ret, 4L, "contender wrote all 4 bytes");

    n = cons_test_echo_captured(got, sizeof got);
    TEST_EXPECT_EQ(n, 4u, "exactly the contender's 4 bytes were emitted");
    TEST_ASSERT(got[0] == 'B' && got[1] == 'B' && got[2] == 'B' && got[3] == 'B',
                "the write landed contiguous and intact");

    cons_test_echo_capture(false);
    test_kthread_join_free(w, &g_txr_exited);
    cons_settle_mgr();
}
