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
#include <thylacine/joey.h>                  // #95: boot_is_complete (the drop-report gate)
#include <thylacine/poll.h>                  // LS-8a: pollable cons (deferred poll-wake)
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>                 // RW-11 SA-1b: sched_mark_interactive
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>               // #55: struct t_stat (cons_stat_native_fill)
#include <thylacine/thread.h>                // RW-11 SA-1b: current_thread
#include <thylacine/types.h>

#include "../arch/arm64/timer.h"              // #75: the TX room-wait deadline
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

#define CONS_RING_SIZE  512u   // power of two (mask-indexed)
_Static_assert((CONS_RING_SIZE & (CONS_RING_SIZE - 1u)) == 0u,
               "CONS_RING_SIZE must be a power of two (the ring is mask-indexed)");

// #129: the ring must be able to hold ONE worst-case cooked line. A canonical
// Enter flushes line_len + 1 bytes (the line plus its terminating newline) in a
// SINGLE cons_rx_input call, so the largest atomic push this ring ever sees is
// CONS_LINE_MAX + 1. Until #129 the ring was 256 and CONS_LINE_MAX was (still
// is) 256, i.e. a maximal line did not fit even into a COMPLETELY EMPTY ring --
// which is the deeper half of the bug, and the half that makes the obvious fix
// wrong. The obvious admission gate ("require count + line_len + 1 <= size")
// is correct in form, but with those sizes it refuses a full-length line at
// count == 0 and keeps refusing forever: RX pauses permanently and the console
// wedges. A bounded drop would have become a deadlock. The gate is only sound
// once the ring can actually hold what the gate is asked to reserve, so the
// sizing IS part of the fix, not an optimization alongside it.
_Static_assert(CONS_LINE_MAX + 1u <= CONS_RING_SIZE,
               "CONS_RING_SIZE must hold one worst-case cooked flush "
               "(CONS_LINE_MAX + 1); otherwise the #129 admission gate can "
               "never admit a full line and RX pauses forever");

// CONS_LINE_MAX (the LS-8b canonical line-assembly bound) lives in cons.h --
// #95's drop test asserts against it, and a test hardcoding the number would
// agree with the code only by coincidence.
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

    // #55 (ARCH 23.5.3): the console winsize. Written by the renderer via the
    // consctl `winsize <cols> <rows>` verb (aurora reports its cell grid --
    // geometry is physical, not negotiated); 0x0 = never set (the serial
    // posture: no renderer exists, readers fall back to CPR, which the HOST
    // terminal answers). Mutated + read under g_cons.lock like the flags.
    // winch_events counts CHANGED applies (diagnostic + the iff-changed
    // regression's witness; each corresponds to one tty:winch post attempt).
    u16         ws_cols;
    u16         ws_rows;
    u32         winch_events;

    // #95: the INPUT-path silent-drop counters. Every RX drop site is counted
    // here, under g_cons.lock (all three sites already run under it). They exist
    // because a lost input byte previously left NO trace anywhere: #95 observed
    // `sleep 30` arrive as `sleep 3` -- one byte gone, the terminator delivered
    // -- and there was nothing in the tree that could say whether the kernel had
    // dropped it. The TX side has had exactly this since #75/#126
    // (g_cons_tx.dropped); the RX side had nothing.
    //
    // Split per SITE, deliberately. A lumped counter would only move the
    // question from "did we drop?" to "where?", and the sites have entirely
    // different causes and fixes.
    //
    // #129 CHANGED WHAT TWO OF THESE MEAN, so they are renamed rather than left
    // to accumulate a new meaning under an old name. Both ring-full sites now
    // REFUSE the byte (back-pressure -- the producer keeps it and retries)
    // instead of dropping it, so counting them as "drops" would be a counter
    // that is wrong in the most expensive way: read at 3am, believed, and
    // pointing at data loss that did not happen.
    //
    //   rx_bp_raw    -- raw/cbreak arm, ring full: the byte was REFUSED. The
    //                   UART holds it in the FIFO (or in the 1-byte holdback)
    //                   and cons_feed_write returns a short count. Non-zero
    //                   means back-pressure engaged, NOT that input was lost.
    //   rx_bp_flush  -- cooked arm, an Enter whose line_len + 1 bytes did not
    //                   fit: REFUSED with the line intact, retried on the next
    //                   offer of the same terminator.
    //   rx_drop_line -- cooked arm, a byte past CONS_LINE_MAX. STILL A REAL
    //                   DROP, deliberately: the line buffer is a fixed bound, so
    //                   back-pressuring here would wedge on a user who simply
    //                   never presses Enter. Bounded, and the byte is un-echoed.
    //   rx_drop_ring -- MUST STAY ZERO. cons_ring_push failed after the room
    //                   check under this same lock said it would fit. That is
    //                   an arithmetic disagreement between cons_ring_room() and
    //                   the ring, i.e. the #129 fix itself is broken. It is not
    //                   a diagnostic counter but an invariant WITNESS: it has no
    //                   reachable driver by construction, and that is the claim
    //                   it exists to falsify.
    //
    // Counting is NOT a fix for any of them; it is what makes the next
    // occurrence decidable instead of unexplained.
    u32         rx_bp_raw;
    u32         rx_bp_flush;
    u32         rx_drop_line;
    u32         rx_drop_ring;

    // #95: set by any drop site, drained by console_mgr, which emits ONE loud
    // line in process context (the intr/sak/pollwake deferred-relay pattern --
    // the drop sites run in IRQ context under this lock and must not do the
    // emitting themselves). `drop_reported` then latches it off FOREVER: a
    // pathological drop storm must not become a diagnostic storm that costs
    // more than the events it reports (the #126 lesson). The running totals stay
    // readable at /ctl/cons afterwards.
    bool        drop_report_pending;
    bool        drop_reported;

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

// =============================================================================
// G-4: the console-renderer DRAIN (the Aurora backend's output half).
// =============================================================================
//
// TAPESTRY.md section 18.7: on the Aurora backend, console output bytes ring
// into a drain fid the bound renderer reads. The tap sits in cons_emit -- the
// ONE chokepoint both program output (cons_output_write) and line-discipline
// echo already cross -- so the renderer sees EXACTLY the byte stream a
// terminal displays. On serial-bearing media (QEMU dev boots) the tap is a
// MIRROR: uart_putc continues byte-identical (the host terminal, the tooling
// ABI, and the serial trusted path all keep working); on a serial-less board
// the uart layer is inert and the ring is the only sink. The exclusive
// board-era switch (suppressing EL0 serial output, bound from the DTB medium
// fact per TRUSTED-PATH section 7) is the recorded seam -- the tap composes
// with it (the selector will gate uart_putc, not the tap).
//
// Bounded + non-blocking for writers: console writers NEVER block on the
// renderer (a stalled/dead renderer must not wedge every console write), so
// the ring drops OLDEST on overflow -- the newest output (the prompt the user
// needs) survives; `overflow` counts the loss. The drain producer runs in IRQ
// context (echo from cons_rx_input), so the lock is irqsave and the only wake
// primitive used inside is wakeup() (IRQ-safe); the POLLIN hook-list walk is
// deferred to console_mgr exactly like the cons ring's (the cons_poll.tla
// deferred-wake relay, second instance).

#define CONS_DRAIN_RING_SIZE  8192u   // power of two (mask-indexed)
_Static_assert((CONS_DRAIN_RING_SIZE & (CONS_DRAIN_RING_SIZE - 1u)) == 0u,
               "CONS_DRAIN_RING_SIZE must be a power of two");

struct cons_drain {
    spin_lock_t lock;                  // irqsave (echo-path pushes run in IRQ context)
    u8          ring[CONS_DRAIN_RING_SIZE];
    u32         head;                  // next byte to read
    u32         tail;                  // next slot to write
    u32         count;                 // mutated under lock; read locklessly in conds
    u32         overflow;              // drop-oldest count (diagnostic)
    bool        open;                  // the single drain fid is open (mutated under lock)
    bool        armed;                 // == open; RELAXED-atomic tap gate read per byte
    bool        reader_busy;           // a cons_drain_read is active (single-reader)
    bool        poll_wake_pending;     // a POLLIN edge awaits console_mgr's hook walk
    struct poll_waiter_list poll_list;
};

static struct cons_drain g_cons_drain = {
    .lock      = SPIN_LOCK_INIT,
    .poll_list = POLL_WAITER_LIST_INIT,
};
static struct Rendez g_cons_drain_rendez = RENDEZ_INIT;  // the drain reader parks here

// The same cross-lock-read discipline as the cons ring's count/flags (see the
// cons_count_* rationale above): mutated under g_cons_drain.lock, read
// locklessly inside sleep conds / the tap gate / console_mgr's cond -- RELAXED
// atomics make the lockless reads well-defined; the no-lost-wakeup guarantee
// comes from the Rendez lock pairing, never from these.
static inline u32  drain_count_load(void)      { return __atomic_load_n(&g_cons_drain.count, __ATOMIC_RELAXED); }
static inline void drain_count_store(u32 v)    { __atomic_store_n(&g_cons_drain.count, v, __ATOMIC_RELAXED); }
static inline bool drain_armed_load(void)      { return __atomic_load_n(&g_cons_drain.armed, __ATOMIC_RELAXED); }
static inline void drain_armed_store(bool v)   { __atomic_store_n(&g_cons_drain.armed, v, __ATOMIC_RELAXED); }
static inline bool drain_pollwake_load(void)   { return __atomic_load_n(&g_cons_drain.poll_wake_pending, __ATOMIC_RELAXED); }
static inline void drain_pollwake_store(bool v) { __atomic_store_n(&g_cons_drain.poll_wake_pending, v, __ATOMIC_RELAXED); }

