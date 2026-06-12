// /dev/cons console input layer (A-4c-1).
//
// v1.0 P4-B landed the write-side (devcons_write -> uart_putc). A-4c-1 adds
// the kernel UART RX path: the PL011 RX IRQ handler (arch/arm64/uart.c) hands
// each received byte to cons_rx_input, which fills a bounded console input ring
// (data bytes) or recognizes a control event (Ctrl-C -> a deferred `interrupt`
// note; a serial BREAK is the A-4c-2 SAK and is discarded at A-4c-1). A blocking
// devcons_read drains the ring.
//
// IRQ-context discipline (IDENTITY-DESIGN.md section 9.8 "As-built"): the RX
// handler runs in IRQ context, where notes_post + poll_waiter_list_wake are NOT
// IRQ-safe (plain spin_lock). So cons_rx_input is wakeup-only -- it fills the
// ring + sets deferred-action flags + wakes two Rendez (wakeup() IS IRQ-safe).
// The console_mgr kproc kthread services the deferred actions (the `interrupt`
// note post; the A-4c-2 SAK revoke/re-grant) in process context.

#ifndef THYLACINE_CONS_H
#define THYLACINE_CONS_H

#include <thylacine/types.h>

struct poll_waiter;   // <thylacine/poll.h> -- the cons_poll hook parameter

// Feed one received byte to the console input layer. Called from the PL011 RX
// IRQ handler (arch/arm64/uart.c::uart_rx_handler), IRQ context. `is_break` is
// true when the PL011 flagged a line BREAK on this entry (DR bit-10 BE) -- the
// A-4c-2 SAK: it sets sak-pending + wakes console_mgr (which runs the privileged
// revoke/re-grant in process context); the accompanying DR byte is NOT enqueued.
// A data byte equal to 0x03 (Ctrl-C) is cooked-consumed (it generates a deferred
// `interrupt` note, NOT ring data). Any other byte is enqueued to the ring
// (dropped if the ring is full -- bounded, never overflows). Wakes a blocked
// devcons_read and/or the console_mgr kthread as appropriate.
void cons_rx_input(u8 byte, bool is_break);

// #57b: the shared console-input/output API -- the ONE implementation behind
// both console front doors. `devcons` (the SYS_CONSOLE_OPEN syscall path) and
// `devdev`'s /dev/cons leaf (the namespace path) both call these, so the
// single-reader busy-guard bounds the console to one reader across both doors.
// cons_input_read: blocking RX-ring drain (death-interruptible; -1 on
// bad-args/reader-busy; >= 1 on data). cons_output_write: forward each byte to
// the UART (== n at v1.0).
long cons_input_read(void *buf, long n);
long cons_output_write(const void *buf, long n);

// LS-8a: the shared console poll. Register-then-observe under the cons lock:
// POLLIN iff the RX ring is non-empty; POLLOUT always (the UART never blocks);
// if `pw` is non-NULL, install it on the console poll-hook list. The IRQ
// producer cannot walk that list (poll_waiter_list_wake is not IRQ-safe), so a
// POLLIN edge sets a flag + wakes console_mgr, which walks it in process context
// (the cons_poll.tla I-9 deferred-wake relay). Shared by devcons
// (SYS_CONSOLE_OPEN) + devdev's /dev/cons leaf -- #57b single-impl.
short cons_poll(short events, struct poll_waiter *pw);

// The console_mgr kproc kthread entry. Spawned once at boot (boot_main). Sleeps
// on the console-manager Rendez; on wake, performs the deferred privileged work
// in process context: post the `interrupt` note to the current console owner
// (Ctrl-C), and the A-4c-2 SAK revoke/re-grant (proc_console_sak). Never returns.
void console_mgr_main(void);

// =============================================================================
// Test harness hooks (exposed like notes_queue_init / devproc_kill_authorized).
// Harmless in production; used by kernel/test/test_cons.c to drive the ring +
// the recognizer deterministically without a real UART IRQ (the integration
// harness cannot inject UART RX -- IDENTITY-DESIGN.md section 9.8 test note).
// =============================================================================

// Reset the console input ring + flags + reader-busy to the empty state.
void cons_test_reset(void);

// True iff a Ctrl-C `interrupt` post is pending (set by cons_rx_input, cleared
// by the console_mgr kthread).
bool cons_test_intr_pending(void);

// True iff an A-4c-2 SAK (serial BREAK) is pending (set by cons_rx_input on a
// BREAK, cleared by the console_mgr kthread before proc_console_sak runs).
bool cons_test_sak_pending(void);

// True iff a POLLIN deferred-wake (cons_rx_input set poll_wake_pending) awaits
// console_mgr's hook-list walk (LS-8a). Cleared by cons_service_deferred.
bool cons_test_pollwake_pending(void);

// Run ONE console_mgr service iteration (drain the deferred flags + act) -- the
// production cons_service_deferred path, driven deterministically. Lets a test
// exercise the LS-8a deferred poll-wake relay (register a poll_waiter via
// cons_poll -> cons_rx_input a byte -> cons_test_service_deferred wakes the hook)
// without a live console_mgr kthread or a real UART IRQ.
void cons_test_service_deferred(void);

// Force the single-reader busy flag (to exercise the devcons_read busy-guard
// without a second live reader thread).
void cons_test_set_reader_busy(bool busy);

#endif // THYLACINE_CONS_H
