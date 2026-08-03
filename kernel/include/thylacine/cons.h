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

// =============================================================================
// LS-8b: the line discipline (termios) -- five independent flags.
// =============================================================================
//
// The single physical console carries a global termios word (per-fd termios is
// Phase-8 /dev/pts, I-20). Five fine-grained flags (granularity B, user-voted
// 2026-06-12) gate the cooking in cons_rx_input + cons_output_write:
//
//   CONS_ICANON  canonical (line) mode: assemble a line, deliver on Enter,
//                handle erase (backspace). Off -> raw byte-at-a-time.
//   CONS_ECHO    echo each input byte to console output (HARD off-guarantee:
//                ECHO clear -> NO input byte reaches the output -- the password
//                mask; the cooked erase/redraw never leaks a masked byte).
//   CONS_ISIG    Ctrl-C (0x03) -> the deferred `interrupt` note (the LS-5 path).
//                Off -> 0x03 is an ordinary data byte.
//   CONS_ICRNL   translate an input CR (0x0d) -> NL (0x0a).
//   CONS_ONLCR   translate an output (and echoed) NL -> CR NL.
//
// The wire grammar (the /dev/consctl control file -- NOT ioctl, the Plan 9
// idiom) names these "icanon"/"echo"/"isig"/"icrnl"/"onlcr" (see
// cons_set_mode_cmd / cons_render_mode). The boot DEFAULT is CONS_ISIG only --
// byte-at-a-time, Ctrl-C cooked, no echo, no translation == EXACTLY the
// pre-LS-8b behavior, so the mechanism is inert until a consumer opts into
// cooked mode (login for cooked-echo prompts; ut for its raw line editor).
#define CONS_ICANON  0x01u
#define CONS_ECHO    0x02u
#define CONS_ISIG    0x04u
#define CONS_ICRNL   0x08u
#define CONS_ONLCR   0x10u

#define CONS_TERMIOS_ALL      (CONS_ICANON | CONS_ECHO | CONS_ISIG | CONS_ICRNL | CONS_ONLCR)
#define CONS_TERMIOS_DEFAULT  CONS_ISIG   // boot default == the pre-LS-8b behavior

// LS-8b: the canonical line-assembly bound (cooked mode). A line longer than
// this drops the overflow (like the ring); Enter still delivers what fits, and
// the dropped byte is not echoed. Exported because #95's drop-counter test
// asserts against it -- a test hardcoding 256 would match the code by
// coincidence, not by construction.
#define CONS_LINE_MAX     256u

// Feed one received byte to the console input layer. Called from the PL011 RX
// IRQ handler (arch/arm64/uart.c::uart_rx_handler), IRQ context. `is_break` is
// true when the PL011 flagged a line BREAK on this entry (DR bit-10 BE) -- the
// A-4c-2 SAK: it sets sak-pending + wakes console_mgr (which runs the privileged
// revoke/re-grant in process context); the accompanying DR byte is NOT enqueued.
// A data byte equal to 0x03 (Ctrl-C) is cooked-consumed (it generates a deferred
// `interrupt` note, NOT ring data). Any other byte is enqueued to the ring.
// Wakes a blocked devcons_read and/or the console_mgr kthread as appropriate.
//
// #129: RETURNS whether the byte was ACCEPTED. False means the ring could not
// take what this byte would push -- and, critically, that NOTHING CHANGED: no
// ring push, no line_len mutation, no echo, no flags. The caller still owns the
// byte and must re-offer this exact byte later; that is what turns a full ring
// from a silent drop into back-pressure. (The PL011 drain holds it in the FIFO
// or its 1-byte holdback; cons_feed_write returns a short count.)
//
// Refusal is confined to the two arms that push ring bytes -- the raw/cbreak
// byte and the canonical terminator's line_len + 1 flush. Everything that
// pushes nothing is ALWAYS accepted, which is load-bearing for two of them:
// a BREAK (the A-4c-2 SAK) and a cooked Ctrl-C must never be blocked by a full
// ring, or a wedged reader could suppress the I-27 trusted-path trigger and the
// interrupt that would unwedge it. A byte past CONS_LINE_MAX is still a DROP
// (returns true): the line buffer is a fixed bound, so back-pressuring there
// would wedge on a user who simply never presses Enter.
bool cons_rx_input(u8 byte, bool is_break);