// The tap: mirror one output/echo byte into the drain ring. Called from
// cons_emit (process OR IRQ context). The armed pre-check is a lockless fast
// path -- disarmed (the boot/serial-only state) costs one RELAXED load per
// byte; the open re-check under the lock closes the check-vs-disarm race (a
// byte racing a close is dropped, never pushed to a dead epoch). Wakes the
// drain reader (wakeup is IRQ-safe) and, on the empty->non-empty edge, arms
// the deferred POLLIN walk + wakes console_mgr.
static void cons_drain_tap(u8 byte) {
    if (!drain_armed_load()) return;

    bool wake_mgr = false;
    irq_state_t s = spin_lock_irqsave(&g_cons_drain.lock);
    if (!g_cons_drain.open) {
        spin_unlock_irqrestore(&g_cons_drain.lock, s);
        return;
    }
    u32 c = drain_count_load();
    if (c >= CONS_DRAIN_RING_SIZE) {
        // Full: drop OLDEST (advance head) so the newest output survives.
        g_cons_drain.head = (g_cons_drain.head + 1u) & (CONS_DRAIN_RING_SIZE - 1u);
        g_cons_drain.overflow++;
        c--;
    }
    g_cons_drain.ring[g_cons_drain.tail] = byte;
    g_cons_drain.tail = (g_cons_drain.tail + 1u) & (CONS_DRAIN_RING_SIZE - 1u);
    drain_count_store(c + 1u);
    if (c == 0u) {
        drain_pollwake_store(true);
        wake_mgr = true;
    }
    spin_unlock_irqrestore(&g_cons_drain.lock, s);

    wakeup(&g_cons_drain_rendez);
    if (wake_mgr) wakeup(&g_cons_mgr_rendez);
}

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
// #95: same discipline -- written under g_cons.lock, read locklessly in
// cons_mgr_pending (which runs under the Rendez lock, not this one).
static inline bool cons_dropreport_load(void)   { return __atomic_load_n(&g_cons.drop_report_pending, __ATOMIC_RELAXED); }
static inline void cons_dropreport_store(bool v) { __atomic_store_n(&g_cons.drop_report_pending, v, __ATOMIC_RELAXED); }

// LS-8b: the termios word. Read + written under g_cons.lock (cooking reads it,
// consctl writes it); RELAXED-atomic for consistency with the sibling flags.
static inline u32  cons_termios_load(void)   { return __atomic_load_n(&g_cons.termios, __ATOMIC_RELAXED); }
static inline void cons_termios_store(u32 v) { __atomic_store_n(&g_cons.termios, v, __ATOMIC_RELAXED); }

// ---------------------------------------------------------------------------
// #75 / P1-F -- the console TX ring + the writer role (ARCH §23.5.2).
//
// Before P1-F, cons_output_write looped byte-by-byte into a LOCK-FREE uart_putc,
// so two CPUs writing /dev/cons interleaved at BYTE granularity -- shredding a
// multi-byte glyph or an SGR escape (#75: 10 of 40 gate boots). Two separable
// mechanisms fix it:
//
//   The RING decouples the writer from uart_putc's bounded-but-slow TXFF spin
//   (#67: up to 20 ms per byte against a stalled host consumer). A push is a
//   memory write under a leaf spinlock; the PL011 TX interrupt drains ring ->
//   FIFO. In the HEALTHY case the post-push kick moves the bytes straight into a
//   non-full FIFO and TXIM is never even armed -- behaviour matches the old
//   direct path minus the spin, and per byte it trades an MMIO FR read + DR
//   write for a spinlock + a memory store (a win, especially under HVF where
//   each MMIO is a vmexit).
//
//   The WRITER ROLE makes a whole cons_output_write call atomic against other
//   console writers. It is REQUIRED because a write larger than the ring must
//   sleep for room, dropping the ring lock -- so the ring lock alone can never
//   span the call. This is the audited srvconn.c::chan_role_acquire shape
//   (#354 / CF-3 B) reused in structure: park on a poll_waiter_list with
//   register-then-observe, TSLEEP_INTR unwind, re-contend on wake.
//
// The two ring producers have OPPOSITE blocking contracts, and the asymmetry is
// load-bearing -- a change that blurs it is a bug:
//   - cons_output_write runs in PROCESS context (spoor_write_common holds no
//     lock across dev->write and documents it as blocking), so it MAY sleep for
//     room, and does.
//   - echo from cons_rx_input runs in IRQ context, so it must NEVER sleep: it
//     pushes non-blocking and DROPS on a full ring (a tty overrun -- the same
//     disposition the drain ring uses).
//
// Exactly ONE thread can ever wait on g_cons_tx_room, because only the role
// holder pushes-with-wait and the role is exclusive. THAT is what makes a
// single-waiter Rendez sound here, where the role itself needs a waiter list.
// If a second waiter is ever introduced this must become a poll_waiter_list.
// ---------------------------------------------------------------------------

#define CONS_TX_RING_SIZE  8192u   // power of two (mask-indexed)
_Static_assert((CONS_TX_RING_SIZE & (CONS_TX_RING_SIZE - 1u)) == 0u,
               "CONS_TX_RING_SIZE must be a power of two");

// #67 inherited, NOT weakened: a stalled host consumer stops the TX IRQ, so the
// room-wait is DEADLINED and a timeout drops the remainder of the write (a short
// write). A bounded-but-lossy console is strictly sounder than a wedged writer.
// Matched to UART_TX_SPIN_MAX_NS so the worst-case per-write stall is the same
// order as the direct path it replaces.
#define CONS_TX_ROOM_WAIT_NS  (20ull * 1000ull * 1000ull)   // 20 ms

struct cons_tx {
    spin_lock_t lock;                  // irqsave: the TX IRQ drains under it
    u8          ring[CONS_TX_RING_SIZE];
    u32         head;                  // next byte to hand the FIFO
    u32         tail;                  // next slot to fill
    u32         count;
    bool        armed;                 // the IRQ-driven path is live (post-GIC)
    bool        writing;               // the writer role (one cons_output_write)
    u32         dropped;               // overrun + deadline drops (diagnostic)
    u32         room_waits;            // times a writer entered the room-wait (diagnostic)
    struct poll_waiter_list role_waiters;
};

static struct cons_tx g_cons_tx = {
    .lock         = SPIN_LOCK_INIT,
    .role_waiters = POLL_WAITER_LIST_INIT,
};

// Single-waiter by construction -- see the header comment.
static struct Rendez g_cons_tx_room = RENDEZ_INIT;

// #75-audit F6: count + writing are written under g_cons_tx.lock but read
// LOCKLESSLY in the tsleep conds (cons_tx_has_room / cons_tx_role_free) and the
// test hook -- a mixed atomic/non-atomic access to one object is a C11 data race
// (the cons_count_store/load precedent, cons.c above). RELAXED is sufficient:
// the no-lost-wake ordering comes from the rendez lock (a stale read only costs
// an extra cond re-check), not from these accesses.
static inline u32  tx_count_load(void)      { return __atomic_load_n(&g_cons_tx.count, __ATOMIC_RELAXED); }
static inline void tx_count_store(u32 v)    { __atomic_store_n(&g_cons_tx.count, v, __ATOMIC_RELAXED); }
static inline bool tx_writing_load(void)    { return __atomic_load_n(&g_cons_tx.writing, __ATOMIC_RELAXED); }
static inline void tx_writing_store(bool v) { __atomic_store_n(&g_cons_tx.writing, v, __ATOMIC_RELAXED); }

// Pop ring -> FIFO while the FIFO has room, then re-evaluate TXIM in the SAME
// critical section. Deciding the ring's state and the interrupt's state together
// is what makes "a non-empty ring left with TX interrupts off" -- the silent
// console wedge -- unrepresentable. Caller holds g_cons_tx.lock.
static void cons_tx_drain_locked(void) {
    while (tx_count_load() != 0u) {
        if (!uart_tx_try_putc((char)g_cons_tx.ring[g_cons_tx.head])) break;  // FIFO full
        g_cons_tx.head = (g_cons_tx.head + 1u) & (CONS_TX_RING_SIZE - 1u);
        tx_count_store(tx_count_load() - 1u);
    }
    uart_tx_irq_set_enabled(tx_count_load() != 0u);
}

// The TX IRQ arm (IRQ context, IRQs masked at PSTATE). Called from
// uart_irq_handler on MIS.TXMIS.
void cons_tx_drain_from_irq(void) {
    irq_state_t s = spin_lock_irqsave(&g_cons_tx.lock);
    uart_tx_irq_clear();                 // clear-first, the #172 RX discipline
    u32 before = tx_count_load();
    cons_tx_drain_locked();
    bool freed = (tx_count_load() < before);
    spin_unlock_irqrestore(&g_cons_tx.lock, s);

    // Wake OUTSIDE the ring lock (the cons_drain_tap discipline): wakeup() takes
    // the rendez lock. g_cons_tx.lock nests only the g_uart_imsc_lock LEAF (via
    // uart_tx_irq_set_enabled, which takes nothing further), so it is a leaf
    // w.r.t. every wait-lock; waking outside it keeps the order acyclic. wakeup()
    // is the only IRQ-safe wake primitive (LS-8a).
    if (freed) wakeup(&g_cons_tx_room);
}

