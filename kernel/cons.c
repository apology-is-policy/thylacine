// /dev/cons — kernel console Dev backed by the PL011 UART (P4-B).
//
// Per ARCHITECTURE.md §9.4 + ROADMAP §6.1. v1.0 P4-B lands the
// write-side: writes go through `uart_putc` to the kernel UART. Reads
// return 0 (EOF) at v1.0 — UART RX is wired in a later sub-chunk
// (Phase 4+ when the IRQ-driven input path with a Rendez block lands).
//
// Single-file leaf Dev with dc='c'. Plan 9 conventionally pairs cons
// with consctl (mode control); consctl is held until the Phase 5 PTY +
// termios surface lands.

#include <thylacine/cons.h>
#include <thylacine/dev.h>
#include <thylacine/poll.h>                  // LS-8a: pollable cons (deferred poll-wake)
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>                 // RW-11 SA-1b: sched_mark_interactive
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/thread.h>                // RW-11 SA-1b: current_thread
#include <thylacine/types.h>

#include "../arch/arm64/uart.h"

// =============================================================================
// A-4c-1: kernel UART console RX (the first kernel console INPUT path).
// =============================================================================
//
// uart_rx_handler (IRQ context) calls cons_rx_input per received byte. Data
// bytes fill g_cons.ring; devcons_read drains it, blocking on
// g_cons_data_rendez when empty. Ctrl-C (0x03) is cooked-consumed -> the
// console_mgr kthread posts the `interrupt` note to the console owner. A serial
// BREAK is the A-4c-2 SAK; A-4c-1 discards it.
//
// IRQ-safety (IDENTITY-DESIGN.md section 9.8 "As-built"): cons_rx_input runs in
// IRQ context, so it does ONLY ring + flag mutation (under an irqsave lock) +
// wakeup() -- the SOLE IRQ-safe wake (notes_post + poll_waiter_list_wake take
// plain spin_locks). The privileged/blocking work runs in console_mgr's process
// context. The data wait is a single Rendez + a single-reader busy-guard:
// poll_waiter_list_wake is not IRQ-safe, and a single-waiter Rendez extincts on
// a second sleeper, so a 2nd concurrent blocking read returns -1 rather than
// racing into that extinction (the console is a single-reader resource at v1.0;
// a multi-reader lift is v1.x).

#define CONS_RING_SIZE  256u   // power of two (mask-indexed); bounded -- full drops
_Static_assert((CONS_RING_SIZE & (CONS_RING_SIZE - 1u)) == 0u,
               "CONS_RING_SIZE must be a power of two (the ring is mask-indexed)");

// LS-8b: the canonical line-assembly buffer (cooked mode). Bounded -- a line
// longer than this drops the overflow (like the ring); Enter still delivers
// what fits. The echo staging per input byte is at most 3 ("\b \b" erase).
#define CONS_LINE_MAX     256u
#define CONS_ECHO_MAX     8u    // max echo bytes one cons_rx_input byte produces

struct cons_input {
    spin_lock_t lock;                  // ring + head/tail + reader_busy; taken irqsave
    u8          ring[CONS_RING_SIZE];
    u32         head;                  // next byte to read
    u32         tail;                  // next slot to write
    u32         count;                 // mutated under lock; read locklessly in cond -- cons_count_*
    bool        reader_busy;           // a devcons_read is parked (single-reader)
    bool        intr_pending;          // mutated under lock; read locklessly in cond -- cons_intr_*
    bool        sak_pending;           // A-4c-2: a serial BREAK (SAK) awaits console_mgr -- cons_sak_*
    bool        poll_wake_pending;     // LS-8a: a POLLIN edge awaits console_mgr's hook walk -- cons_pollwake_*

    // LS-8b: the line discipline. `termios` holds the five cooking flags
    // (CONS_ICANON|ECHO|ISIG|ICRNL|ONLCR), mutated + read under g_cons.lock
    // (cons_termios_* are RELAXED-atomic for consistency with the sibling
    // flags + so any future lockless read is well-defined). `line`/`line_len`
    // are the cooked-mode line-assembly buffer (canonical mode only), mutated
    // under g_cons.lock; a completed line is flushed to `ring` on Enter.
    u32         termios;
    u8          line[CONS_LINE_MAX];
    u32         line_len;

