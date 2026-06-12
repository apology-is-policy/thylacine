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

static struct cons_input g_cons = { .lock = SPIN_LOCK_INIT, .poll_list = POLL_WAITER_LIST_INIT };
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

void cons_rx_input(u8 byte, bool is_break) {
    bool wake_data = false, wake_mgr = false;
    irq_state_t s = spin_lock_irqsave(&g_cons.lock);
    if (is_break) {
        // A-4c-2 SAK: a serial BREAK is a PL011 line condition (DR.BE), not a
        // data byte -- EL0-written bytes cannot forge it, and the accompanying
        // DR byte (0x00) is never enqueued. Set sak-pending + defer the
        // privileged revoke/re-grant to console_mgr's process context
        // (proc_console_sak takes g_proc_table_lock to revoke + re-grant the
        // console-attach bit -- not IRQ-safe). The recognizer is stateless: one flag, no multi-byte state
        // machine to starve or partially-spoof (the I-27 "cannot be
        // starved/spoofed by crafted input" obligation is thus structural).
        cons_sak_store(true);
        wake_mgr = true;
    } else if (byte == 0x03u) {         // Ctrl-C: cooked-consume -> interrupt note
        cons_intr_store(true);
        wake_mgr = true;
    } else {
        u32 c = cons_count_load();
        if (c < CONS_RING_SIZE) {
            g_cons.ring[g_cons.tail] = byte;
            g_cons.tail = (g_cons.tail + 1u) & (CONS_RING_SIZE - 1u);
            cons_count_store(c + 1u);
            wake_data = true;
            // LS-8a: relay the POLLIN readiness edge to pollers. Only the
            // empty->non-empty transition needs a wake -- a poller that sampled
            // count>0 returned POLLIN without sleeping, so the only sleeping
            // pollers sampled count==0, and this byte (c==0) is the edge that
            // makes them ready. poll_waiter_list_wake is NOT IRQ-safe, so set
            // the flag + wake console_mgr (which walks the hook list in process
            // context). The flag is set under g_cons.lock, the SAME lock as the
            // count store, so the mgr's deferred drain+walk is causally after
            // this mutation -> any poller that registered before it is found
            // (register-then-observe; cons_poll.tla NoMissedConsPoll).
            if (c == 0u) {
                cons_pollwake_store(true);
                wake_mgr = true;
            }
        }
        // else: ring full -> drop (bounded; never overflows).
    }
    spin_unlock_irqrestore(&g_cons.lock, s);

    // wakeup() is IRQ-safe (irqsave); a wake with no waiter is a no-op. The
    // producer set the condition under g_cons.lock and the wakeup takes the
    // Rendez lock the sleeper's cond-check + sleep-transition hold -> no lost
    // wakeup (the register-then-observe / I-9 pairing; see the cons_count_* /
    // cons_intr_* rationale above for why the cond's lockless read is sound).
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

// Writes forward each byte to the PL011 UART via uart_putc. Plan 9
// idiom: writes don't persist — the byte IS the message. Returns the
// number of bytes accepted (== n at v1.0; UART can't fail short).
//
// #57b: the ONE console-output implementation, shared by devcons (the syscall
// path) and devdev's /dev/cons leaf (the namespace path).
long cons_output_write(const void *buf, long n) {
    if (!buf) return -1;
    if (n < 0) return -1;
    if (n == 0) return 0;

    const u8 *bytes = (const u8 *)buf;
    for (long i = 0; i < n; i++) {
        uart_putc((char)bytes[i]);
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