// Arm the IRQ-driven path. Until this runs (pre-GIC boot), every byte takes the
// direct bounded uart_putc, so boot prints and the tooling-ABI banner are
// unaffected. The ring is empty at arm time, so the transition cannot reorder.
void cons_tx_arm(void) {
    irq_state_t s = spin_lock_irqsave(&g_cons_tx.lock);
    g_cons_tx.armed = true;
    spin_unlock_irqrestore(&g_cons_tx.lock, s);
}

// #75: flush the ring synchronously, bounded. The extinction / Halls path calls
// this before its own direct-DR dump so pre-crash ring output is not lost. It
// runs on a dying, IRQ-masked machine, so it must NOT wait on the IRQ and must
// NOT take the ring lock (a dying CPU may already hold it) -- it drains with a
// bounded trylock, then gives up. HX-I discipline: bounded, never recursing.
void cons_tx_flush_for_dump(void) {
    if (!spin_trylock(&g_cons_tx.lock)) return;   // a peer holds it: skip, do not wedge
    for (u32 i = 0; i < CONS_TX_RING_SIZE && tx_count_load() != 0u; i++) {
        if (!uart_tx_try_putc((char)g_cons_tx.ring[g_cons_tx.head])) break;
        g_cons_tx.head = (g_cons_tx.head + 1u) & (CONS_TX_RING_SIZE - 1u);
        tx_count_store(tx_count_load() - 1u);
    }
    spin_unlock(&g_cons_tx.lock);
    uart_tx_drain_sync();
}

// #75-audit F3: a bounded SYNCHRONOUS flush for a HEALTHY caller (unlike
// cons_tx_flush_for_dump's trylock, which is for the dying machine). #75 buffers
// EL0 output that drains lazily via the TX IRQ, so a residual ring can drain
// between the byte-by-byte uart_putc calls of the direct-path "Thylacine boot OK"
// banner and TEAR that tooling-ABI line (TOOLING.md section 10; a torn banner =
// a false boot-failure, the #74 gate-blindness class). Draining the ring to the
// wire before the banner closes that window. Bounded: each iteration drains >= 1
// FIFO or the ring is empty; uart_tx_drain_sync is itself bounded. Blocking-lock
// is safe here -- the banner runs in the quiescing boot context (cpu0), the ring
// is uncontended.
void cons_tx_flush(void) {
    for (u32 i = 0; i < CONS_TX_RING_SIZE + 1u; i++) {
        irq_state_t s = spin_lock_irqsave(&g_cons_tx.lock);
        cons_tx_drain_locked();          // ring -> FIFO (as much as fits) + TXIM re-eval
        bool empty = (tx_count_load() == 0u);
        spin_unlock_irqrestore(&g_cons_tx.lock, s);
        if (empty) return;
        uart_tx_drain_sync();            // FIFO full -> wait it out, then drain more
    }
}

// Non-blocking ring push. false == the ring is FULL (the caller drops or waits).
// Pre-arm this takes the direct bounded path and always succeeds.
static bool cons_tx_push_nowait(u8 b) {
    irq_state_t s = spin_lock_irqsave(&g_cons_tx.lock);
    if (!g_cons_tx.armed) {
        spin_unlock_irqrestore(&g_cons_tx.lock, s);
        uart_putc((char)b);            // pre-GIC boot path: direct, bounded (#67)
        return true;
    }
    if (tx_count_load() == CONS_TX_RING_SIZE) {
        spin_unlock_irqrestore(&g_cons_tx.lock, s);
        return false;
    }
    g_cons_tx.ring[g_cons_tx.tail] = b;
    g_cons_tx.tail = (g_cons_tx.tail + 1u) & (CONS_TX_RING_SIZE - 1u);
    tx_count_store(tx_count_load() + 1u);
    spin_unlock_irqrestore(&g_cons_tx.lock, s);
    return true;
}

// Hand the FIFO whatever it will take now + (re)arm TXIM for the remainder.
static void cons_tx_kick(void) {
    irq_state_t s = spin_lock_irqsave(&g_cons_tx.lock);
    if (g_cons_tx.armed) cons_tx_drain_locked();
    spin_unlock_irqrestore(&g_cons_tx.lock, s);
}

static void cons_tx_count_drop(void) {
    irq_state_t s = spin_lock_irqsave(&g_cons_tx.lock);
    g_cons_tx.dropped++;
    spin_unlock_irqrestore(&g_cons_tx.lock, s);
}

// Counts ENTRIES to the room-wait, not completed sleeps: the preceding kick may
// have freed a slot, in which case tsleep's cond is already true and returns at
// once. A rising count is the console telling you writers are back-pressuring on
// a slow consumer -- the diagnostic sibling of `dropped`, which counts the ones
// that gave up. Taking the ring lock here is free: the caller is about to sleep.
static void cons_tx_count_room_wait(void) {
    irq_state_t s = spin_lock_irqsave(&g_cons_tx.lock);
    g_cons_tx.room_waits++;
    spin_unlock_irqrestore(&g_cons_tx.lock, s);
}

static int cons_tx_has_room(void *arg) {
    (void)arg;
    return (int)(tx_count_load() < CONS_TX_RING_SIZE);
}

static int cons_tx_role_free(void *arg) {
    (void)arg;
    return (int)!tx_writing_load();
}

// Claim the writer role, parking until it frees. The audited chan_role_acquire
// (#354) shape. Returns 0 with the role HELD (caller MUST release), or
// TSLEEP_INTR on a #811 death-interrupt with the role NOT held.
static int cons_tx_role_acquire(void) {
    for (;;) {
        irq_state_t s = spin_lock_irqsave(&g_cons_tx.lock);
        if (!tx_writing_load()) {
            tx_writing_store(true);
            spin_unlock_irqrestore(&g_cons_tx.lock, s);
            return 0;
        }
        // Held -- park on the role list. register-then-observe: the hook is
        // registered under g_cons_tx.lock BEFORE the flag is re-sampled by
        // tsleep under the waiter's own rendez lock, so a concurrent release's
        // clear-then-wake is either captured by the cond re-check or delivered
        // to the registered hook (I-9; poll.tla). The stack Rendez/hook are
        // unregistered below before this frame pops (poll.tla NoStaleHook).
        struct Rendez      pr;
        struct poll_waiter pw;
        rendez_init(&pr);
        poll_waiter_init(&pw, &pr);
        poll_waiter_list_register(&g_cons_tx.role_waiters, &pw);
        spin_unlock_irqrestore(&g_cons_tx.lock, s);

        int ts = tsleep(&pr, cons_tx_role_free, NULL, 0);
        poll_waiter_list_unregister(&pw);
        if (ts == TSLEEP_INTR) return TSLEEP_INTR;
        // AWOKEN -- loop and re-contend (another writer may have won).
    }
}

// #75 test hooks. The RING is exercised by every byte of console output on
// every boot (a ring bug means no boot at all), so it needs no dedicated test;
// the ROLE does -- it is the mechanism that makes a write call-atomic, and its
// absence is silent until two CPUs happen to interleave. These let a test hold
// the role from one thread and prove a second writer PARKS rather than emitting.
void cons_test_tx_role_hold(void) {
    irq_state_t s = spin_lock_irqsave(&g_cons_tx.lock);
    tx_writing_store(true);
    spin_unlock_irqrestore(&g_cons_tx.lock, s);
}

bool cons_test_tx_role_held(void) {
    return tx_writing_load();
}

static void cons_tx_role_release(void) {
    irq_state_t s = spin_lock_irqsave(&g_cons_tx.lock);
    tx_writing_store(false);
    spin_unlock_irqrestore(&g_cons_tx.lock, s);
    // Clear-then-wake, outside the lock (the release side of register-then-observe).
    poll_waiter_list_wake(&g_cons_tx.role_waiters);
}

// #75 test hook -- the release half of cons_test_tx_role_hold.
void cons_test_tx_role_drop(void) { cons_tx_role_release(); }

// #75-audit F2 test hooks. The role has a test (above); the ROOM-WAIT and the
// #67 DEADLINE did not -- cons_emit_wait short-circuits on echo capture BEFORE
// cons_tx_push_nowait, so the role test never reaches the ring at all. These let
// a test stall the UART (uart_test_tx_stall), fill the ring for real, and drive
// both legs deterministically.
//
// cons_test_tx_ring_free discards `n` bytes from the ring HEAD *without* writing
// them to the UART. Discarding is what keeps the test HONEST about the console:
// its filler is a ring's worth, thousands of bytes, and a test that flooded the
// boot log to prove a point would be manufacturing the next harness-blindness
// bug (#74/#85/#87) in order to test this one.
//
// `wake` is the load-bearing parameter, not a convenience. Passing FALSE frees
// room WITHOUT waking, which is the only way to set up the state the I-9 leg
// actually needs: a writer still parked while the ring has room. From there the
// test unstalls and calls the REAL cons_tx_drain_from_irq over the couple of
// bytes left, so the wake under test is production's freed-gated wakeup() --
// not a re-implementation of it here. (tsleep re-checks its cond only on a wake,
// so a silent free leaves the sleeper parked exactly as required.)
u32 cons_test_tx_ring_free(u32 n, bool wake) {
    irq_state_t s = spin_lock_irqsave(&g_cons_tx.lock);
    u32 have  = tx_count_load();
    u32 freed = (n < have) ? n : have;
    g_cons_tx.head = (g_cons_tx.head + freed) & (CONS_TX_RING_SIZE - 1u);
    tx_count_store(have - freed);
    uart_tx_irq_set_enabled(tx_count_load() != 0u);
    spin_unlock_irqrestore(&g_cons_tx.lock, s);
    if (wake && freed != 0u) wakeup(&g_cons_tx_room);
    return freed;
}