    // LS-8a: the poll-hook list for /dev/cons. The SYS_CONSOLE_OPEN fd (devcons)
    // AND the namespace /dev/cons leaf (devdev) share it -- #57b single-impl, so
    // a wake reaches every poller of the one console. cons_rx_input runs in IRQ
    // context and CANNOT walk it (poll_waiter_list_wake takes a plain non-irqsave
    // lock + nests a wakeup); it sets poll_wake_pending instead, and console_mgr
    // walks the list in process context (the cons_poll.tla I-9 deferred-wake
    // relay). The list lives in this file-scope static -> IMMORTAL, so the
    // RW-2 2C-F1 registered-object-lifetime hazard (a sibling freeing the
    // embedded list mid-sleep) structurally cannot arise here; multi-poller
    // composition is the standard poll.tla case (each poller has its own private
    // Rendez + stack waiter).
    struct poll_waiter_list poll_list;
};

static struct cons_input g_cons = {
    .lock      = SPIN_LOCK_INIT,
    .termios   = CONS_TERMIOS_DEFAULT,   // LS-8b: boot default == pre-LS-8b behavior
    .poll_list = POLL_WAITER_LIST_INIT,
};
static struct Rendez g_cons_data_rendez = RENDEZ_INIT;   // a reader parks here
static struct Rendez g_cons_mgr_rendez  = RENDEZ_INIT;   // console_mgr parks here

// `count` and `intr_pending` are MUTATED only under g_cons.lock, but they are
// also READ locklessly inside the sleep conds (cons_data_ready /
// cons_mgr_pending run under the Rendez lock, NOT g_cons.lock). A plain
// cross-lock read of a field written under another lock is a C11 data race, so
// these two fields are accessed via RELAXED atomics -- which makes the lockless
// cond read well-defined (never torn) WITHOUT changing the lock structure.
// Crucially, the no-lost-wakeup guarantee (I-9) does NOT come from these atomics:
// it comes from the Rendez lock (the producer's wakeup() acquires the Rendez lock
// that the sleeper's cond-check + sleep-transition hold), so a stale RELAXED read
// at worst costs one extra sleep/recheck cycle, never a lost wake. (NOTE: this is
// NOT the devnotes_read pattern -- that reads a dedicated per-waiter `ready` flag;
// here the cond reads the shared count/flag directly, hence the atomic.)
static inline u32  cons_count_load(void)   { return __atomic_load_n(&g_cons.count, __ATOMIC_RELAXED); }
static inline void cons_count_store(u32 v) { __atomic_store_n(&g_cons.count, v, __ATOMIC_RELAXED); }
static inline bool cons_intr_load(void)    { return __atomic_load_n(&g_cons.intr_pending, __ATOMIC_RELAXED); }
static inline void cons_intr_store(bool v) { __atomic_store_n(&g_cons.intr_pending, v, __ATOMIC_RELAXED); }
static inline bool cons_sak_load(void)     { return __atomic_load_n(&g_cons.sak_pending, __ATOMIC_RELAXED); }
static inline void cons_sak_store(bool v)  { __atomic_store_n(&g_cons.sak_pending, v, __ATOMIC_RELAXED); }
static inline bool cons_pollwake_load(void)   { return __atomic_load_n(&g_cons.poll_wake_pending, __ATOMIC_RELAXED); }
static inline void cons_pollwake_store(bool v) { __atomic_store_n(&g_cons.poll_wake_pending, v, __ATOMIC_RELAXED); }

// LS-8b: the termios word. Read + written under g_cons.lock (cooking reads it,
// consctl writes it); RELAXED-atomic for consistency with the sibling flags.
static inline u32  cons_termios_load(void)   { return __atomic_load_n(&g_cons.termios, __ATOMIC_RELAXED); }
static inline void cons_termios_store(u32 v) { __atomic_store_n(&g_cons.termios, v, __ATOMIC_RELAXED); }