// #57b: the shared console-input/output API -- the ONE implementation behind
// both console front doors. `devcons` (the SYS_CONSOLE_OPEN syscall path) and
// `devdev`'s /dev/cons leaf (the namespace path) both call these, so the
// single-reader busy-guard bounds the console to one reader across both doors.
// cons_input_read: blocking RX-ring drain (death-interruptible; -1 on
// bad-args/reader-busy; >= 1 on data). cons_output_write: forward each byte to
// the UART (== n at v1.0).
long cons_input_read(void *buf, long n);
long cons_output_write(const void *buf, long n);

// #75 / P1-F (ARCH §23.5.2). The console TX ring.
//
// cons_output_write is atomic against other console writers: it holds a writer
// role for the whole call (contending writers PARK, they do not interleave), and
// pushes into a ring the PL011 TX interrupt drains. It returns a SHORT count if
// the #67 deadline fires against a stalled host consumer or a #811 death
// interrupts it -- it never hangs. Echo (IRQ context) shares the ring but is
// non-blocking and drops on overrun; the opposite blocking contracts of the two
// producers are load-bearing.

// Drain ring -> FIFO. Called from uart_irq_handler on MIS.TXMIS (IRQ context).
void cons_tx_drain_from_irq(void);

// Arm the IRQ-driven path. Until this runs, every byte takes the direct bounded
// uart_putc, so early-boot prints and the tooling-ABI banner are unaffected.
// boot_main calls it after gic_attach + gic_enable_irq for the UART SPI.
void cons_tx_arm(void);

// Bounded synchronous ring flush for the extinction / Halls dump, so pre-crash
// output is not stranded in the ring. Takes the ring lock by TRYLOCK only (a
// dying CPU may already hold it) and never waits on the IRQ -- HX-I discipline.
void cons_tx_flush_for_dump(void);

// #75-audit F3: bounded synchronous flush for a HEALTHY caller (blocking-lock,
// waits the FIFO out). boot_mark_complete calls it before the direct-path
// "Thylacine boot OK" banner so residual EL0 ring output cannot tear that
// tooling-ABI line (TOOLING.md section 10).
void cons_tx_flush(void);

// #126: the NON-BLOCKING kernel diagnostic emit. Use these -- NOT uart_puts /
// uart_putdec / uart_puthex64 -- for any steady-state kernel diagnostic issued
// from a context that can neither sleep nor spin: IRQ context, or under a
// spinlock (every g_proc_table_lock acquisition in proc.c is irqsave).
//
// The direct arch emitters spin on a full TX FIFO for up to 20 ms PER BYTE
// before dropping it (#67). That bound does NOT compose: ~90 bytes of
// diagnostic emitted back-to-back under a global irqsave lock is ~1.8 s
// interrupt-dead with that lock held -- the #126 defect, and exactly the stall
// #67's per-byte bound exists to prevent. These push into the TX ring instead
// (never spinning; dropping on a full ring, the echo disposition) and feed the
// G-4 drain tap, so the line also reaches the framebuffer console (#76).
//
// Never sleep; never spin; take only leaf locks and wake outside them. Legal
// wherever cons_emit is -- which includes the UART RX IRQ handler. Pre-arm they
// fall through to the direct bounded path, so boot output is byte-identical.
void cons_diag_puts(const char *s);
void cons_diag_putdec(u64 v);
void cons_diag_puthex64(u64 v);

// #75 test hooks (test-only). The RING needs no dedicated test -- every byte of
// console output on every boot goes through it, so a ring bug means no boot at
// all. The ROLE does: it is what makes a write call-atomic, and its absence is
// silent until two CPUs happen to interleave. These let a test hold the role and
// prove a second cons_output_write PARKS instead of emitting.
void cons_test_tx_role_hold(void);
void cons_test_tx_role_drop(void);
bool cons_test_tx_role_held(void);