u32 cons_test_tx_ring_count(void) {
    irq_state_t s = spin_lock_irqsave(&g_cons_tx.lock);
    u32 c = tx_count_load();
    spin_unlock_irqrestore(&g_cons_tx.lock, s);
    return c;
}

// Read under the lock: `dropped` is written under it, and a lockless read would
// be the C11 mixed-access race the F6 fix removed elsewhere in this file.
u32 cons_test_tx_dropped(void) {
    irq_state_t s = spin_lock_irqsave(&g_cons_tx.lock);
    u32 d = g_cons_tx.dropped;
    spin_unlock_irqrestore(&g_cons_tx.lock, s);
    return d;
}

// The park OBSERVABLE. Without it a test can only INFER that a writer parked
// ("it ran and has not finished"), which is both timing-dependent and vacuous if
// the test frees a slot before the writer reaches the wait. A rising count is
// proof it got there.
u32 cons_test_tx_room_waits(void) {
    irq_state_t s = spin_lock_irqsave(&g_cons_tx.lock);
    u32 w = g_cons_tx.room_waits;
    spin_unlock_irqrestore(&g_cons_tx.lock, s);
    return w;
}

bool cons_test_tx_armed(void) {
    irq_state_t s = spin_lock_irqsave(&g_cons_tx.lock);
    bool a = g_cons_tx.armed;
    spin_unlock_irqrestore(&g_cons_tx.lock, s);
    return a;
}

// Exported so the test asserts against the REAL constants instead of mirroring
// them -- a silently-drifted mirror is its own bug class (the struct t_stat
// lesson: a per-mirror size assert proves only that mirror's self-consistency).
u32 cons_test_tx_ring_capacity(void)  { return CONS_TX_RING_SIZE; }
u64 cons_test_tx_room_wait_ns(void)   { return CONS_TX_ROOM_WAIT_NS; }

// LS-8b: the echo / output sink. Console echo (cons_rx_input) AND program output
// (cons_output_write) emit one cooked byte through cons_emit. In production it is
// uart_putc; a test enables capture (cons_test_echo_capture) to buffer the bytes
// instead -- so a test can assert EXACTLY what was echoed AND the ECHO-off
// no-output property. g_cons_echo_capture is ALWAYS false in production (only the
// test hook sets it), so the production emit is a single never-taken branch then
// uart_putc; the capture buffer is single-threaded test state (the UP test
// harness drives it), never touched concurrently. CAVEAT (audit F3): the capture
// buffer is NOT lock-protected -- a test must never enable capture while a live
// UART RX IRQ could fire cons_rx_input -> cons_emit on another CPU (the LS-CI
// interactive harness in particular). At v1.0 capture is strictly UP test-time,
// so this cannot arise; the guard is the discipline, not a lock.
static u8   g_cons_echo_cap[128];
static u32  g_cons_echo_cap_len;
static bool g_cons_echo_capture;

static void cons_emit(u8 b) {
    // G-4: the drain tap fires FIRST and unconditionally of the capture mode
    // -- the tap models the renderer's view (which sees the byte stream
    // regardless of where the serial side lands), and a test with capture ON
    // can then assert drain content with the UART suppressed.
    cons_drain_tap(b);
    if (g_cons_echo_capture) {
        if (g_cons_echo_cap_len < sizeof(g_cons_echo_cap))
            g_cons_echo_cap[g_cons_echo_cap_len++] = b;
        return;
    }
    // #75: NON-BLOCKING. This is the IRQ-context (echo) contract -- it must
    // never sleep, so a full ring DROPS (a tty overrun, the same disposition
    // the drain ring uses). Kick immediately: echo is <= CONS_ECHO_MAX bytes
    // and typing latency is user-visible, so it does not batch.
    if (cons_tx_push_nowait(b)) cons_tx_kick();
    else                        cons_tx_count_drop();
}

// #75: the PROCESS-context emit used by cons_output_write. Pushes, and on a
// full ring kicks the FIFO then parks until the TX IRQ frees a slot.
//
// Returns false only when the write must be cut short -- a #811 death-interrupt
// or the #67 deadline against a stalled host consumer. The caller then returns a
// SHORT WRITE, which is POSIX-legal and is the inherited "bounded-but-lossy
// console beats a wedged writer" disposition; it must never become a hang.
//
// I-9 (no lost wake): cons_tx_kick re-evaluates ring + TXIM under the ring lock,
// and cons_tx_drain_from_irq wakes AFTER releasing that lock, so a slot freed in
// the window between our full-observation and our park is either seen by
// tsleep's cond re-check (under the rendez lock) or delivered to this rendez.
static bool cons_emit_wait(u8 b) {
    cons_drain_tap(b);
    if (g_cons_echo_capture) {
        if (g_cons_echo_cap_len < sizeof(g_cons_echo_cap))
            g_cons_echo_cap[g_cons_echo_cap_len++] = b;
        return true;
    }
    for (;;) {
        if (cons_tx_push_nowait(b)) return true;
        cons_tx_kick();
        cons_tx_count_room_wait();
        int ts = tsleep(&g_cons_tx_room, cons_tx_has_room, NULL,
                        timer_now_ns() + CONS_TX_ROOM_WAIT_NS);
        if (ts == TSLEEP_INTR) return false;            // #811 death -> short write
        if (ts == TSLEEP_TIMEDOUT) {                    // #67 stalled consumer
            cons_tx_count_drop();
            return false;                               // drop the rest -> short write
        }
    }
}

// ---------------------------------------------------------------------------
// #126: the NON-BLOCKING kernel diagnostic emit -- for a context that can
// neither sleep nor spin (IRQ context, or under a spinlock with IRQs masked).
//
// The direct arch emitters (uart_puts / uart_putdec / uart_puthex64) spin on a
// full TX FIFO for up to UART_TX_SPIN_MAX_NS PER BYTE before dropping it. That
// bound is per-BYTE and does NOT compose into a per-MESSAGE one, which is the
// whole bug: the #80 orphan diagnostic emits ~90 bytes back-to-back while
// holding g_proc_table_lock (which proc.c takes irqsave 40 times and plain 0
// times), so a stalled host consumer turned one adoption into ~1.8 s
// IRQ-masked with the global process-table lock held -- precisely the
// interrupt-dead stall #67's bound was introduced to prevent.
//
// These route the same bytes through the #75 TX ring instead: never spinning,
// dropping on a full ring (the echo disposition), kicking the FIFO once per
// call. A stalled consumer therefore costs a bounded handful of MMIO accesses,
// not seconds. They also feed the G-4 drain tap, so a kernel diagnostic reaches
// the framebuffer console and not only serial -- the #76 class, which every
// direct-path emit silently exhibits.
//
// CONTRACT: never sleeps, never spins, and takes only LEAF locks
// (g_cons_drain.lock; g_cons_tx.lock, which nests only the g_uart_imsc_lock
// leaf), waking outside them. So it is legal from IRQ context and from under
// any lock ordered above those -- the same path cons_emit already takes for
// echo from the UART RX IRQ, the most constrained context in the kernel.
//
// Pre-arm (early boot) cons_tx_push_nowait falls through to the direct bounded
// path, so output before cons_tx_arm() stays byte-identical.
// ---------------------------------------------------------------------------

// Deliberately does NOT consult g_cons_echo_capture (unlike cons_emit): that
// 128-byte buffer exists so a test can assert EXACTLY what was ECHOED, and a
// kernel diagnostic landing in it would corrupt the assertions it exists for.
static void cons_diag_byte(u8 b) {
    cons_drain_tap(b);
    if (!cons_tx_push_nowait(b)) cons_tx_count_drop();
}

void cons_diag_puts(const char *s) {
    if (!s) return;
    for (; *s; s++) {
        // ONLCR, matching uart_puts byte-for-byte: QEMU's `-serial mon:stdio`
        // host tty does no CR translation, and a bare LF staircases on aurora's
        // VT (#76).
        if (*s == '\n') cons_diag_byte((u8)'\r');
        cons_diag_byte((u8)*s);
    }
    cons_tx_kick();
}

void cons_diag_putdec(u64 v) {
    char buf[21];                       // max u64 is 20 decimal digits
    int  i = 0;
    if (v == 0) {
        cons_diag_byte((u8)'0');
    } else {
        while (v) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
        while (i > 0) cons_diag_byte((u8)buf[--i]);   // reversed: high digit first
    }
    cons_tx_kick();
}