// LS-8b: the echo / output sink. Console echo (cons_rx_input) AND program output
// (cons_output_write) emit one cooked byte through cons_emit. In production it is
// uart_putc; a test enables capture (cons_test_echo_capture) to buffer the bytes
// instead -- so a test can assert EXACTLY what was echoed AND the ECHO-off
// no-output property. g_cons_echo_capture is ALWAYS false in production (only the
// test hook sets it), so the production emit is a single never-taken branch then
// uart_putc; the capture buffer is single-threaded test state (the UP test
// harness drives it), never touched concurrently.
static u8   g_cons_echo_cap[128];
static u32  g_cons_echo_cap_len;
static bool g_cons_echo_capture;

static void cons_emit(u8 b) {
    if (g_cons_echo_capture) {
        if (g_cons_echo_cap_len < sizeof(g_cons_echo_cap))
            g_cons_echo_cap[g_cons_echo_cap_len++] = b;
        return;
    }
    uart_putc((char)b);
}

// Stage one echoed/output byte into `echo[*necho]`, applying ONLCR (NL -> CR NL).
// Bounded by CONS_ECHO_MAX at every call site (a NL stages 2, a plain byte 1).
static void cons_echo_stage(u8 b, u32 tio, u8 *echo, int *necho) {
    if (b == (u8)'\n' && (tio & CONS_ONLCR)) {
        echo[(*necho)++] = (u8)'\r';
        echo[(*necho)++] = (u8)'\n';
    } else {
        echo[(*necho)++] = b;
    }
}

// Enqueue one byte to the RX ring under g_cons.lock. Returns true iff a byte was
// actually enqueued (-> a data-Rendez wake is owed). On the empty->non-empty
// edge it arms the LS-8a deferred poll-wake (poll_wake_pending) and sets
// *wake_mgr (the console_mgr walks the hook list in process context). Bounded:
// drops silently when the ring is full (never overflows).
static bool cons_ring_push(u8 byte, bool *wake_mgr) {
    u32 c = cons_count_load();
    if (c >= CONS_RING_SIZE) return false;
    g_cons.ring[g_cons.tail] = byte;
    g_cons.tail = (g_cons.tail + 1u) & (CONS_RING_SIZE - 1u);
    cons_count_store(c + 1u);
    if (c == 0u) {                 // empty -> non-empty: arm the deferred poll-wake
        cons_pollwake_store(true);
        *wake_mgr = true;
    }
    return true;
}