// #75-audit F2 test hooks (test-only). The role test above runs under echo
// capture, which short-circuits cons_emit_wait BEFORE the ring -- so the
// ROOM-WAIT park and the #67 DEADLINE drop had no deterministic coverage at all.
// Paired with uart_test_tx_stall (arch/arm64/uart.h) these drive both legs:
// stall the UART, fill the ring for real, then either let the deadline fire or
// free a slot and prove the parked writer wakes.
//
// cons_test_tx_ring_free DISCARDS n bytes from the head (never writing them to
// the UART), keeping the test's ring-sized filler off the wire and out of the
// boot log the gates parse. `wake == false` frees room WITHOUT waking, which is
// how the test parks a writer over a ring that has room -- the setup that lets
// the REAL cons_tx_drain_from_irq deliver the wake under test. Returns the
// number of bytes actually freed.
u32  cons_test_tx_ring_free(u32 n, bool wake);
u32  cons_test_tx_ring_count(void);
u32  cons_test_tx_dropped(void);
u32  cons_test_tx_room_waits(void);
bool cons_test_tx_armed(void);
u32  cons_test_tx_ring_capacity(void);
u64  cons_test_tx_room_wait_ns(void);

// #174/#129 backpressure PRE-check: true iff the RX ring has room for the
// worst-case push one more admitted byte could cause. The PL011 RX drain
// (uart_rx_handler / uart_rx_pump) checks this BEFORE reading a byte out of the
// FIFO -- on false it leaves the byte in the FIFO and pauses RX.
//
// #129 widened the reservation from ONE byte to CONS_LINE_MAX + 1. In canonical
// mode a single admitted byte can be the terminator that flushes an entire
// assembled line, so "room for this byte" was never the question the drain
// needed answered -- the same per-ITEM-vs-per-OPERATION category error #126 hit
// on the TX side. Lockless and deliberately unconditional (it consults neither
// termios nor line_len; both can change under it). It is an OPTIMIZATION, not
// the guarantee: cons_rx_input's under-lock room check is what makes an overrun
// impossible, so a stale read here costs at most one refuse-and-retry.
bool cons_rx_can_accept(void);

// #95/#129: the INPUT-path counters. NULL args are skipped. Surfaced at
// /ctl/cons; console_mgr emits one loud line on the first real DROP of a boot.
//
// Read the names carefully -- they are not interchangeable:
//   bp_raw / bp_flush -- BACK-PRESSURE, not loss. The ring was full and the byte
//                        was refused; the producer holds it and retries. Normal
//                        operation on a busy console; non-zero is a load signal.
//   drop_line         -- a real drop: a byte past CONS_LINE_MAX in cooked line
//                        assembly. Bounded and deliberate (see cons_rx_input).
//   drop_ring         -- MUST BE ZERO. A push failed after the under-lock room
//                        check authorized it, i.e. #129's room arithmetic
//                        disagrees with the ring. Non-zero is a kernel bug.
void cons_rx_counters(u32 *bp_raw, u32 *bp_flush, u32 *drop_line, u32 *drop_ring);

// #95: the TX-side loss counts (#75/#126), for the same /ctl/cons surface.
void cons_tx_drops(u32 *dropped, u32 *room_waits);

// LS-8a: the shared console poll. Register-then-observe under the cons lock:
// POLLIN iff the RX ring is non-empty; POLLOUT always (the UART never blocks);
// if `pw` is non-NULL, install it on the console poll-hook list. The IRQ
// producer cannot walk that list (poll_waiter_list_wake is not IRQ-safe), so a
// POLLIN edge sets a flag + wakes console_mgr, which walks it in process context
// (the cons_poll.tla I-9 deferred-wake relay). Shared by devcons
// (SYS_CONSOLE_OPEN) + devdev's /dev/cons leaf -- #57b single-impl.
short cons_poll(short events, struct poll_waiter *pw);

// LS-8b: the /dev/consctl control surface (the Plan 9 idiom, not ioctl). Both
// take a KERNEL buffer (the syscall layer already copied user->kernel).
//
// cons_set_mode_cmd: parse + apply one consctl write. The grammar is
// whitespace-separated "+name"/"-name" tokens (name in {icanon,echo,isig,icrnl,
// onlcr}); "+" sets the flag, "-" clears it. ALL tokens are parsed before any is
// applied -- a single malformed token rejects the whole write (atomic
// multi-flag set, the tcsetattr seam) and leaves the mode unchanged. Returns n
// on success, -1 on a malformed command (bad sign, unknown name, empty).
//
// cons_render_mode: render the current mode for read-back (the tcgetattr seam)
// as five space-separated "+name"/"-name" tokens + '\n' (same grammar as the
// write -- symmetric). Writes the WHOLE line into buf (needs >= 34 bytes);
// returns the byte count, or 0 if buf is too small (never a partial line).
// `allow_flags` (#55 audit F2): true for the trusted console-attach chain
// (the SYS_CONSOLE_OPEN fd + login/ut's inherited consctl) -> the full five-flag
// grammar; false for a RENDERER-minted consctl (CCONSWINSZONLY) -> the winsize
// verb ONLY (a `+`/`-` flag token rejects the write). Keeps a compromised
// renderer off the global termios (the serial-input ECHO-off mask defeat).
long cons_set_mode_cmd(const void *buf, long n, bool allow_flags);
long cons_render_mode(void *buf, long n);