void cons_diag_puthex64(u64 v) {
    static const char hexdigits[] = "0123456789abcdef";
    cons_diag_byte((u8)'0');
    cons_diag_byte((u8)'x');
    for (int i = 60; i >= 0; i -= 4) cons_diag_byte((u8)hexdigits[(v >> i) & 0xF]);
    cons_tx_kick();
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

// Free slots in the RX ring. Caller holds g_cons.lock, which is what makes this
// EXACT rather than advisory -- #129's whole correctness argument is that the
// room check and the push it authorizes happen in one lock hold, so no second
// producer can consume the room in between. cons_rx_can_accept() below is the
// lockless PRE-check; this is the decision.
static u32 cons_ring_room(void) {
    return CONS_RING_SIZE - cons_count_load();
}

// #174 / #129: the lockless PRE-check the PL011 drain runs BEFORE reading a byte
// out of the FIFO -- on false it leaves the byte in the FIFO and masks RX rather
// than pulling in a byte the ring cannot take.
//
// #129 makes it reserve the WORST-CASE COOKED FLUSH (CONS_LINE_MAX + 1), not one
// byte. The one-byte form was the bug: it answered "is there room for this
// byte?" when the question the drain is really asking is "is there room for
// everything this byte will cause?", and in canonical mode a single admitted
// byte can be the terminator that flushes an entire assembled line. That is the
// same category error as #126 on the TX side -- a per-ITEM bound standing in for
// a per-OPERATION bound -- and it reads as correct right up until the item and
// the operation differ by 256x.
//
// Deliberately UNCONDITIONAL (it does not consult ICANON or line_len):
//   - Reading line_len here would be a lockless read of a field another producer
//     mutates, so a stale-low value would re-open exactly the hole being closed.
//   - Gating on the CURRENT mode is not sound either: termios can flip between
//     this check and the push, so a byte admitted under a raw-mode check can
//     meet a cooked-mode flush. Reserving unconditionally is immune to both.
// The cost is 257 slots of headroom, which is why CONS_RING_SIZE doubled: usable
// depth before pausing stays 255 bytes, unchanged from the pre-#129 ring.
//
// This is now an OPTIMIZATION, not the correctness mechanism. It keeps the
// common case off the refusal path; cons_rx_input's under-lock room check is
// what actually guarantees no overrun, and the drain's 1-byte holdback is what
// makes a refusal lossless. Being stale is therefore harmless in both
// directions: stale-true costs one refusal-and-hold, stale-false pauses early.
bool cons_rx_can_accept(void) {
    return cons_count_load() + CONS_LINE_MAX + 1u <= CONS_RING_SIZE;
}

// #95: record ONE dropped input byte at `site` and arm console_mgr's deferred
// one-shot report. Caller holds g_cons.lock (every drop site does). The mgr wake
// must be armed HERE and not left to cons_ring_push's empty->non-empty edge: a
// drop means the ring was FULL, so there is no edge, and without this the report
// would sit pending until some unrelated byte happened to arrive.
// The REPORT (not the count) is gated on boot-complete, because the kernel test
// suite deliberately overflows this ring -- cons.ring_overflow_drop pushes 266
// bytes into 256, and cons.rx_drop_counters drives all three sites on purpose.
// Ungated, every boot would print an alarming INPUT DROP line during the test
// phase, and -- far worse -- the test would SPEND the one-shot latch, so a real
// drop later in the same boot would print nothing. The instrument would be
// disarmed by its own test. Kernel tests run before boot-complete and every
// real input workload runs after, so this gate makes the test phase silent and
// leaves the latch armed for exactly the window that matters. (Counting is
// unconditional -- that is what the test asserts on, and cons_test_reset zeroes
// it afterwards.)
static void cons_rx_note_drop(u32 *site, bool *wake_mgr) {
    (*site)++;
    if (!g_cons.drop_reported && boot_is_complete()) {
        cons_dropreport_store(true);
        *wake_mgr = true;
    }
}

bool cons_rx_input(u8 byte, bool is_break) {
    bool wake_data = false, wake_mgr = false;
    bool accepted = true;
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
                // terminating newline, so this pushes line_len + 1 bytes in ONE
                // call -- the largest atomic push the ring ever takes.
                //
                // #129: check room for the WHOLE flush before pushing any of it,
                // under this lock. Pre-#129 this pushed until the ring filled and
                // counted the remainder as dropped, and it dropped the TAIL --
                // including the newline -- so the line silently became a
                // different, shorter line with no terminator.
                if (cons_ring_room() < g_cons.line_len + 1u) {
                    // REFUSE, changing nothing: the line stays assembled, the
                    // terminator is not consumed, nothing is echoed. The producer
                    // holds this byte and re-offers it once the reader drains.
                    // Leaving line_len intact is the load-bearing half -- zeroing
                    // it here would destroy the line the refusal exists to save.
                    g_cons.rx_bp_flush++;
                    accepted = false;
                } else {
                    for (u32 i = 0; i < g_cons.line_len; i++)
                        if (cons_ring_push(g_cons.line[i], &wake_mgr)) wake_data = true;
                        else cons_rx_note_drop(&g_cons.rx_drop_ring, &wake_mgr);
                    if (cons_ring_push((u8)'\n', &wake_mgr)) wake_data = true;
                    else cons_rx_note_drop(&g_cons.rx_drop_ring, &wake_mgr);
                    g_cons.line_len = 0u;
                    if (tio & CONS_ECHO) cons_echo_stage((u8)'\n', tio, echo, &necho);
                }
            } else {                                  // ordinary char: buffer it
                if (g_cons.line_len < CONS_LINE_MAX) {
                    g_cons.line[g_cons.line_len++] = byte;
                    if (tio & CONS_ECHO) cons_echo_stage(byte, tio, echo, &necho);
                } else {
                    // Line buffer full -> drop (bounded; Enter still delivers
                    // what fits). A dropped byte is NOT echoed. #95: counted,
                    // because "Enter still delivers what fits" is exactly the
                    // truncated-command shape and was previously silent.
                    cons_rx_note_drop(&g_cons.rx_drop_line, &wake_mgr);
                }
            }
        } else {
            // Raw / cbreak mode: byte-at-a-time to the ring (the pre-LS-8b path).
            // #129: refuse rather than drop when the ring is full. The echo moves
            // INSIDE the accepted branch -- echoing a refused byte would show the
            // user a character the console did not take, and would then show it
            // twice when the producer re-offers it.
            if (cons_ring_room() == 0u) {
                g_cons.rx_bp_raw++;
                accepted = false;
            } else {
                if (cons_ring_push(byte, &wake_mgr)) wake_data = true;
                else cons_rx_note_drop(&g_cons.rx_drop_ring, &wake_mgr);
                if (tio & CONS_ECHO) cons_echo_stage(byte, tio, echo, &necho);
            }
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
    return accepted;
}

// cond: the ring holds at least one byte. Runs under the Rendez lock (NOT
// g_cons.lock), so the count read is a RELAXED atomic (see the cons_count_*
// rationale); the Rendez lock provides the no-lost-wakeup pairing.
static int cons_data_ready(void *arg) {
    (void)arg;
    return cons_count_load() > 0u;
}

// #58 test-only hold: while set, console_mgr's wake cond reads false, so a
// woken mgr RE-PARKS without consuming any pending flag (the flags persist;
// the release path wakes explicitly, so no wake is lost -- I-9 intact). Lets
// the deferred-wake tests run their register/produce/assert dance
// DETERMINISTICALLY on SMP: without it, a woken mgr dispatched on a PEER CPU
// consumed the pending flag between the producer byte and the assert
// (~1-in-50 HVF boots), the TEST_ASSERT early-return then LEAKED the test's
// stack poll_waiter on the list, and the next walk extincted on the reused
// frame's clobbered magic (EXTINCTION: pw_wake, 2026-07-21). Production never
// sets it -- the compiled-in cons_test_* family posture. Plain bool + the
// __atomic_* builtins (the tree idiom; the builtins reject _Atomic-qualified
// objects).
static bool g_cons_mgr_hold;

// cond: a deferred console action is pending (a Ctrl-C interrupt OR an A-4c-2
// SAK). Same lockless-under-Rendez-lock discipline as cons_data_ready.
static int cons_mgr_pending(void *arg) {
    (void)arg;
    if (__atomic_load_n(&g_cons_mgr_hold, __ATOMIC_ACQUIRE)) return 0; // #58
    return cons_intr_load() || cons_sak_load() || cons_pollwake_load()
        || drain_pollwake_load()    // G-4: a drain POLLIN edge awaits the walk
        || cons_dropreport_load();  // #95: an input byte was dropped, unreported
}