void cons_rx_input(u8 byte, bool is_break) {
    bool wake_data = false, wake_mgr = false;
    u8   echo[CONS_ECHO_MAX];
    int  necho = 0;

    irq_state_t s = spin_lock_irqsave(&g_cons.lock);
    u32 tio = cons_termios_load();

    if (is_break) {
        // A-4c-2 SAK: a serial BREAK is a PL011 line condition (DR.BE), not a
        // data byte -- EL0-written bytes cannot forge it, and the accompanying
        // DR byte (0x00) is never enqueued. Recognized UNCONDITIONALLY of
        // termios (the I-27 trusted-path line condition must not be gated by a
        // mode flag). Set sak-pending + defer the privileged revoke/re-grant to
        // console_mgr's process context (proc_console_sak takes g_proc_table_lock
        // -- not IRQ-safe). The recognizer is stateless: one flag, no multi-byte
        // state machine to starve or partially-spoof.
        cons_sak_store(true);
        wake_mgr = true;
    } else {
        // LS-8b: ICRNL -- translate an input CR to NL BEFORE ISIG / canon /
        // echo see the byte (so Enter-as-CR terminates a canonical line + echoes
        // as a newline).
        if (byte == (u8)'\r' && (tio & CONS_ICRNL)) byte = (u8)'\n';

        if (byte == 0x03u && (tio & CONS_ISIG)) {
            // ISIG: Ctrl-C is cooked-consumed -> the deferred `interrupt` note
            // (the LS-5 path). ISIG clear -> 0x03 falls through as a data byte.
            cons_intr_store(true);
            wake_mgr = true;
        } else if (tio & CONS_ICANON) {
            // Canonical (cooked) mode: assemble a line; deliver it on NL.
            if (byte == 0x7fu || byte == 0x08u) {     // DEL / BS: erase one char
                if (g_cons.line_len > 0u) {
                    g_cons.line_len--;
                    if (tio & CONS_ECHO) {            // visually erase: back, space, back
                        echo[necho++] = (u8)'\b';
                        echo[necho++] = (u8)' ';
                        echo[necho++] = (u8)'\b';
                    }
                }
                // empty line + erase: nothing to erase, nothing echoed (never
                // back over the prompt).
            } else if (byte == (u8)'\n') {            // terminator: deliver line + NL
                // POSIX canonical: the read returns the line INCLUDING its
                // terminating newline. Flush line[0..len) then the NL to the ring
                // (the empty->non-empty poll-edge is handled inside cons_ring_push).
                for (u32 i = 0; i < g_cons.line_len; i++)
                    if (cons_ring_push(g_cons.line[i], &wake_mgr)) wake_data = true;
                if (cons_ring_push((u8)'\n', &wake_mgr)) wake_data = true;
                g_cons.line_len = 0u;
                if (tio & CONS_ECHO) cons_echo_stage((u8)'\n', tio, echo, &necho);
            } else {                                  // ordinary char: buffer it
                if (g_cons.line_len < CONS_LINE_MAX) {
                    g_cons.line[g_cons.line_len++] = byte;
                    if (tio & CONS_ECHO) cons_echo_stage(byte, tio, echo, &necho);
                }
                // else: line buffer full -> drop (bounded; Enter still delivers
                // what fits). A dropped byte is NOT echoed.
            }
        } else {
            // Raw / cbreak mode: byte-at-a-time to the ring (the pre-LS-8b path).
            if (cons_ring_push(byte, &wake_mgr)) wake_data = true;
            if (tio & CONS_ECHO) cons_echo_stage(byte, tio, echo, &necho);
        }
    }
    spin_unlock_irqrestore(&g_cons.lock, s);

    // Echo is emitted with the lock RELEASED: cons_emit -> uart_putc is lock-free
    // (it polls TXFF; no lock/sleep), so the staged bytes go out without holding
    // g_cons.lock across the UART busy-wait. wakeup() is IRQ-safe (irqsave); a
    // wake with no waiter is a no-op. The producer set the condition under
    // g_cons.lock and the wakeup takes the Rendez lock the sleeper's cond-check +
    // sleep-transition hold -> no lost wakeup (I-9; cons_poll.tla for the
    // poll-edge relay).
    for (int i = 0; i < necho; i++) cons_emit(echo[i]);
    if (wake_data) wakeup(&g_cons_data_rendez);
    if (wake_mgr)  wakeup(&g_cons_mgr_rendez);
}

// cond: the ring holds at least one byte. Runs under the Rendez lock (NOT
// g_cons.lock), so the count read is a RELAXED atomic (see the cons_count_*
// rationale); the Rendez lock provides the no-lost-wakeup pairing.
static int cons_data_ready(void *arg) {
    (void)arg;
    return cons_count_load() > 0u;
}

// cond: a deferred console action is pending (a Ctrl-C interrupt OR an A-4c-2
// SAK). Same lockless-under-Rendez-lock discipline as cons_data_ready.
static int cons_mgr_pending(void *arg) {
    (void)arg;
    return cons_intr_load() || cons_sak_load() || cons_pollwake_load();
}