// =============================================================================
// #55: the console winsize (ARCH 23.5.3). One kernel-held size (cols, rows --
// the Linux unsigned-short band; 0x0 = never set, the serial posture) beside
// the five mode flags, under g_cons.lock. The WRITER is the renderer (aurora
// reports its cell grid via the consctl `winsize <cols> <rows>` verb -- the
// ptyfs PTY-2c grammar, byte-identical); a CHANGED size posts tty:winch to the
// console owner's PGRP (proc_console_post_winch -- iff-changed, the Linux
// TIOCSWINSZ / ptyfs semantics). Readback: cons_render_mode appends
// "winsize <cols> <rows>" to the mode line (parser parity with the ptyfs ctl),
// and cons_render_winsize renders the standalone `winsize <cols> <rows>\n`
// line the ungated /dev/winsize leaf serves.
// =============================================================================

// Snapshot the current winsize (0,0 = never set). Lock-free-safe callers take
// a coherent pair via one g_cons.lock hold internally.
void cons_winsize_get(u16 *cols, u16 *rows);

// Render `winsize <cols> <rows>\n` (max 21 bytes incl. the NL). Returns the
// byte count, or 0 if buf is too small (never a partial line).
long cons_render_winsize(void *buf, long n);

// Diagnostic: count of CHANGED winsize applies (each corresponds to one
// tty:winch post attempt). The iff-changed regression reads it -- an unchanged
// rewrite must NOT advance it.
u32 cons_winch_events(void);

// #55: the is-a-cons qid contract (the ptsname PTS_FLAG precedent, pouch
// 0021's client ABI). cons fds were STATLESS (fstat -1 -> pouch folded them
// to ENOTTY -> isatty()==false on the console); cons_stat_native_fill gives
// devcons + devdev's cons leaf one shared t_stat fill: zero-fill (I-13),
// T_S_IFCHR posture, SYSTEM-owned, qid_path carrying CONS_STAT_QID_FLAG --
// bit 41, DISJOINT from ptyfs's PTS_FLAG (bit 40) under the shared S_IFCHR
// posture (/net's bit-40 qids report S_IFREG and fail the S_ISCHR pre-gate).
#define CONS_STAT_QID_FLAG (1ULL << 41)
struct Spoor;
struct t_stat;
int cons_stat_native_fill(struct Spoor *c, struct t_stat *out);

// =============================================================================
// G-4: the console-renderer drain/feed backend (TAPESTRY.md section 18.7 /
// AURORA.md section 4). The bound renderer (Aurora, SPAWN_PERM_CONSOLE_RENDERER)
// holds the /dev/consdrain + /dev/consfeed pair, served by devdev:
//
//   drain (read-only): every byte cons_emit carries -- program output through
//   cons_output_write AND line-discipline echo -- is MIRRORED into a bounded
//   drop-oldest ring the renderer reads (blocking; pollable via the LS-8a
//   deferred-wake relay). On serial-bearing media the UART path continues
//   byte-identical (the tee -- the tooling ABI + the serial trusted path keep
//   working); the exclusive board-era switch bound from the DTB medium fact
//   (TRUSTED-PATH section 7) is the recorded seam. Kernel diagnostics
//   (SYS_PUTS, extinction, Halls) go uart-direct and are NEVER in the drain --
//   the crash path must not depend on a userspace renderer.
//
//   feed (write-only): the renderer's decoded keyboard bytes enter
//   cons_rx_input EXACTLY as UART RX bytes do -- the LS-8 cooking (ICANON /
//   ECHO / ISIG / ICRNL) is backend-independent and unchanged; a graphical
//   Ctrl-C rides ISIG to the console OWNER. `is_break` is HARDWIRED false:
//   no feed byte sequence can synthesize the SAK line condition (I-27).
// =============================================================================

// Arm the drain (the /dev/consdrain open; single-open, -1 if already open;
// a fresh open discards the prior epoch's bytes) / disarm it (the last close;
// wakes a parked reader to EOF + the registered pollers).
int  cons_drain_open(void);
void cons_drain_close(void);

