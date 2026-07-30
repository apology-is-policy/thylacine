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
#include <thylacine/syscall.h>  // #55: struct t_stat + T_S_IFCHR (the qid contract)
#include <thylacine/thread.h>
#include <thylacine/types.h>

void test_cons_blocking_read_wakeup(void);
void test_cons_tx_role_serializes_writers(void);
void test_cons_tx_room_wait_and_deadline(void);
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

// #75-audit F2: the stalled-consumer emulator (arch/arm64/uart.c) + the clock
// the deadline assertion reads.
extern void uart_test_tx_stall(bool on);
extern u64  timer_now_ns(void);

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

// #89: the dance body returns an error string (NULL = ok) instead of asserting,
// so the caller can RELEASE AND REAP the consumer on EVERY exit -- the same #58
// structural rule the poll dance below already follows, applied to the kthread
// instead of the poll hook. A failing mid-dance TEST_ASSERT used to `return`
// straight past test_kthread_join_free, leaving the consumer PARKED inside
// devcons_read; the next test's cons_rx_input then woke that orphan, which
// drained the byte out from under it. Observed exactly so: this test failed on
// its SLEEPING assert, and cons.poll_deferred_wake (registered later) then
// failed "buffered data: POLLIN not ready on re-sample" in the SAME boot -- one
// leak, two red tests.
static const char *cons_blocking_read_dance(struct Thread *consumer) {
    // SMP (#20/#28 spin-until): ready() may place the consumer on a PEER CPU, so
    // ONE sched() does not guarantee it has both run AND completed its park --
    // the observed failure had ran == 1 (it ran) with state != SLEEPING (not yet
    // parked). Bounded, so a real lost-park regression still FAILS.
    { int spins = 0;
      while ((g_cbr_ran < 1 || consumer->state != THREAD_SLEEPING) && spins++ < 100000)
          sched(); }
    if (g_cbr_ran != 1)                     return "consumer ran + parked in devcons_read";
    if (consumer->state != THREAD_SLEEPING) return "consumer SLEEPING inside devcons_read";

    // Producer: feed one byte. cons_rx_input enqueues it + wakeup()s the data
    // Rendez. The invariant is NO LOST WAKEUP -- i.e. no longer SLEEPING. Do not
    // pin RUNNABLE exactly: a peer CPU may already have dispatched it to RUNNING,
    // which is a scheduling transient, not a correctness difference (#28's
    // relax-to-the-real-invariant rule).
    cons_rx_input((u8)'q', false);
    if (consumer->state == THREAD_SLEEPING)
        return "consumer still SLEEPING after cons_rx_input (lost wakeup)";

    // Consumer resumes inside devcons_read, drains 'q', returns 1, parks.
    { int spins = 0; while (g_cbr_ran < 2 && spins++ < 100000) sched(); }
    if (g_cbr_ran != 2)                 return "consumer resumed post-wake";
    if (g_cbr_ret != 1L)                return "devcons_read returned exactly 1 byte";
    if ((long)g_cbr_byte != (long)'q')  return "the woken read returned 'q'";
    return NULL;
}