// Service all deferred console actions in process context: drain the flags
// under g_cons.lock, then act with the lock RELEASED. The act must run lock-free
// -- proc_console_sak takes g_proc_table_lock; poll_waiter_list_wake takes the
// poll_list lock + nests a wakeup; neither is legal under g_cons.lock. Shared by
// console_mgr_main + the test harness (cons_test_service_deferred) so a test
// drives the production path EXACTLY.
static void cons_service_deferred(void) {
    irq_state_t s = spin_lock_irqsave(&g_cons.lock);
    bool do_intr = cons_intr_load();
    bool do_sak  = cons_sak_load();
    bool do_poll = cons_pollwake_load();
    cons_intr_store(false);
    cons_sak_store(false);
    cons_pollwake_store(false);
    spin_unlock_irqrestore(&g_cons.lock, s);

    // RW-7 R2-F2 (round-2 F2): a SAK SUPERSEDES a Ctrl-C coalesced into the
    // same batch -- the two pending flags lose their arrival order, and
    // posting `interrupt` to the PRE-SAK owner (joey during bringup ->
    // non-self-managing -> the LS-5 terminate latch -> init dies) is exactly
    // the outcome R2-F2 removed from the SAK itself, re-synthesized via
    // coalescing. Post-SAK the owner is NULL, so the chronologically-correct
    // delivery of an after-BREAK Ctrl-C is a drop; a before-BREAK Ctrl-C
    // losing to a near-simultaneous SAK is the operator's intent (they hit
    // BREAK to reach the trusted prompt). Both run in process context, never
    // under g_cons.lock (proc_console_sak takes g_proc_table_lock; since
    // R2-F2 it posts NO note -- it only revokes + re-grants the attach bit).
    if (do_sak)       proc_console_sak();
    else if (do_intr) proc_console_post_interrupt();

    // LS-8a: the deferred poll-wake. A POLLIN edge (cons_rx_input set
    // poll_wake_pending) -> walk the hook list now, in process context, where
    // poll_waiter_list_wake's plain lock + nested wakeup are legal. Independent
    // of intr/sak (a data byte arrives with no Ctrl-C). The walk runs with
    // g_cons.lock RELEASED (lock order object -> list); the producer's count
    // mutation already happened-before via the just-drained flag, so any poller
    // registered before it is found -- cons_poll.tla NoMissedConsPoll.
    if (do_poll)      poll_waiter_list_wake(&g_cons.poll_list);
}

// The console_mgr kproc kthread (spawned once at boot). Services deferred
// console actions in process context.
void console_mgr_main(void) {
    for (;;) {
        // kproc's console_mgr never group-terminates at v1.0; a (defensive)
        // death-interrupt just re-loops -- there is no caller state to unwind.
        if (sleep(&g_cons_mgr_rendez, cons_mgr_pending, NULL) == SLEEP_INTR)
            continue;
        cons_service_deferred();
    }
}

void cons_test_reset(void) {
    irq_state_t s = spin_lock_irqsave(&g_cons.lock);
    g_cons.head = g_cons.tail = 0u;
    cons_count_store(0u);
    g_cons.reader_busy = false;
    cons_intr_store(false);
    cons_sak_store(false);
    cons_pollwake_store(false);
    cons_termios_store(CONS_TERMIOS_DEFAULT);   // LS-8b: back to the boot default
    g_cons.line_len = 0u;
    spin_unlock_irqrestore(&g_cons.lock, s);
}

bool cons_test_intr_pending(void) {
    return cons_intr_load();
}

bool cons_test_sak_pending(void) {
    return cons_sak_load();
}

bool cons_test_pollwake_pending(void) {
    return cons_pollwake_load();
}

void cons_test_service_deferred(void) {
    cons_service_deferred();
}

void cons_test_set_reader_busy(bool busy) {
    irq_state_t s = spin_lock_irqsave(&g_cons.lock);
    g_cons.reader_busy = busy;
    spin_unlock_irqrestore(&g_cons.lock, s);
}

u32 cons_test_termios(void) {
    return cons_termios_load();
}

void cons_test_set_termios(u32 v) {
    irq_state_t s = spin_lock_irqsave(&g_cons.lock);
    cons_termios_store(v & CONS_TERMIOS_ALL);
    g_cons.line_len = 0u;                        // a mode flip starts a fresh line
    spin_unlock_irqrestore(&g_cons.lock, s);
}

void cons_test_echo_capture(bool on) {
    g_cons_echo_cap_len = 0u;
    g_cons_echo_capture = on;
}

u32 cons_test_echo_captured(u8 *out, u32 max) {
    u32 k = (g_cons_echo_cap_len < max) ? g_cons_echo_cap_len : max;
    for (u32 i = 0; i < k; i++) out[i] = g_cons_echo_cap[i];
    return g_cons_echo_cap_len;                  // true count (caller detects overflow)
}