// Blocking drain read: >= 1 byte on data; 0 (EOF) when disarmed with nothing
// buffered; -1 on bad args / not-open / a second concurrent reader.
long cons_drain_read(void *buf, long n);

// Drain poll: POLLIN iff bytes are buffered or the drain is disarmed (EOF is
// readable); read-only leaf, so never POLLOUT. Registers `pw` on the drain
// hook list (walked by console_mgr for IRQ-context edges).
short cons_drain_poll(short events, struct poll_waiter *pw);

// Feed: inject `n` bytes into the console line discipline (never a BREAK).
// Returns the number ACCEPTED (a SHORT count, possibly 0, when the RX ring
// fills -- #129 back-pressure, not an error); -1 on bad args.
//
// The short return is the contract, not an edge case: this is the graphical
// console's entire keyboard, and before #129 it returned n unconditionally
// while cons_rx_input silently dropped the overflow. A caller that ignores the
// count is discarding keystrokes exactly as the old code did.
long cons_feed_write(const void *buf, long n);

// The console_mgr kproc kthread entry. Spawned once at boot (boot_main). Sleeps
// on the console-manager Rendez; on wake, performs the deferred privileged work
// in process context: post the `interrupt` note to the current console owner
// (Ctrl-C), the A-4c-2 SAK revoke/re-grant (proc_console_sak), and the LS-8a /
// G-4 deferred poll-hook walks (the cons ring's + the drain's). Never returns.
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

// #58: hold console_mgr parked through a deferred-wake test dance (a held mgr's
// wake cond reads false, so it re-parks WITHOUT consuming pending flags; the
// release wakes it explicitly -- no lost wake). Makes the register/produce/
// assert dance deterministic on SMP: a peer-CPU-dispatched mgr otherwise
// consumed the pending flag mid-dance, and the failing assert's early return
// leaked the test's stack poll_waiter (-> EXTINCTION: pw_wake at the next
// walk). Direct-drive via cons_test_service_deferred bypasses the cond and is
// never blocked by the hold. Production never sets it.
void cons_test_mgr_hold(bool on);

// Force the single-reader busy flag (to exercise the devcons_read busy-guard
// without a second live reader thread).
void cons_test_set_reader_busy(bool busy);

// LS-8b: read / force the global termios word (drive the cooking deterministically
// without a live consctl writer). cons_test_set_termios mirrors what a consctl
// write does (under g_cons.lock).
u32  cons_test_termios(void);
void cons_test_set_termios(u32 v);

// LS-8b: the echo/output capture sink. Console echo + cons_output_write emit
// through cons_emit, which (only when capture is ON -- a test hook) buffers the
// bytes instead of writing the UART, so a test can assert EXACTLY what was
// echoed (and the ECHO-off no-output property). Always OFF in production (the
// emit path is then a single never-taken branch + uart_putc).
// cons_test_echo_capture(on) enables + resets the buffer; cons_test_echo_captured
// copies up to `max` captured bytes into out and returns the TRUE captured count
// (so a test detects truncation/overflow).
void cons_test_echo_capture(bool on);
u32  cons_test_echo_captured(u8 *out, u32 max);

// G-4: drain observability for tests. Buffered byte count / cumulative
// drop-oldest count / the deferred drain POLLIN edge flag.
u32  cons_test_rx_count(void);      // #129-audit F2: NON-BLOCKING RX depth
u32  cons_test_drain_count(void);
u32  cons_test_drain_overflow(void);
bool cons_test_drain_pollwake_pending(void);

// #130-R2 F2: the harness backstop. Each bit names a piece of console state a
// test arms for a window and must release; a failing assert inside the window
// returns from the test and skips the release, which costs the console (or the
// boot) for every test after it. test_run_all calls this after EVERY test,
// releases whatever was left armed, and fails the test that leaked it -- see
// the full rationale at the definition in cons.c.
#define CONS_TEST_OWNED_ECHO_CAPTURE  (1u << 0)
#define CONS_TEST_OWNED_TX_ROLE       (1u << 1)
#define CONS_TEST_OWNED_MGR_HOLD      (1u << 2)
#define CONS_TEST_OWNED_READER_BUSY   (1u << 3)
u32  cons_test_release_owned_state(void);

#endif // THYLACINE_CONS_H