void cons_test_mgr_hold(bool on) {
    __atomic_store_n(&g_cons_mgr_hold, on, __ATOMIC_RELEASE);
    if (!on) wakeup(&g_cons_mgr_rendez);   // release re-arms the mgr
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
    // #95: snapshot the drop counts under the lock and latch the report off, so
    // the emit below runs exactly once for the life of the boot no matter how
    // many further drops occur.
    bool do_drop_report = cons_dropreport_load();
    u32  d_line = g_cons.rx_drop_line, d_ring = g_cons.rx_drop_ring;
    u32  b_raw = g_cons.rx_bp_raw, b_flush = g_cons.rx_bp_flush;
    cons_dropreport_store(false);
    if (do_drop_report) g_cons.drop_reported = true;
    spin_unlock_irqrestore(&g_cons.lock, s);

    // #95: the loud one-shot. A dropped input byte is a silent correctness event
    // -- a command loses a character and runs anyway -- so it must announce
    // itself in the log the moment it happens, not wait for someone to think to
    // read /ctl/cons. cons_diag_* is the #126 non-blocking emitter (TX ring, no
    // spin, no sleep), and this runs with g_cons.lock RELEASED like every other
    // deferred action.
    // #129: only a REAL loss arms this latch. The two ring-full sites now
    // back-pressure, and back-pressure is normal operation on a busy console --
    // reporting it would be a false alarm AND would spend the one-shot latch, so
    // a genuine loss later in the same boot would print nothing. (That is the
    // #95 lesson applied to the fix itself: an instrument disarmed by routine
    // events is disarmed exactly when it matters.) The bp counts ride along in
    // the line as context -- they say how hard the console was pushed when the
    // loss happened -- but they never trigger it.
    if (do_drop_report) {
        cons_diag_puts("cons: INPUT DROP (#95) line=");
        cons_diag_putdec(d_line);
        cons_diag_puts(" ring=");
        cons_diag_putdec(d_ring);
        cons_diag_puts(" (bp raw=");
        cons_diag_putdec(b_raw);
        cons_diag_puts(" flush=");
        cons_diag_putdec(b_flush);
        cons_diag_puts(") -- further drops counted silently at /ctl/cons\n");
    }

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

    // G-4: the drain's deferred poll-wake -- the SAME relay, second instance
    // (the drain tap can fire in IRQ context via the echo path). The flag is
    // drained under the DRAIN lock (its own leaf), the walk lock-free after.
    bool do_drain_poll;
    irq_state_t ds = spin_lock_irqsave(&g_cons_drain.lock);
    do_drain_poll = drain_pollwake_load();
    drain_pollwake_store(false);
    spin_unlock_irqrestore(&g_cons_drain.lock, ds);
    if (do_drain_poll) poll_waiter_list_wake(&g_cons_drain.poll_list);
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
    g_cons.ws_cols = 0u;                        // #55: winsize back to unset
    g_cons.ws_rows = 0u;
    g_cons.winch_events = 0u;
    g_cons.rx_bp_raw = 0u;                      // #95/#129: RX counters + report latch
    g_cons.rx_bp_flush = 0u;
    g_cons.rx_drop_line = 0u;
    g_cons.rx_drop_ring = 0u;
    cons_dropreport_store(false);
    g_cons.drop_reported = false;
    spin_unlock_irqrestore(&g_cons.lock, s);

    // G-4: the drain back to the boot (disarmed, empty) state.
    s = spin_lock_irqsave(&g_cons_drain.lock);
    g_cons_drain.open = false;
    drain_armed_store(false);
    g_cons_drain.head = g_cons_drain.tail = 0u;
    drain_count_store(0u);
    g_cons_drain.overflow = 0u;
    g_cons_drain.reader_busy = false;
    drain_pollwake_store(false);
    spin_unlock_irqrestore(&g_cons_drain.lock, s);
}

// #95: the TX-side counts, for /ctl/cons. Same values the cons_test_tx_*
// accessors return; a separate production-named entry point so the surface does
// not call a `_test_` symbol. The TX counters have existed since #75/#126 with
// no surface at all -- you had to be writing a kernel unit test to see them,
// which is the same "had to know where to look" problem #95 is about.
void cons_tx_drops(u32 *dropped, u32 *room_waits) {
    irq_state_t s = spin_lock_irqsave(&g_cons_tx.lock);
    if (dropped)    *dropped    = g_cons_tx.dropped;
    if (room_waits) *room_waits = g_cons_tx.room_waits;
    spin_unlock_irqrestore(&g_cons_tx.lock, s);
}

// #95/#129: the RX-path counts. Read under the lock -- they are written under
// it, and a lockless read would be the C11 mixed-access race the F6 fix removed
// elsewhere in this file. NULL args are skipped so a caller can ask for one.
//
// RENAMED at #129 (was cons_rx_drops(raw, flush, line)) because two of the three
// stopped meaning "dropped": the ring-full sites now back-pressure. Keeping the
// old name would have left every caller, every doc, and every future reader
// believing input was lost where it was merely deferred.
void cons_rx_counters(u32 *bp_raw, u32 *bp_flush, u32 *drop_line, u32 *drop_ring) {
    irq_state_t s = spin_lock_irqsave(&g_cons.lock);
    if (bp_raw)    *bp_raw    = g_cons.rx_bp_raw;
    if (bp_flush)  *bp_flush  = g_cons.rx_bp_flush;
    if (drop_line) *drop_line = g_cons.rx_drop_line;
    if (drop_ring) *drop_ring = g_cons.rx_drop_ring;
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

// #130-R2 F2: the harness backstop for console state a test OWNS for a window.
//
// Every one of these is armed by one call and released by another, with the
// test's body in between -- and TEST_ASSERT is `test_fail(); return;`, so ANY
// failing assert inside such a window skips the release. The consequences are
// not "one red test":
//
//   ECHO_CAPTURE  cons_emit / cons_emit_wait divert into a 128-byte buffer and
//                 RETURN, so every later /dev/cons write is swallowed -- the
//                 login prompt, the shell, the LS-CI transcript. SILENT: kernel
//                 diagnostics take cons_diag_byte, which ignores capture, so the
//                 suite keeps reporting PASS over a dead userspace console.
//   TX_ROLE       cons_tx_role_acquire parks contenders UNTIMED, so every later
//                 console writer parks forever: the boot hangs.
//   MGR_HOLD      console_mgr stops servicing deferred work; poll wakes strand.
//   READER_BUSY   the single-reader guard refuses every later devcons_read.
//
// So a test's failure destroys the diagnosis of everything after it -- the exact
// shape #74/#85/#87 are in other clothes. TEST_YIELD_UNTIL_SOFT was the
// per-site answer and it only covers the wait; an ordinary assert in the window
// is the far commoner case, and per-site discipline cannot cover a site that
// does not exist yet.
//
// Releasing here is safe unconditionally (each release is idempotent), but the
// bitmask is the point: a silent auto-repair would hide the leak it repaired.
// The caller reports what it had to clean up and FAILS the offending test --
// make the operation report its effect, never predict it.
u32 cons_test_release_owned_state(void) {
    u32 owned = 0;

    if (g_cons_echo_capture) {
        owned |= CONS_TEST_OWNED_ECHO_CAPTURE;
        cons_test_echo_capture(false);
    }
    if (cons_test_tx_role_held()) {
        owned |= CONS_TEST_OWNED_TX_ROLE;
        cons_test_tx_role_drop();
    }
    if (__atomic_load_n(&g_cons_mgr_hold, __ATOMIC_ACQUIRE)) {
        owned |= CONS_TEST_OWNED_MGR_HOLD;
        cons_test_mgr_hold(false);
    }
    irq_state_t s = spin_lock_irqsave(&g_cons.lock);
    bool busy = g_cons.reader_busy;
    if (busy) g_cons.reader_busy = false;
    spin_unlock_irqrestore(&g_cons.lock, s);
    if (busy) owned |= CONS_TEST_OWNED_READER_BUSY;

    return owned;
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
        // #174: having freed ring space, resume RX backpressure -- drain any FIFO
        // bytes the handler held while the ring was full + unmask RX. No-op (one
        // atomic read) when RX is not paused. Runs with g_cons.lock RELEASED:
        // uart_rx_pump takes g_uart_rx_lock then re-enters cons_rx_input
        // (g_cons.lock), so the lock order g_uart_rx_lock -> g_cons.lock holds and
        // there is no g_cons.lock -> g_uart_rx_lock edge. The cons single-reader
        // guard (reader_busy) means at most one pump runs at a time.
        uart_rx_pump();
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

    // #75 (ARCH §23.5.2): hold the writer role across the WHOLE call so this
    // write's bytes are contiguous with respect to every other console writer.
    // Without it two CPUs interleave at byte granularity and tear multi-byte
    // glyphs and escape sequences. The role is a sleeping park, NOT a spinlock:
    // a contending writer parks on the role list, so a long write makes peers
    // wait but never pins a CPU.
    if (cons_tx_role_acquire() != 0) return -1;   // #811 death before we wrote anything

    u32 tio = cons_termios_load();
    const u8 *bytes = (const u8 *)buf;
    long i = 0;
    for (; i < n; i++) {
        if (bytes[i] == (u8)'\n' && (tio & CONS_ONLCR)) {
            if (!cons_emit_wait((u8)'\r')) break;
            if (!cons_emit_wait((u8)'\n')) break;
        } else {
            if (!cons_emit_wait(bytes[i])) break;
        }
    }

    // Hand the FIFO whatever it can take now (the healthy case moves every byte
    // here and never arms TXIM) and arm the interrupt for any remainder.
    cons_tx_kick();
    cons_tx_role_release();
    return i;   // a short count == the #67 deadline drop or a #811 death unwind
}

static long devcons_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)c; (void)off;
    return cons_output_write(buf, n);
}

// =============================================================================
// G-4: the renderer drain/feed API (the /dev/consdrain + /dev/consfeed leaves,
// devdev). Reached only by the bound console renderer (the devdev open + I/O
// gates enforce proc_is_console_renderer).
// =============================================================================