static void devcons_reset(void)    { /* no-op */ }
static void devcons_init(void)     { /* no-op — UART came up at boot */ }
static void devcons_shutdown(void) { /* no-op */ }

static struct Spoor *devcons_attach(const char *spec) {
    (void)spec;
    return dev_simple_attach(&devcons, QTFILE);
}

static struct Walkqid *devcons_walk(struct Spoor *c, struct Spoor *nc,
                                    const char **name, int nname) {
    (void)c; (void)nc; (void)name; (void)nname;
    return NULL;
}

static int devcons_stat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devcons_open(struct Spoor *c, int omode) {
    return dev_simple_open(c, omode);
}

static struct Spoor *devcons_create(struct Spoor *c, const char *name, int omode, u32 perm, u32 gid) {
    (void)c; (void)name; (void)omode; (void)perm; (void)gid;
    return NULL;
}

static void devcons_close(struct Spoor *c) {
    dev_simple_close(c);
}

// A-4c-1: blocking console read. Drains the RX ring; blocks on
// g_cons_data_rendez when empty (death-interruptible per #811). Single-reader:
// a 2nd concurrent blocking read returns -1 (the data Rendez is single-waiter;
// a 2nd sleeper would extinct). Returns the byte count (>= 1) on data, 0 only on
// a death-interrupt with nothing buffered (immaterial -- a group-flagged Thread
// never re-enters EL0), or -1 on bad args / reader-busy.
//
// #57b: this is the ONE console-input implementation, shared by both front doors
// -- `devcons` (the SYS_CONSOLE_OPEN syscall path) and `devdev`'s /dev/cons leaf
// (the namespace path). Both call cons_input_read, so the single-reader busy-guard
// (g_cons.reader_busy) bounds the console to one reader ACROSS both doors -- there
// is no second reader path that could race the first.
long cons_input_read(void *buf, long n) {
    if (!buf || n < 0) return -1;
    if (n == 0)        return 0;

    irq_state_t s = spin_lock_irqsave(&g_cons.lock);
    if (g_cons.reader_busy) {
        spin_unlock_irqrestore(&g_cons.lock, s);
        return -1;
    }
    g_cons.reader_busy = true;
    spin_unlock_irqrestore(&g_cons.lock, s);

    // RW-11 SA-1b: a TRUSTED console reader -- the session shell (the console
    // OWNER) or a console-ATTACHED authority (login/corvus) -- is an interactive
    // "terminal app" (ARCH 8.3); its wake (input arrived) should preempt NORMAL
    // work, so it is promoted to the INTERACTIVE band. The gate is NARROW on
    // purpose (audit F1): /dev/cons has NO per-open capability gate and is
    // inherited as stdin by foreground children at v1.0 (PTY is LS-8/Phase-8), so
    // an ungated promotion would let any unprivileged program that reads its stdin
    // self-promote above NORMAL and starve it (a fixed-priority band, no aging).
    // The band==NORMAL pre-check keeps the (locking) owner query off the path once
    // the reader is already promoted (sticky) -- and bounds it to interactive
    // frequency for an untrusted reader that stays NORMAL.
    struct Thread *reader = current_thread();
    if (reader && reader->band == SCHED_BAND_NORMAL && reader->proc &&
        (proc_is_console_attached(reader->proc) ||
         proc_is_console_owner(reader->proc))) {
        sched_mark_interactive(reader);
    }

    u8 *out = (u8 *)buf;
    long got = 0;
    for (;;) {
        s = spin_lock_irqsave(&g_cons.lock);
        u32 c = cons_count_load();
        while (c > 0u && got < n) {
            out[got++] = g_cons.ring[g_cons.head];
            g_cons.head = (g_cons.head + 1u) & (CONS_RING_SIZE - 1u);
            c--;
        }
        cons_count_store(c);
        spin_unlock_irqrestore(&g_cons.lock, s);
        if (got > 0) break;            // read() returns as soon as >= 1 byte is ready
        if (sleep(&g_cons_data_rendez, cons_data_ready, NULL) == SLEEP_INTR) break;
    }

    s = spin_lock_irqsave(&g_cons.lock);
    g_cons.reader_busy = false;
    spin_unlock_irqrestore(&g_cons.lock, s);
    return got;
}