void test_cons_blocking_read_wakeup(void) {
    cons_test_reset();
    g_cbr_ran = 0; g_cbr_ret = -1; g_cbr_byte = -1; g_cbr_exited = false;
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty at test entry");

    struct Thread *consumer = thread_create(kproc(), cbr_consumer_entry);
    TEST_ASSERT(consumer != NULL, "thread_create(consumer)");
    ready(consumer);

    const char *err = cons_blocking_read_dance(consumer);

    // Release BEFORE joining: test_kthread_join_free spins UNBOUNDED on the
    // terminal-park flag, so joining a consumer still parked in devcons_read
    // would hang the boot. A byte unblocks it; the read's outcome no longer
    // matters (the verdict is already latched in `err`), and cons_test_reset
    // clears whatever is left in the ring.
    if (g_cbr_ran < 2) cons_rx_input((u8)'z', false);
    test_kthread_join_free(consumer, &g_cbr_exited);
    cons_test_reset();

    TEST_ASSERT(err == NULL, err ? err : "cons_blocking_read_dance");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty after consumer freed");
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
// The dance body returns an error string (NULL = ok) instead of asserting, so
// the caller can unregister the stack hook on EVERY exit -- the #58 structural
// rule: a failing mid-dance assert's early return LEAKED the registered hook,
// and the next walk extincted on the reused frame's clobbered magic
// (EXTINCTION: pw_wake). The mgr is HELD by the caller across the produce/
// assert legs, so a peer-CPU dispatch cannot consume the pending flag early
// (the race that fired the leaking assert, ~1-in-50 HVF boots); the helper
// releases the hold itself when the dance needs the walk.
static const char *cons_poll_dance(struct poll_waiter *pw) {
    // Poller side: register a hook via cons_poll on the empty ring (register-
    // then-observe). No POLLIN yet; the hook is not ready.
    short rev = cons_poll(POLLIN, pw);
    if ((rev & POLLIN) != 0) return "empty cons: POLLIN at register";
    if (pw->ready) return "poll_waiter ready before any data";

    // Producer (IRQ side): a data byte on the empty->non-empty edge arms
    // poll_wake_pending + wakes console_mgr -- but does NOT walk the hook list
    // (deferred). Deterministic under the hold: the mgr cannot consume yet.
    cons_rx_input((u8)'p', false);
    if (!cons_test_pollwake_pending()) return "data byte did not arm poll_wake_pending";
    if (pw->ready) return "the IRQ producer walked the poll hook (must defer)";

    // Release: console_mgr services -> drains poll_wake_pending ->
    // poll_waiter_list_wake walks the list -> pw->ready. The mgr may run on a
    // PEER CPU; the bounded sched() loop just yields until the walk lands.
    cons_test_mgr_hold(false);
    for (int i = 0; i < 100000 && !pw->ready; i++) sched();
    if (!pw->ready) return "console_mgr's deferred walk never set the poll hook ready";
    if (cons_test_pollwake_pending()) return "poll_wake_pending not consumed";

    // The ring now holds the byte -> POLLIN ready on re-sample.
    if ((cons_poll(POLLIN, NULL) & POLLIN) == 0)
        return "buffered data: POLLIN not ready on re-sample";
    return NULL;
}

void test_cons_poll_deferred_wake(void) {
    cons_test_mgr_hold(true);           // deterministic dance (#58)
    cons_test_reset();

    struct Rendez r; rendez_init(&r);
    struct poll_waiter pw; poll_waiter_init(&pw, &r);
    const char *err = cons_poll_dance(&pw);
    poll_waiter_list_unregister(&pw);   // NoStaleHook on EVERY path (#58)
    cons_test_mgr_hold(false);          // idempotent (the err path may still hold)
    TEST_ASSERT(err == NULL, err ? err : "unreachable");
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

    TEST_EXPECT_EQ(cons_set_mode_cmd("+echo", 5, true), 5L, "+echo accepted");
    TEST_EXPECT_EQ((long)cons_test_termios(), (long)(CONS_ISIG | CONS_ECHO), "+echo set ECHO");

    TEST_EXPECT_EQ(cons_set_mode_cmd("-isig", 5, true), 5L, "-isig accepted");
    TEST_EXPECT_EQ((long)cons_test_termios(), (long)CONS_ECHO, "-isig cleared ISIG");

    TEST_EXPECT_EQ(cons_set_mode_cmd("+icanon +echo", 13, true), 13L, "two tokens accepted");
    TEST_EXPECT_EQ((long)cons_test_termios(), (long)(CONS_ICANON | CONS_ECHO),
                   "atomic multi-flag set");

    // Malformed commands reject (-1) and leave the mode unchanged.
    u32 before = cons_test_termios();
    TEST_EXPECT_EQ(cons_set_mode_cmd("+bogus", 6, true), -1L, "unknown name -> -1");
    TEST_EXPECT_EQ(cons_set_mode_cmd("echo", 4, true), -1L, "missing +/- sign -> -1");
    TEST_EXPECT_EQ(cons_set_mode_cmd("+", 1, true), -1L, "empty name -> -1");
    TEST_EXPECT_EQ(cons_set_mode_cmd("", 0, true), -1L, "empty command -> -1");
    TEST_EXPECT_EQ(cons_set_mode_cmd("+echo +bad", 10, true), -1L, "one bad token rejects the batch");
    TEST_EXPECT_EQ((long)cons_test_termios(), (long)before,
                   "a rejected command leaves the mode unchanged");
    cons_test_reset();
}

// LS-8b: the /dev/consctl read-back render. Symmetric grammar with the write:
// five "+name"/"-name" tokens, then (#55) the winsize, one line -- the ptyfs
// ctl_render shape ("-icanon ... -onlcr winsize 0 0\n").
void test_cons_consctl_render(void) {
    cons_test_reset();                                 // default = ISIG only, ws 0x0
    char buf[64];
    long n = cons_render_mode(buf, (long)sizeof(buf));
    const char *want_default = "-icanon -echo +isig -icrnl -onlcr winsize 0 0\n";
    TEST_EXPECT_EQ(n, 46L, "default render length");
    bool ok = (n == 46);
    for (long i = 0; ok && i < n; i++) if (buf[i] != want_default[i]) ok = false;
    TEST_ASSERT(ok, "default renders flags + winsize 0 0");

    cons_test_set_termios(CONS_TERMIOS_ALL);
    n = cons_render_mode(buf, (long)sizeof(buf));
    const char *want_all = "+icanon +echo +isig +icrnl +onlcr winsize 0 0\n";
    ok = (n == 46);
    for (long i = 0; ok && i < n; i++) if (buf[i] != want_all[i]) ok = false;
    TEST_ASSERT(ok, "all-set renders every flag with '+'");

    // A too-small buffer renders nothing (never a partial line).
    TEST_EXPECT_EQ(cons_render_mode(buf, 10), 0L, "too-small buffer -> 0");
    TEST_EXPECT_EQ(cons_render_mode(buf, 40), 0L, "no room for the winsize tail -> 0");
    cons_test_reset();
}

// #55: the winsize round-trip -- the consctl verb sets it; the mode render,
// the standalone leaf render, and the snapshot all agree; malformed winsize
// tokens reject the WHOLE write (the tcsetattr-atomic seam extends to the
// new verb).
void test_cons_winsize_roundtrip(void) {
    cons_test_reset();

    u16 wc = 1, wr = 1;
    cons_winsize_get(&wc, &wr);
    TEST_ASSERT(wc == 0 && wr == 0, "reset -> winsize unset (0x0)");

    TEST_EXPECT_EQ(cons_set_mode_cmd("winsize 132 50", 14, true), 14L, "winsize verb accepted");
    cons_winsize_get(&wc, &wr);
    TEST_ASSERT(wc == 132 && wr == 50, "snapshot reads 132x50");

    char buf[64];
    long n = cons_render_mode(buf, (long)sizeof(buf));
    const char *want = "-icanon -echo +isig -icrnl -onlcr winsize 132 50\n";
    bool ok = (n == 49);
    for (long i = 0; ok && i < n; i++) if (buf[i] != want[i]) ok = false;
    TEST_ASSERT(ok, "mode render carries winsize 132 50");

    n = cons_render_winsize(buf, (long)sizeof(buf));
    const char *leaf = "winsize 132 50\n";
    ok = (n == 15);
    for (long i = 0; ok && i < n; i++) if (buf[i] != leaf[i]) ok = false;
    TEST_ASSERT(ok, "leaf render is winsize 132 50");
    // #55 audit F4: the floor is the fixed 20-byte MAX ("winsize 65535 65535\n"),
    // not content-dependent -- n=20 renders, n=19 returns 0 (conservative-safe).
    TEST_EXPECT_EQ(cons_render_winsize(buf, 20) > 0 ? 1L : 0L, 1L, "leaf: n=20 renders");
    TEST_EXPECT_EQ(cons_render_winsize(buf, 19), 0L, "leaf: n<20 -> 0 (max-reserve floor)");

    // A mixed write applies flags + winsize atomically.
    TEST_EXPECT_EQ(cons_set_mode_cmd("+echo winsize 80 24", 19, true), 19L, "mixed write accepted");
    TEST_ASSERT((cons_test_termios() & CONS_ECHO) != 0u, "mixed write set ECHO");
    cons_winsize_get(&wc, &wr);
    TEST_ASSERT(wc == 80 && wr == 24, "mixed write set 80x24");

    // Malformed winsize rejects the WHOLE write (flags too -- atomic).
    u32 before = cons_test_termios();
    TEST_EXPECT_EQ(cons_set_mode_cmd("winsize 80", 10, true), -1L, "missing rows -> -1");
    TEST_EXPECT_EQ(cons_set_mode_cmd("winsize a b", 11, true), -1L, "non-digit -> -1");
    TEST_EXPECT_EQ(cons_set_mode_cmd("winsize 70000 1", 15, true), -1L, "cols > 65535 -> -1");
    TEST_EXPECT_EQ(cons_set_mode_cmd("-echo winsize 9", 15, true), -1L,
                   "a bad winsize rejects the batch");
    TEST_ASSERT(cons_test_termios() == before, "rejected batch left the flags alone");
    cons_winsize_get(&wc, &wr);
    TEST_ASSERT(wc == 80 && wr == 24, "rejected batch left the winsize alone");

    // "winsizeX" is NOT the verb (the token must end at whitespace/EOL).
    TEST_EXPECT_EQ(cons_set_mode_cmd("winsizeX 1 2", 12, true), -1L, "winsizeX -> -1");
    cons_test_reset();
}

// #55: iff-changed -- a CHANGED apply advances winch_events (one tty:winch
// post attempt each); an unchanged rewrite must NOT (a repeat-post storm
// would be a notes-queue DoS on the owner's pgrp). The pgrp fan itself is
// notes_post_pgrp (PTY-1e, separately covered); with no console owner at
// test time the post is a structural no-op, so the counter is the witness.
void test_cons_winsize_winch_iff_changed(void) {
    cons_test_reset();
    TEST_EXPECT_EQ((long)cons_winch_events(), 0L, "reset -> 0 winch events");

    TEST_EXPECT_EQ(cons_set_mode_cmd("winsize 100 40", 14, true), 14L, "set 100x40");
    TEST_EXPECT_EQ((long)cons_winch_events(), 1L, "first set -> 1 event");

    TEST_EXPECT_EQ(cons_set_mode_cmd("winsize 100 40", 14, true), 14L, "rewrite 100x40");
    TEST_EXPECT_EQ((long)cons_winch_events(), 1L, "unchanged rewrite -> NO new event");

    TEST_EXPECT_EQ(cons_set_mode_cmd("winsize 100 41", 14, true), 14L, "set 100x41");
    TEST_EXPECT_EQ((long)cons_winch_events(), 2L, "changed rows -> 2nd event");

    // A flags-only write never touches the winsize (no event).
    TEST_EXPECT_EQ(cons_set_mode_cmd("+echo", 5, true), 5L, "flags-only write");
    TEST_EXPECT_EQ((long)cons_winch_events(), 2L, "flags-only -> no event");
    cons_test_reset();
}

// #55: the is-a-cons qid contract (ARCH 23.5.3) on the devcons vtable -- the
// SYS_CONSOLE_OPEN / std-fd chain's Dev. S_IFCHR posture + the bit-41 marker
// (disjoint from ptyfs's PTS_FLAG bit 40) + SYSTEM-owned + zero-fill (I-13:
// the pad bytes must cross as defined zeroes).
void test_cons_stat_native_qid_contract(void) {
    struct Spoor *cs = devcons.attach(NULL);
    TEST_ASSERT(cs != NULL, "devcons attach");

    struct t_stat st;
    for (size_t i = 0; i < sizeof(st); i++) ((u8 *)&st)[i] = 0xAA;  // poison
    TEST_EXPECT_EQ((long)devcons.stat_native(cs, &st), 0L, "stat_native fills");

    TEST_ASSERT((st.mode & T_S_IFMT) == T_S_IFCHR, "mode is S_IFCHR");
    TEST_ASSERT((st.mode & 0777u) == 0620u, "perm bits 0620");
    TEST_ASSERT((st.qid_path & CONS_STAT_QID_FLAG) != 0u, "bit-41 CONS marker set");
    TEST_ASSERT((st.qid_path & (1ULL << 40)) == 0u, "bit 40 (PTS_FLAG) clear");
    TEST_ASSERT(st.uid == PRINCIPAL_SYSTEM && st.gid == GID_SYSTEM, "SYSTEM-owned");
    TEST_ASSERT(st.qid_type == QTFILE, "qid_type QTFILE");
    TEST_ASSERT(st.size == 0 && st.nlink == 1, "size 0, nlink 1");
    // I-13: the poisoned pad bytes were overwritten by the zero-fill.
    TEST_ASSERT(st._pad_qid[0] == 0 && st._pad_qid[1] == 0 && st._pad_qid[2] == 0,
                "qid pad zero-filled");
    TEST_ASSERT(st._pad_blksize == 0 && st._pad_dev == 0, "tail pads zero-filled");

    spoor_unref(cs);
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
    TEST_EXPECT_EQ(cons_set_mode_cmd("+echo", 5, true), 5L, "consctl +echo accepted");

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
// The canonical-mode twin of cons_poll_dance -- the same #58 shape (held mgr +
// error-string returns + unregister-on-every-exit in the caller): its
// post-Enter pending/!ready asserts carried the identical peer-CPU-dispatch
// race, and any mid-dance failure leaked the hook.
static const char *cons_cook_edge_dance(struct poll_waiter *pw) {
    short rev = cons_poll(POLLIN, pw);
    if ((rev & POLLIN) != 0) return "empty cons: POLLIN at register";
    if (pw->ready) return "poll_waiter ready before any line";

    // Buffer "hi": canonical mode holds it in line[]; the ring stays EMPTY, so
    // there is NO poll edge and console_mgr is NOT woken (the bytes have not
    // entered the ring -- POSIX canonical: a poller waits for a full line).
    cons_rx_input((u8)'h', false);
    cons_rx_input((u8)'i', false);
    if (cons_test_pollwake_pending()) return "buffered chars armed a ring edge";
    if (pw->ready) return "poll wake fired while the line was still assembling";

    // Enter: the whole line ("hi" + NL) flushes to the ring in ONE call, arming
    // the empty->non-empty edge once + waking console_mgr (deferred -- the IRQ
    // producer does NOT walk the hook). Deterministic under the hold.
    cons_rx_input((u8)'\n', false);
    if (!cons_test_pollwake_pending()) return "Enter did not arm the poll edge";
    if (pw->ready) return "the IRQ producer walked the hook (must defer)";

    cons_test_mgr_hold(false);
    for (int i = 0; i < 100000 && !pw->ready; i++) sched();
    if (!pw->ready) return "console_mgr's deferred walk never set the hook ready";
    if (cons_test_pollwake_pending()) return "poll_wake_pending not consumed";

    // The ring holds the whole delivered line.
    u8 buf[8];
    long n = cons_drain(buf, sizeof(buf));
    if (n != 3L) return "the ring does not hold the delivered line hi+NL";
    if (!(buf[0]=='h' && buf[1]=='i' && buf[2]=='\n')) return "line is not hi\\n";
    return NULL;
}

void test_cons_cook_canonical_poll_edge(void) {
    cons_test_mgr_hold(true);           // deterministic dance (#58)
    cons_test_reset();
    cons_test_set_termios(CONS_ICANON | CONS_ISIG);     // cooked, no echo

    struct Rendez r; rendez_init(&r);
    struct poll_waiter pw; poll_waiter_init(&pw, &r);
    const char *err = cons_cook_edge_dance(&pw);
    poll_waiter_list_unregister(&pw);   // NoStaleHook on EVERY path (#58)
    cons_test_mgr_hold(false);          // idempotent (the err path may still hold)
    TEST_ASSERT(err == NULL, err ? err : "unreachable");
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
// The drain twin of cons_poll_dance -- same #58 shape: error-string returns so
// the caller unregisters on EVERY exit; the mgr held across the produce/assert
// legs (THIS test's "byte armed the drain poll edge" assert is the one a
// peer-CPU mgr dispatch raced on 2026-07-21 -- the failing assert's early
// return leaked the hook on the DRAIN list, and devdev.renderer_gate's later
// walk extincted on the reused frame: EXTINCTION: pw_wake).
static const char *cons_drain_dance(struct poll_waiter *pw) {
    short rev = cons_drain_poll(POLLIN, pw);
    if ((rev & POLLIN) != 0) return "empty drain: POLLIN at register";
    if (pw->ready) return "hook ready before any byte";

    cons_output_write("k", 1);
    if (!cons_test_drain_pollwake_pending()) return "byte did not arm the drain poll edge";
    if (pw->ready) return "the producer walked the drain hook (must defer)";

    cons_test_mgr_hold(false);
    for (int i = 0; i < 100000 && !pw->ready; i++) sched();
    if (!pw->ready) return "console_mgr's deferred walk never set the drain hook ready";
    if (cons_test_drain_pollwake_pending()) return "the drain poll edge was not consumed";
    if ((cons_drain_poll(POLLIN, NULL) & POLLIN) == 0)
        return "buffered drain byte: POLLIN not ready on re-sample";
    return NULL;
}

void test_cons_drain_poll_deferred_wake(void) {
    cons_test_reset();
    cons_test_echo_capture(true);
    // The open-assert precedes the hold + the hook: a failure here latches
    // nothing and leaks nothing.
    TEST_EXPECT_EQ(cons_drain_open(), 0, "drain arms");
    cons_test_mgr_hold(true);           // deterministic dance (#58)

    struct Rendez r; rendez_init(&r);
    struct poll_waiter pw; poll_waiter_init(&pw, &r);
    const char *err = cons_drain_dance(&pw);
    poll_waiter_list_unregister(&pw);   // NoStaleHook on EVERY path (#58)
    cons_test_mgr_hold(false);          // idempotent (the err path may still hold)
    TEST_ASSERT(err == NULL, err ? err : "unreachable");

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

// ---------------------------------------------------------------------------
// cons.tx_room_wait_and_deadline (#75-audit F2)
// ---------------------------------------------------------------------------
//
// THE OWED TEST from the #75 close. tx_role_serializes_writers (above) covers
// the writer role, but it runs under echo capture -- which short-circuits
// cons_emit_wait BEFORE cons_tx_push_nowait -- so it never reaches the ring.
// That left the ring's two wait/wake legs with no deterministic coverage:
//
//   (A) the #67 DEADLINE. A stalled host consumer must yield a bounded SHORT
//       WRITE. This is the anti-wedge property of the trusted-path console: if
//       it regressed, a paused terminal would HANG every console writer instead
//       of dropping bytes -- and a hang here takes the console with it, so the
//       failure would be silent.
//   (B) the ROOM-WAIT I-9 wake. A writer parked on a full ring must resume when
//       the drain frees a slot. A lost wake strands it for the whole deadline,
//       and without (A) forever.
//
// uart_test_tx_stall emulates the stall (the FIFO never accepts AND TXIM arming
// is gated -- a software-only stall against an EMPTY hardware FIFO would leave
// the TX line asserted against a drain that can never progress: an IRQ storm).
// The harness prints via uart_puts (direct), which the stall leaves alone, so
// test output still reaches the console across the window.
//
// The filler is DISCARDED, never flushed. A test that dumped a ring's worth of
// padding into the boot log to prove a point would be manufacturing the next
// harness-blindness bug (#74/#85/#87) in order to test this one.
//
// NON-VACUOUS against PRODUCTION code (revert probes recorded in the commit):
//   (A) make cons_emit_wait give up on a full ring without tsleeping at all
//       (an instant drop) -> the elapsed assertion fails. Removing the
//       TSLEEP_TIMEDOUT arm outright instead hangs the boot here, which is the
//       same property observed the other way round.
//   (B) drop the `if (freed) wakeup(...)` from cons_tx_drain_from_irq -> the
//       parked writer is never woken, times out, and returns 0 instead of 1.

#define TXW_FILL_MAX 9216u
static u8 g_txw_fill[TXW_FILL_MAX];

static volatile u32  g_txw_ran;
static volatile long g_txw_ret;
static volatile bool g_txw_exited;

static void txw_writer(void) {
    g_txw_ran++;
    g_txw_ret = cons_output_write(" ", 1);   // SPACE: see the filler note below
    g_txw_ran++;
    test_kthread_park_terminal(&g_txw_exited);
}

void test_cons_tx_room_wait_and_deadline(void) {
    const u32 cap      = cons_test_tx_ring_capacity();
    const u64 deadline = cons_test_tx_room_wait_ns();

    TEST_ASSERT(cap + 64u <= TXW_FILL_MAX,
                "filler must exceed the ring -- grow TXW_FILL_MAX");
    // Asserted, not assumed: cons_tx_arm() runs at boot_main before
    // test_run_all(). Disarmed, every push takes the direct path and succeeds,
    // so the ring could never fill and BOTH legs below would pass vacuously.
    TEST_ASSERT(cons_test_tx_armed(),
                "TX ring must be armed or this test is vacuous");

    // SPACE deliberately, for two reasons. No NL, so ONLCR does not expand and
    // one input byte is exactly one ring byte -- that is what makes the counts
    // below exact. And part (B) genuinely drains a couple of bytes to the wire
    // (that is the point: the wake must come from the real drain), so whatever
    // survives lands in the boot log next to this test's own PASS line. Spaces
    // are invisible there; '.' produced a "..PASS" that reads like corruption
    // in a log whose trustworthiness is the whole reason this test exists.
    for (u32 i = 0; i < TXW_FILL_MAX; i++) g_txw_fill[i] = (u8)' ';

    cons_test_tx_ring_free(cap, true);
    TEST_EXPECT_EQ(cons_test_tx_ring_count(), 0u, "ring empty before the test");

    // --- (A) the #67 deadline: a stalled consumer yields a SHORT write -------
    u32 dropped0 = cons_test_tx_dropped();
    uart_test_tx_stall(true);
    u64 t0      = timer_now_ns();
    long ret    = cons_output_write(g_txw_fill, (long)(cap + 64u));
    u64 elapsed = timer_now_ns() - t0;
    uart_test_tx_stall(false);

    // Sample and DISCARD before asserting, for the same reason part (B) does:
    // TEST_ASSERT returns on failure, and a return with a ring's worth of filler
    // still queued would let the next console write flush thousands of stray
    // bytes into the boot log. Clean up first, judge second.
    u32 a_dropped = cons_test_tx_dropped();
    u32 a_cnt     = cons_test_tx_ring_count();
    cons_test_tx_ring_free(cap, true);
    u32 a_cnt_end = cons_test_tx_ring_count();

    // Exact, not a range. In the kernel test phase there is provably no other
    // console writer -- EL0 does not exist yet, and echo needs RX at a prompt
    // that has not been printed. If this ever misses, something DID write to
    // the console during the test phase and that is worth knowing.
    TEST_EXPECT_EQ((u32)ret, cap,
                   "short write: exactly a ring's worth accepted, the rest dropped");
    TEST_ASSERT(ret < (long)(cap + 64u), "the write did NOT complete in full");
    TEST_ASSERT(elapsed >= deadline,
                "the writer PARKED for the deadline (not an instant drop)");
    TEST_ASSERT(a_dropped > dropped0, "the deadline drop was counted");
    TEST_EXPECT_EQ(a_cnt, cap, "ring holds the accepted bytes");
    TEST_EXPECT_EQ(a_cnt_end, 0u,
                   "filler discarded -- no test padding reached the console");

    // --- (B) the room-wait I-9 wake -----------------------------------------
    g_txw_ran = 0;
    g_txw_ret = -999;
    g_txw_exited = false;

    // Part (A) asserted only AFTER its unstall, and part (B) does the same --
    // it records into locals across the stalled window and asserts once the
    // stall is lifted. This is deliberate: TEST_ASSERT returns from the test on
    // failure, so an assertion INSIDE the window would leave the console TX ring
    // permanently stalled, and every later test that writes real console bytes
    // would then block 20 ms per byte against a ring that can never drain --
    // turning a clean FAIL into a mystery timeout. A test's failure mode must
    // not destroy the diagnosis (#74/#85/#87 are all that bug in other clothes).
    uart_test_tx_stall(true);

    // Fill to exactly full from this thread: every push fits, so this never
    // parks and the role is released before the contender starts.
    long b_filled   = cons_output_write(g_txw_fill, (long)cap);
    u32  b_cnt_full = cons_test_tx_ring_count();
    u32  waits0     = cons_test_tx_room_waits();

    struct Thread *w = thread_create(kproc(), txw_writer);
    u32 b_waits = waits0, b_ran_parked = 0, b_cnt_parked = 0;
    u32 b_freed = 0, b_cnt_left = 0, b_ran_silent = 0;
    if (w != NULL) {
        ready(w);
        // Wait for the PARK ITSELF, not for a proxy. Inferring the park from
        // "it ran and has not finished" would be both timing-dependent and
        // vacuous if we freed a slot before the writer reached the wait. The
        // loop exits the moment it parks, keeping us well inside the deadline.
        for (int spins = 0;
             cons_test_tx_room_waits() == waits0 && g_txw_ran < 2u && spins < 10000;
             spins++)
            sched();
        b_waits      = cons_test_tx_room_waits();
        b_ran_parked = g_txw_ran;
        b_cnt_parked = cons_test_tx_ring_count();

        // Now make the wake come from PRODUCTION, not from this test. Silently
        // discard all but two bytes (wake=false, so the writer stays parked
        // even though the ring has room -- tsleep re-checks its cond only on a
        // wake); the real cons_tx_drain_from_irq below then moves those two and
        // delivers the wake under test. Two bytes is also all the filler that
        // ever reaches the console from this test.
        b_freed      = cons_test_tx_ring_free(cap - 2u, false);
        b_cnt_left   = cons_test_tx_ring_count();
        b_ran_silent = g_txw_ran;
    }

    uart_test_tx_stall(false);

    // The drain runs before the assertions so the parked writer always gets its
    // wake and completes -- otherwise a failing assertion here would strand it
    // and the join below would have nothing to join.
    u32  b_ran_done = 0;
    long b_ret      = -999;
    if (w != NULL) {
        cons_tx_drain_from_irq();
        for (int spins = 0; g_txw_ran < 2u && spins < 10000; spins++) sched();
        b_ran_done = g_txw_ran;
        b_ret      = g_txw_ret;
    }
    cons_test_tx_ring_free(cap, true);
    u32 b_cnt_end = cons_test_tx_ring_count();

    TEST_ASSERT(w != NULL, "thread_create");
    TEST_EXPECT_EQ((u32)b_filled, cap, "ring filled to capacity without parking");
    TEST_EXPECT_EQ(b_cnt_full, cap, "ring full");
    TEST_ASSERT(b_waits > waits0, "the contender PARKED on the room-wait");
    TEST_EXPECT_EQ(b_ran_parked, 1u, "parked writer has not completed");
    TEST_EXPECT_EQ(b_cnt_parked, cap, "ring still full while parked");
    TEST_EXPECT_EQ(b_freed, cap - 2u, "silently freed all but two bytes");
    TEST_EXPECT_EQ(b_cnt_left, 2u, "two bytes left to drain");
    TEST_EXPECT_EQ(b_ran_silent, 1u, "a silent free does NOT wake the writer");
    TEST_EXPECT_EQ(b_ran_done, 2u, "writer woke and completed");
    TEST_EXPECT_EQ(b_ret, 1L,
                   "the parked byte was written -- a LOST wake in "
                   "cons_tx_drain_from_irq would instead time out and return 0");
    TEST_EXPECT_EQ(b_cnt_end, 0u, "filler discarded");

    test_kthread_join_free(w, &g_txw_exited);
    cons_settle_mgr();
}

// ---------------------------------------------------------------------------
// cons.sys_puts_uses_shared_console_path (#76)
// ---------------------------------------------------------------------------
//
// THE #76 REGRESSION. SYS_PUTS must emit through cons_output_write -- the ONE
// console-output implementation (#57b) -- and not through a uart_putc loop of
// its own.
//
// Pre-fix, sys_puts_handler walked the user buffer byte-by-byte into the
// LOCK-FREE uart_putc: the pre-P1-F shape, left behind in this caller when
// P1-F converted cons_output_write. That cost two properties at once:
//
//   - the WRITER ROLE, so a t_putstr shredded a concurrent /dev/cons write at
//     byte granularity. Observed live in LS-CI: a login prompt emerged as
//     `patapestrssyd: mworodd:e`, which is "password: " (login, via fd 1)
//     interleaved byte-for-byte with "tapestryd: mode " (tapestryd, via
//     t_putstr). A role only some writers take excludes nobody.
//   - the DRAIN TAP, which fires from cons_emit / cons_emit_wait only, so
//     nothing written via SYS_PUTS ever reached the G-4 renderer: the native
//     diagnostic stream was invisible on the graphical console while looking
//     perfectly normal on serial.
//
// The DRAIN is what makes this deterministically testable. The role's absence
// shows up only under a race, but the tap's absence is visible from a single
// thread -- and both properties live behind the SAME call, so proving SYS_PUTS
// reaches the drain proves it took the shared path and the role comes with it.
//
// It has to run at EL0. sys_puts_handler takes a USER VA, and kproc has no
// user address space (pgtable_root == 0), so no in-kernel caller can reach it
// at all -- a unit test of the handler is not merely awkward, it is
// impossible. /hello is the vehicle: an established spawn-and-reap binary
// (sys_spawn_with_fds.zero_count_succeeds) whose entire output is one
// t_putstr, so every byte the drain sees came through SYS_PUTS.
//
// Echo capture stays OFF deliberately -- the bytes take the real ring and the
// real UART, so this exercises the production path end to end rather than the
// short-circuit, and the extra serial line is the same one the spawn test
// already prints.
//
// NON-VACUOUS: restore the uart_putc loop in sys_puts_handler and the drain
// captures nothing -- `have` is 0 and the assert below fails.
//
// The count is sampled with the NON-BLOCKING cons_test_drain_count() before
// any cons_drain_read(). cons_drain_read SLEEPS on an armed-but-empty drain,
// so reading first would turn a pre-fix run into a BOOT HANG instead of a
// failed test -- and a hang reports nothing.

extern int sys_spawn_with_fds_for_proc(struct Proc *p, const char *name,
                                       size_t name_len,
                                       const u32 *fds, u32 fd_count);

void test_cons_sys_puts_uses_shared_console_path(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    int st = 0;
    while (wait_pid(&st) > 0) { /* drain stragglers */ }

    cons_test_reset();
    TEST_EXPECT_EQ(cons_drain_open(), 0, "drain arms");

    int pid = sys_spawn_with_fds_for_proc(t->proc, "hello", 5, NULL, 0);
    int status = -1;
    int reaped = (pid > 0) ? wait_pid(&status) : -1;

    // Sample + DISARM before asserting. TEST_ASSERT returns on failure, and a
    // drain left open makes every later drain test fail with a single-open -1
    // instead of its own verdict -- one broken test reporting as three.
    u32  have = cons_test_drain_count();
    u8   dbuf[128];
    long dn = (have > 0u) ? cons_drain_read(dbuf, (long)sizeof dbuf) : 0;
    cons_drain_close();
    cons_test_reset();

    TEST_ASSERT(pid > 0, "spawned /hello");
    TEST_EXPECT_EQ(reaped, pid, "reaped /hello");
    TEST_EXPECT_EQ(status, 0, "/hello exited 0");

    // THE PROPERTY: the child's t_putstr reached the drain, so SYS_PUTS went
    // through cons_output_write (role-held, tapped) rather than uart_putc.
    TEST_ASSERT(have > 0u, "SYS_PUTS output reached the drain tap");
    TEST_ASSERT(dn > 0, "drain read returned the tapped bytes");

    // Search rather than assert at offset 0. The property is "the child's bytes
    // reached the tap", NOT "nothing else wrote to the console" -- pinning the
    // match to offset 0 would couple this test to the second, and a future cons
    // write anywhere in the spawn path would then fail it with a message
    // blaming SYS_PUTS. Finding the marker anywhere still proves the routing.
    static const u8 want[] = { 'h','e','l','l','o',' ','f','r','o','m' };
    bool found = false;
    for (long i = 0; !found && i + (long)sizeof(want) <= dn; i++) {
        u32 k = 0;
        while (k < sizeof(want) && dbuf[i + (long)k] == want[k]) k++;
        found = (k == sizeof(want));
    }
    TEST_ASSERT(found, "drain holds /hello's SYS_PUTS bytes verbatim");
}