// Arm the drain: the renderer's open of /dev/consdrain. Single-open (a second
// concurrent open returns -1 -- one drain fid at a time; the fid may be
// dup/inherited like any fd, the open is the mint gate). A fresh open starts a
// FRESH epoch: stale bytes from a dead renderer's epoch are discarded so the
// new holder never renders another epoch's tail.
int cons_drain_open(void) {
    irq_state_t s = spin_lock_irqsave(&g_cons_drain.lock);
    if (g_cons_drain.open) {
        spin_unlock_irqrestore(&g_cons_drain.lock, s);
        return -1;
    }
    g_cons_drain.open = true;
    g_cons_drain.head = g_cons_drain.tail = 0u;
    drain_count_store(0u);
    g_cons_drain.overflow = 0u;
    // Resetting reader_busy here is sound because no reader from the PRIOR
    // epoch can still be in flight: an in-flight cons_drain_read runs under
    // the read syscall's #844 obj pin on the drain Spoor, so the close hook
    // (the only disarm) cannot have run while it was active; and on the
    // death path a parked reader is #811-unwound (and never re-parks under
    // a pending die) BEFORE the #926/#68 close-at-exit closes the fid. A
    // stale reader surviving into a fresh epoch would race this ring and
    // make the single-waiter drain Rendez two-sleeper (an extinction) --
    // the pin + death ordering exclude it structurally.
    g_cons_drain.reader_busy = false;
    drain_pollwake_store(false);
    drain_armed_store(true);
    spin_unlock_irqrestore(&g_cons_drain.lock, s);
    return 0;
}

// Disarm: the drain fid's last close (devdev close hook; also fires via the
// #926/#68 close-at-exit when the renderer dies). Runs in process context
// (handle close paths only), so waking the parked reader + walking the poll
// hook list directly is legal (the console_mgr precedent). A reader parked
// mid-close observes !armed via the sleep cond and returns EOF.
void cons_drain_close(void) {
    irq_state_t s = spin_lock_irqsave(&g_cons_drain.lock);
    g_cons_drain.open = false;
    drain_armed_store(false);
    spin_unlock_irqrestore(&g_cons_drain.lock, s);
    wakeup(&g_cons_drain_rendez);
    poll_waiter_list_wake(&g_cons_drain.poll_list);
}

// cond: drain data ready OR the drain was disarmed (EOF). Lockless RELAXED
// reads under the Rendez lock (the cons_data_ready discipline).
static int cons_drain_ready(void *arg) {
    (void)arg;
    return drain_count_load() > 0u || !drain_armed_load();
}

// Blocking drain read (the renderer's output stream). Mirrors cons_input_read:
// single-reader busy-guard; drains what is buffered (>= 1 byte) or parks on
// the drain Rendez (death-interruptible per #811); returns 0 (EOF) when the
// drain is disarmed with nothing buffered, -1 on bad args / reader-busy /
// never-armed.
long cons_drain_read(void *buf, long n) {
    if (!buf || n < 0) return -1;
    if (n == 0)        return 0;

    irq_state_t s = spin_lock_irqsave(&g_cons_drain.lock);
    if (!g_cons_drain.open || g_cons_drain.reader_busy) {
        spin_unlock_irqrestore(&g_cons_drain.lock, s);
        return -1;
    }
    g_cons_drain.reader_busy = true;
    spin_unlock_irqrestore(&g_cons_drain.lock, s);

    // RW-11 SA-1b applied to the renderer: the drain reader IS the display --
    // its wake (output arrived) should preempt NORMAL work so a keystroke's
    // echo paints promptly. The gate is structural (only the bound renderer
    // can reach this read) + narrow (band==NORMAL pre-check keeps the promoted
    // case off the locking query).
    struct Thread *reader = current_thread();
    if (reader && reader->band == SCHED_BAND_NORMAL && reader->proc &&
        proc_is_console_renderer(reader->proc)) {
        sched_mark_interactive(reader);
    }

    u8 *out = (u8 *)buf;
    long got = 0;
    for (;;) {
        s = spin_lock_irqsave(&g_cons_drain.lock);
        u32 c = drain_count_load();
        while (c > 0u && got < n) {
            out[got++] = g_cons_drain.ring[g_cons_drain.head];
            g_cons_drain.head = (g_cons_drain.head + 1u) & (CONS_DRAIN_RING_SIZE - 1u);
            c--;
        }
        drain_count_store(c);
        spin_unlock_irqrestore(&g_cons_drain.lock, s);
        if (got > 0) break;                    // >= 1 byte satisfies the read
        if (!drain_armed_load()) break;        // disarmed + empty -> EOF (0)
        if (sleep(&g_cons_drain_rendez, cons_drain_ready, NULL) == SLEEP_INTR) break;
    }

    s = spin_lock_irqsave(&g_cons_drain.lock);
    g_cons_drain.reader_busy = false;
    spin_unlock_irqrestore(&g_cons_drain.lock, s);
    return got;
}

// Drain poll: POLLIN iff buffered bytes exist OR the drain is disarmed (a
// disarmed drain reads EOF immediately -- readable by poll semantics). The
// drain leaf is read-only, so no POLLOUT. Register-then-observe under the
// drain lock (the cons_poll discipline; the hook list is walked deferred by
// console_mgr for IRQ-context edges, directly by cons_drain_close).
short cons_drain_poll(short events, struct poll_waiter *pw) {
    short revents = 0;
    irq_state_t s = spin_lock_irqsave(&g_cons_drain.lock);
    if (events & POLLIN) {
        if (drain_count_load() > 0u || !g_cons_drain.open) revents |= POLLIN;
    }
    if (pw) poll_waiter_list_register(&g_cons_drain.poll_list, pw);
    spin_unlock_irqrestore(&g_cons_drain.lock, s);
    return revents;
}

// The feed: the renderer's decoded keyboard bytes enter the EXISTING LS-8 line
// discipline EXACTLY as UART RX bytes do -- cooking, ECHO (whose emit lands in
// BOTH the UART and the drain -- the renderer paints its own echo), ISIG (a
// graphical Ctrl-C posts `interrupt` to the console OWNER via the LS-5 path),
// ICRNL, canonical assembly: all unchanged and backend-independent.
//
// I-27 (load-bearing, by construction): `is_break` is HARDWIRED false. A serial
// BREAK is a PL011 LINE CONDITION -- the one unforgeable SAK trigger -- and no
// feed byte sequence can synthesize it, so the renderer (untrusted for
// elevation) can never fire the SAK. The graphical SAK is a KERNEL-scanned
// trusted-tier keyboard combo (MENAGERIE section 7), a board-era surface;
// on QEMU media the trusted path stays serial (TAPESTRY section 18.7).
// #129: the feed is the SECOND producer into the RX ring, and until now the only
// UNGATED one. #174 gave the PL011 drain proper back-pressure (check, then leave
// the byte in the FIFO), but this path -- the graphical console's entire keyboard
// -- just called cons_rx_input per byte unconditionally and returned n whatever
// happened, so a full ring silently ate keystrokes and told the renderer they
// had landed. The serial console had back-pressure; the framebuffer console had
// a lie.
//
// Now it stops at the first refusal and returns a SHORT count. That is the
// ordinary POSIX answer for a write to a device with a full buffer, so it needs
// no new ABI and no renderer change beyond honoring the count it is already
// contractually obliged to honor. Refusal leaves the console byte-for-byte
// unchanged (see cons_rx_input), so the caller's retry re-offers the identical
// byte and nothing is lost or duplicated.
//
// Returning 0 (the very first byte refused) is a legal short write, NOT an
// error: a blocking writer loops, and the reader's drain frees room. It is
// deliberately not -EAGAIN -- this Dev has no non-blocking contract to hang that
// on, and an errno here would make the renderer treat back-pressure as failure.
long cons_feed_write(const void *buf, long n) {
    if (!buf || n < 0) return -1;
    const u8 *bytes = (const u8 *)buf;
    long i = 0;
    for (; i < n; i++)
        if (!cons_rx_input(bytes[i], /*is_break=*/false)) break;
    return i;
}

// #129-audit F2: the RX ring depth, NON-BLOCKING. A test cannot use cons_drain
// (= devcons.read) to observe emptiness, because that PARKS on an empty ring --
// so an assertion that should fail cleanly instead hangs the boot (the #133
// class: a test's failure must not destroy the run). The TX ring has had
// cons_test_tx_ring_count since #75; the RX side had nothing.
u32 cons_test_rx_count(void) { return cons_count_load(); }

u32 cons_test_drain_count(void)    { return drain_count_load(); }
u32 cons_test_drain_overflow(void) {
    irq_state_t s = spin_lock_irqsave(&g_cons_drain.lock);
    u32 v = g_cons_drain.overflow;
    spin_unlock_irqrestore(&g_cons_drain.lock, s);
    return v;
}
bool cons_test_drain_pollwake_pending(void) { return drain_pollwake_load(); }

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