static long devcons_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)c; (void)off;
    return cons_input_read(buf, n);
}

static struct Block *devcons_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

// Writes forward each byte to the PL011 UART via cons_emit (-> uart_putc). Plan 9
// idiom: writes don't persist — the byte IS the message. Returns the
// number of bytes accepted (== n at v1.0; UART can't fail short).
//
// LS-8b: ONLCR -- an output NL is translated to CR NL when the flag is set
// (default clear, so the pre-LS-8b behavior is unchanged: bare LF forwarded).
// The termios read is lockless (RELAXED atomic; a mode flip racing a write just
// switches translation mid-buffer -- cosmetic, never torn).
//
// #57b: the ONE console-output implementation, shared by devcons (the syscall
// path) and devdev's /dev/cons leaf (the namespace path).
long cons_output_write(const void *buf, long n) {
    if (!buf) return -1;
    if (n < 0) return -1;
    if (n == 0) return 0;

    u32 tio = cons_termios_load();
    const u8 *bytes = (const u8 *)buf;
    for (long i = 0; i < n; i++) {
        if (bytes[i] == (u8)'\n' && (tio & CONS_ONLCR)) {
            cons_emit((u8)'\r');
            cons_emit((u8)'\n');
        } else {
            cons_emit(bytes[i]);
        }
    }
    return n;
}

static long devcons_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)c; (void)off;
    return cons_output_write(buf, n);
}

static long devcons_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

static void devcons_remove(struct Spoor *c) {
    (void)c;
}

static int devcons_wstat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devcons_power(struct Spoor *c, int on) {
    (void)c; (void)on;
    return NULL;
}

// =============================================================================
// LS-8b: the /dev/consctl control surface (parse + render).
// =============================================================================
//
// The wire grammar: whitespace-separated "+name"/"-name" tokens; the names below
// are the five flags, in render order. NOT ioctl -- the Plan 9 consctl-file
// idiom (capability-microkernel SOTA agrees: a control channel). Phase-8 Pouch
// maps tcsetattr/tcgetattr <-> these strings at the boundary-line.
struct cons_flag_name { const char *name; u32 bit; };
static const struct cons_flag_name g_cons_flag_names[] = {
    { "icanon", CONS_ICANON },
    { "echo",   CONS_ECHO   },
    { "isig",   CONS_ISIG   },
    { "icrnl",  CONS_ICRNL  },
    { "onlcr",  CONS_ONLCR  },
};
#define CONS_FLAG_COUNT (sizeof(g_cons_flag_names) / sizeof(g_cons_flag_names[0]))

static bool cons_is_space(u8 c) {
    return c == (u8)' ' || c == (u8)'\t' || c == (u8)'\n' || c == (u8)'\r';
}

// Match buf[start..end) (the chars AFTER the +/- sign) against a flag name.
// Returns the bit, or 0 on no match (unknown name OR an empty token).
static u32 cons_flag_lookup(const u8 *buf, long start, long end) {
    for (size_t f = 0; f < CONS_FLAG_COUNT; f++) {
        const char *nm = g_cons_flag_names[f].name;
        long i = start;
        size_t j = 0;
        while (i < end && nm[j] != '\0' && (u8)nm[j] == buf[i]) { i++; j++; }
        if (i == end && nm[j] == '\0') return g_cons_flag_names[f].bit;
    }
    return 0u;
}

long cons_set_mode_cmd(const void *buf, long n) {
    if (!buf || n < 0) return -1;
    const u8 *b = (const u8 *)buf;

    // Parse ALL tokens first (atomic apply): a single malformed token rejects
    // the whole write with no change (the tcsetattr-is-atomic seam).
    u32 set_mask = 0u, clear_mask = 0u;
    int tokens = 0;
    long i = 0;
    while (i < n) {
        while (i < n && cons_is_space(b[i])) i++;            // skip whitespace
        if (i >= n) break;
        u8 sign = b[i];
        if (sign != (u8)'+' && sign != (u8)'-') return -1;   // malformed token
        long name_start = i + 1;
        long j = name_start;
        while (j < n && !cons_is_space(b[j])) j++;            // token end
        u32 bit = cons_flag_lookup(b, name_start, j);
        if (bit == 0u) return -1;                            // unknown / empty name
        if (sign == (u8)'+') { set_mask |= bit;   clear_mask &= ~bit; }
        else                 { clear_mask |= bit; set_mask   &= ~bit; }
        tokens++;
        i = j;
    }
    if (tokens == 0) return -1;                               // empty command

    irq_state_t s = spin_lock_irqsave(&g_cons.lock);
    u32 cur = cons_termios_load();
    cons_termios_store((cur | set_mask) & ~clear_mask);
    spin_unlock_irqrestore(&g_cons.lock, s);
    return n;
}

long cons_render_mode(void *buf, long n) {
    if (!buf || n < 0) return 0;
    u32 tio = cons_termios_load();                           // atomic snapshot
    u8 *out = (u8 *)buf;
    long off = 0;
    for (size_t f = 0; f < CONS_FLAG_COUNT; f++) {
        const char *nm = g_cons_flag_names[f].name;
        long namelen = 0;
        while (nm[namelen]) namelen++;
        long need = 1 + namelen + 1;                         // sign + name + sep
        if (off + need > n) return 0;                        // too small -> nothing
        out[off++] = (tio & g_cons_flag_names[f].bit) ? (u8)'+' : (u8)'-';
        for (long k = 0; k < namelen; k++) out[off++] = (u8)nm[k];
        out[off++] = (f + 1 == CONS_FLAG_COUNT) ? (u8)'\n' : (u8)' ';
    }
    return off;
}

// LS-8a: the ONE console poll implementation, shared by devcons (the
// SYS_CONSOLE_OPEN fd) + devdev's /dev/cons leaf (#57b single-impl). Register-
// then-observe: sample readiness AND (if pw) install the hook, BOTH under
// g_cons.lock -- so a cons_rx_input that sets the ring count under the same lock
// is serialized against this sample+register (the producer's mutation either
// happens-before the sample, seen directly, or after, found by the deferred
// hook-list walk). POLLIN iff the ring holds >= 1 byte; POLLOUT always
// (uart_putc never blocks -- a poller must therefore request POLLIN to wait for
// input). pw is registered UNCONDITIONALLY (even when POLLIN-ready);
// sys_poll_for_proc's fast path unregisters it -- the poll.tla / devpipe
// discipline. The poll_waiter_list_register nests the (plain) list lock under
// g_cons.lock (irqsave) -- lock order object -> list, IRQs already masked.
short cons_poll(short events, struct poll_waiter *pw) {
    short revents = 0;
    irq_state_t s = spin_lock_irqsave(&g_cons.lock);
    if ((events & POLLIN) && cons_count_load() > 0u) revents |= POLLIN;
    if (events & POLLOUT)                            revents |= POLLOUT;
    if (pw) poll_waiter_list_register(&g_cons.poll_list, pw);
    spin_unlock_irqrestore(&g_cons.lock, s);
    return revents;
}

static short devcons_poll(struct Spoor *c, short events, struct poll_waiter *pw) {
    (void)c;
    return cons_poll(events, pw);
}

struct Dev devcons = {
    .dc       = 'c',
    .name     = "cons",

    .reset    = devcons_reset,
    .init     = devcons_init,
    .shutdown = devcons_shutdown,

    .attach   = devcons_attach,
    .walk     = devcons_walk,
    .stat     = devcons_stat,

    .open     = devcons_open,
    .create   = devcons_create,
    .close    = devcons_close,

    .read     = devcons_read,
    .bread    = devcons_bread,
    .write    = devcons_write,
    .bwrite   = devcons_bwrite,
    .poll     = devcons_poll,

    .remove   = devcons_remove,
    .wstat    = devcons_wstat,
    .power    = devcons_power,
};