// #55: parse one whitespace-delimited decimal token in [0, 65535] starting at
// *i (whitespace already skipped). Advances *i past the token. Returns the
// value, or -1 on malformed (empty / non-digit / overflow).
static long cons_parse_u16_token(const u8 *b, long n, long *i) {
    long j = *i;
    long v = 0;
    long digits = 0;
    while (j < n && !cons_is_space(b[j])) {
        u8 ch = b[j];
        if (ch < (u8)'0' || ch > (u8)'9') return -1;
        v = v * 10 + (long)(ch - (u8)'0');
        if (v > 65535) return -1;                            // the u16 band
        digits++;
        j++;
    }
    if (digits == 0) return -1;
    *i = j;
    return v;
}

long cons_set_mode_cmd(const void *buf, long n, bool allow_flags) {
    if (!buf || n < 0) return -1;
    const u8 *b = (const u8 *)buf;

    // Parse ALL tokens first (atomic apply): a single malformed token rejects
    // the whole write with no change (the tcsetattr-is-atomic seam).
    u32 set_mask = 0u, clear_mask = 0u;
    int tokens = 0;
    bool have_ws = false;                                     // #55 winsize staged
    long ws_cols = 0, ws_rows = 0;
    long i = 0;
    while (i < n) {
        while (i < n && cons_is_space(b[i])) i++;            // skip whitespace
        if (i >= n) break;
        u8 sign = b[i];
        // #55 (ARCH 23.5.3): the `winsize <cols> <rows>` verb -- the ptyfs
        // PTY-2c grammar, byte-identical. Staged with the flag masks so the
        // whole write stays atomic (a malformed winsize rejects everything).
        if (sign == (u8)'w' && i + 7 <= n &&
            b[i+1]==(u8)'i' && b[i+2]==(u8)'n' && b[i+3]==(u8)'s' &&
            b[i+4]==(u8)'i' && b[i+5]==(u8)'z' && b[i+6]==(u8)'e' &&
            (i + 7 == n || cons_is_space(b[i+7]))) {
            i += 7;
            while (i < n && cons_is_space(b[i])) i++;
            ws_cols = cons_parse_u16_token(b, n, &i);
            if (ws_cols < 0) return -1;
            while (i < n && cons_is_space(b[i])) i++;
            ws_rows = cons_parse_u16_token(b, n, &i);
            if (ws_rows < 0) return -1;
            have_ws = true;
            tokens++;
            continue;
        }
        if (sign != (u8)'+' && sign != (u8)'-') return -1;   // malformed token
        // #55 audit F2: a renderer-minted consctl (CCONSWINSZONLY) may write
        // ONLY the winsize verb -- a `+`/`-` flag token rejects the whole
        // write, so a compromised renderer cannot flip the global termios
        // (the ECHO-off serial-input mask defeat).
        if (!allow_flags) return -1;
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

    bool winch = false;                                       // #55 changed?
    irq_state_t s = spin_lock_irqsave(&g_cons.lock);
    u32 cur = cons_termios_load();
    cons_termios_store((cur | set_mask) & ~clear_mask);
    if (have_ws) {
        // #55: iff-changed (the Linux TIOCSWINSZ / ptyfs semantics). An
        // unchanged rewrite must NOT post -- a repeat-post storm would be a
        // notes-queue DoS on the owner's pgrp (the 25.4 row obligation).
        if (g_cons.ws_cols != (u16)ws_cols || g_cons.ws_rows != (u16)ws_rows) {
            g_cons.ws_cols = (u16)ws_cols;
            g_cons.ws_rows = (u16)ws_rows;
            g_cons.winch_events++;
            winch = true;
        }
    }
    // A mode change starts a FRESH canonical line (the TCSAFLUSH discipline):
    // discard any half-assembled line[] so a canonical->raw->canonical flip can
    // never strand a fragment that then prepends the next line. This matches the
    // test hook (cons_test_set_termios) -- without it the production consctl
    // path and the test path diverge (the cooking tests would not catch a
    // fragment-survival regression). No current consumer flips mid-line (login
    // flips between completed reads; ut at prompt boundaries), but the kernel
    // must be unambiguous against any consctl writer.
    g_cons.line_len = 0u;
    spin_unlock_irqrestore(&g_cons.lock, s);

    // #55: the tty:winch post runs AFTER g_cons.lock drops -- no g_cons.lock
    // -> g_proc_table_lock edge (the 25.4 row's ordering obligation). This is
    // process context (a consctl write; NEVER reachable from cons_rx_input/
    // IRQ), so no console_mgr deferral is needed, unlike the ISIG cook.
    if (winch) proc_console_post_winch();
    return n;
}

// #55: append the decimal rendering of v (known <= 65535) at out[*off].
// The caller has bounds-checked (max 5 digits).
static void cons_render_dec_u16(u8 *out, long *off, u32 v) {
    char tmp[6];
    int t = 0;
    do { tmp[t++] = (char)('0' + (v % 10u)); v /= 10u; } while (v != 0u);
    while (t > 0) out[(*off)++] = (u8)tmp[--t];
}

long cons_render_mode(void *buf, long n) {
    if (!buf || n < 0) return 0;
    u16 wc, wr;
    cons_winsize_get(&wc, &wr);                              // coherent pair
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
        out[off++] = (u8)' ';                                // #55: winsize follows
    }
    // #55: `winsize <cols> <rows>` closes the line (the ptyfs ctl_render
    // shape: "+icanon ... +onlcr winsize 80 24\n" -- parser parity, pouch
    // 0021's strstr(buf, "winsize ") works on either ctl).
    if (off + 8 + 5 + 1 + 5 + 1 > n) return 0;               // "winsize " CCCCC ' ' RRRRR '\n'
    const char ws[] = "winsize ";
    for (long k = 0; ws[k]; k++) out[off++] = (u8)ws[k];
    cons_render_dec_u16(out, &off, wc);
    out[off++] = (u8)' ';
    cons_render_dec_u16(out, &off, wr);
    out[off++] = (u8)'\n';
    return off;
}

// #55: the coherent winsize snapshot (one g_cons.lock hold -- a reader must
// never see a torn (cols, rows) pair across a concurrent verb apply).
void cons_winsize_get(u16 *cols, u16 *rows) {
    irq_state_t s = spin_lock_irqsave(&g_cons.lock);
    if (cols) *cols = g_cons.ws_cols;
    if (rows) *rows = g_cons.ws_rows;
    spin_unlock_irqrestore(&g_cons.lock, s);
}

// #55: the standalone `winsize <cols> <rows>\n` line the UNGATED /dev/winsize
// leaf serves (readback for apps that cannot mint consctl). Unset renders
// `winsize 0 0` -- the serial posture: never an error; readers fall back to
// the CPR probe (which the host terminal answers on serial).
long cons_render_winsize(void *buf, long n) {
    if (!buf || n < 20) return 0;   // #55 audit F4: "winsize 65535 65535\n" = 20 bytes exactly
    u16 wc, wr;
    cons_winsize_get(&wc, &wr);
    u8 *out = (u8 *)buf;
    long off = 0;
    const char ws[] = "winsize ";
    for (long k = 0; ws[k]; k++) out[off++] = (u8)ws[k];
    cons_render_dec_u16(out, &off, wc);
    out[off++] = (u8)' ';
    cons_render_dec_u16(out, &off, wr);
    out[off++] = (u8)'\n';
    return off;
}

u32 cons_winch_events(void) {
    irq_state_t s = spin_lock_irqsave(&g_cons.lock);
    u32 v = g_cons.winch_events;
    spin_unlock_irqrestore(&g_cons.lock, s);
    return v;
}

// #55: the shared is-a-cons t_stat fill (ARCH 23.5.3) -- devcons (the
// SYS_CONSOLE_OPEN fd, the std-fd inheritance chain) + devdev's /dev/cons
// leaf report ONE contract: zero-fill (I-13 -- no stack garbage crosses),
// T_S_IFCHR posture (the same posture ptyfs presents, so pouch 0021's
// S_ISCHR pre-gate passes), SYSTEM-owned, and qid_path carrying
// CONS_STAT_QID_FLAG (bit 41 -- DISJOINT from ptyfs's PTS_FLAG bit 40; the
// ptsname documented-client-ABI precedent). This retires the statless-cons
// latent (fstat -1 -> pouch folded to ENOTTY -> isatty()==false on the
// console -> musl stdio ran fully-buffered).
int cons_stat_native_fill(struct Spoor *c, struct t_stat *out) {
    if (!c || !out) return -1;
    for (size_t i = 0; i < sizeof(*out); i++) ((u8 *)out)[i] = 0;
    out->mode     = T_S_IFCHR | 0620u;
    out->nlink    = 1;
    out->qid_path = CONS_STAT_QID_FLAG | c->qid.path;
    out->qid_type = QTFILE;
    out->blksize  = 256;
    out->uid      = PRINCIPAL_SYSTEM;
    out->gid      = GID_SYSTEM;
    // devno: stamped by spoor_stat_native (#100 -- the Spoor's identity, not
    // the Dev's), the devramfs idiom; the zero-fill above leaves it 0 here.
    return 0;
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

// #55: SYS_FSTAT on a SYS_CONSOLE_OPEN fd (the std-fd inheritance chain) --
// the shared is-a-cons contract (see cons_stat_native_fill).
static int devcons_stat_native(struct Spoor *c, struct t_stat *out) {
    return cons_stat_native_fill(c, out);
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
    .stat_native = devcons_stat_native,      // #55: the is-a-cons qid contract
                                             // (also CL-4's clang fstat fix)

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
