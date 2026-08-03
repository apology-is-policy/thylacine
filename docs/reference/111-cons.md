# 111 — The console device (`/dev/cons`, `kernel/cons.c`)

**Status:** as-built through **LS-8b** (pollable cons + the deferred poll-wake +
the five-flag termios line discipline + `/dev/consctl`). The shell-side
multi-fd poll loop + the cooked-default flip is **LS-8c**; the per-fd-termios PTY
master/slave pair (`/dev/ptmx` + `/dev/pts`) is **Phase 8** (I-20). The login
echo consumer (LS-6 fold-in) is pending a console-mode access decision (see
"Known caveats / footguns").

## Purpose

`kernel/cons.c` is the kernel console — the one physical terminal, backed by the
PL011 UART. It is the single-reader input + write-through output device that the
getty / login / shell read and write as fd 0/1/2. It sits below two front doors
that share its one implementation (#57b):

- **`devcons`** (`dc='c'`) — the `SYS_CONSOLE_OPEN` syscall path. The getty mints
  a `KOBJ_SPOOR` `R|W` handle on it and hands it to `/sbin/login` as fd 0/1/2.
- **`devdev`'s `/dev/cons` leaf** (`kernel/devdev.c`) — the namespace path
  (`open("/dev/cons")`). Both call the same `cons_*` API, so the single-reader
  guard bounds the console to one reader **across both doors**.

The console is the **I-27 trusted path**: the SAK (serial BREAK) + console-attach
gate live here (A-4c), which is why the line discipline is kernel-side, not a
userspace `consd`.

## Public API (the shared `cons_*` surface, `<thylacine/cons.h>`)

```c
// Feed one received byte (PL011 RX IRQ context). is_break == a line BREAK (SAK).
void cons_rx_input(u8 byte, bool is_break);

// Blocking RX-ring drain. >=1 on data; 0 only on a death-interrupt with nothing
// buffered; -1 on bad args / a second concurrent reader (single-reader guard).
long cons_input_read(void *buf, long n);

// Forward each byte to the UART (== n at v1.0; the UART never fails short).
long cons_output_write(const void *buf, long n);

// LS-8a: poll. POLLIN iff the RX ring is non-empty; POLLOUT always (the UART
// never blocks); if pw != NULL, install it on the console hook list under the
// cons lock (register-then-observe). Shared by devcons + the /dev/cons leaf.
short cons_poll(short events, struct poll_waiter *pw);

// LS-8b: the /dev/consctl control surface (the Plan 9 idiom, not ioctl). Both
// take a KERNEL buffer. cons_set_mode_cmd parses + applies one consctl write
// ("+name"/"-name" tokens, atomic; -1 on malformed); cons_render_mode renders
// the current mode for read-back (five "+name"/"-name" tokens + '\n', 34 bytes).
long cons_set_mode_cmd(const void *buf, long n);
long cons_render_mode(void *buf, long n);

// The console_mgr kproc kthread entry (spawned once at boot).
void console_mgr_main(void);
```

## Implementation

### Data structure — `struct cons_input g_cons` (file-scope static, immortal)

```c
struct cons_input {
    spin_lock_t lock;                  // ring + head/tail/count + flags; taken irqsave
    u8          ring[CONS_RING_SIZE];  // 512, power-of-two, mask-indexed (#129)
    u32         head, tail, count;     // count: RELAXED-atomic (read in conds)
    bool        reader_busy;           // single-reader guard
    bool        intr_pending;          // Ctrl-C -> deferred `interrupt` note
    bool        sak_pending;           // serial BREAK -> deferred SAK (A-4c-2)
    bool        poll_wake_pending;     // LS-8a: a POLLIN edge -> deferred poll walk
    struct poll_waiter_list poll_list; // LS-8a: the console poll-hook list
};
```

Two Rendez accompany it: `g_cons_data_rendez` (a blocking reader parks here) and
`g_cons_mgr_rendez` (the `console_mgr` kthread parks here).

### The IRQ producer + the `console_mgr` deferral

`cons_rx_input` runs in **IRQ context** (`arch/arm64/uart.c::uart_rx_handler`).
It does ONLY ring + flag mutation under `g_cons.lock` (irqsave) + `wakeup()` —
the sole IRQ-safe wake. Everything that takes a plain (non-irqsave) lock —
`notes_post` (the Ctrl-C `interrupt`), `proc_console_sak` (the BREAK revoke), and
**`poll_waiter_list_wake` (LS-8a)** — is deferred to the `console_mgr` kproc
kthread, which runs in **process context**.

Per byte, `cons_rx_input`:
- **data byte** → enqueue to the ring + `wakeup(&g_cons_data_rendez)` (the
  blocking reader). On the **empty→non-empty edge** (`count == 0`), also set
  `poll_wake_pending` + `wakeup(&g_cons_mgr_rendez)` (LS-8a; see below).
- **Ctrl-C (0x03)** → set `intr_pending` + wake the mgr (cooked-consumed, never
  ring data).
- **BREAK** → set `sak_pending` + wake the mgr (the A-4c-2 SAK; never ring data).

`console_mgr_main` loops `sleep(&g_cons_mgr_rendez, cons_mgr_pending)` then calls
`cons_service_deferred()`, which drains all three flags under `g_cons.lock`,
releases it, and acts lock-free: `proc_console_sak()` (SAK supersedes a coalesced
Ctrl-C, RW-7 R2-F2), else `proc_console_post_interrupt()`, and — independently —
`poll_waiter_list_wake(&g_cons.poll_list)` if `poll_wake_pending` was set.

### LS-8a — the deferred poll-wake (I-9, `specs/cons_poll.tla`)

A **poller** does not block-read; it registers a `poll_waiter` hook on
`g_cons.poll_list` (via `cons_poll` with `pw != NULL`) and parks on its own
private Rendez (`sys_poll_for_proc`, `kernel/poll.c`). Waking it means walking the
hook list — `poll_waiter_list_wake` — which takes a plain spinlock and nests a
`wakeup`, so it **cannot run from IRQ context**. The RX IRQ therefore sets
`poll_wake_pending` and wakes the `console_mgr`, which walks the list in process
context. This is exactly Linux's tty model (the hard IRQ buffers the byte and
schedules `flush_to_ldisc` work; the read/poll wakeups run in that work item).

**No wakeup is lost across the relay (I-9 generalized; `specs/cons_poll.tla`):**

1. `poll_wake_pending` is set under the **same `g_cons.lock`** as the ring-count
   mutation. The mgr drains it under `g_cons.lock` too, so the mgr's walk is
   causally **after** the count mutation — any poller that registered (sampled +
   installed its hook under `g_cons.lock`) before the mutation is found by the
   walk. (Register-then-observe at the poller; `cons_poll` holds `g_cons.lock`
   across the sample + the `poll_waiter_list_register`.)
2. The mgr's own sleep is register-then-observe: `sleep(&g_cons_mgr_rendez,
   cons_mgr_pending)` enqueues on the Rendez, then re-checks `poll_wake_pending`
   under the Rendez lock — so a flag set as the mgr heads to sleep either keeps
   it off the sleep path (the `cons_mgr_pending` guard) or finds it enqueued to
   wake. The `BUGGY_MGR_LOST_WAKE` cfg (a hand-rolled check-then-sleep) is the
   executable counterexample.

Only the **empty→non-empty edge** arms the wake: a poller that sampled
`count > 0` returned POLLIN without sleeping, so the only sleeping pollers sampled
`count == 0`, and the next byte (`count == 0` before it) is the edge that makes
them ready.

**Lock order:** `g_cons.lock` (object) → `poll_list.lock` (list) → (in
`wakeup`) `g_timerwait.lock` → rendez → cpu_sched — the `poll.h` order. `cons_poll`
nests list under object (register); the mgr takes them sequentially (drain under
object, release, then wake under list). No path takes list then object.

**Lifetime:** `g_cons.poll_list` lives in the file-scope static `g_cons` →
**immortal**, so the RW-2 2C-F1 registered-object-lifetime hazard (a sibling
thread freeing an embedded list mid-sleep) **structurally cannot arise** here
(unlike pipe/srvconn, whose lists live in a refcounted object). Multi-poller
composition is the standard `poll.tla` case (each poller has its own private
Rendez + stack waiter).

## Line discipline (termios) — LS-8b (as-built)

The single console carries a global termios word (`g_cons.termios`, per-fd
termios is Phase-8 `/dev/pts`, I-20). Five **independent** flags (granularity B,
user-voted 2026-06-12) gate the cooking:

| Flag | Effect | Where |
|---|---|---|
| `CONS_ICANON` | line mode: assemble a line, deliver on Enter, handle erase (BS/DEL) | `cons_rx_input` |
| `CONS_ECHO` | echo each input byte to output (HARD off-guarantee: the password mask) | `cons_rx_input` |
| `CONS_ISIG` | Ctrl-C (0x03) → the `interrupt` note (LS-5); off → a `0x03` data byte | `cons_rx_input` |
| `CONS_ICRNL` | input CR (0x0d) → NL (0x0a) | `cons_rx_input` |
| `CONS_ONLCR` | output (and echoed) NL → CR NL | `cons_output_write` + echo |

**The cooking runs in `cons_rx_input` (IRQ context).** `uart_putc` is lock-free
+ IRQ-safe (it polls TXFF, writes DR — no lock, no sleep), so echo-from-IRQ is
sound; no second raw ring / deferred-cook is needed (the ISIG Ctrl-C→note path is
already deferred via `intr_pending` + `console_mgr`). This is the simpler sound
choice for a low-volume (human-typing) console; Linux's `flush_to_ldisc` deferral
is a throughput optimization a single console doesn't need. Echo bytes are staged
under `g_cons.lock` (a ≤3-byte stack buffer — the erase `"\b \b"` is the max) and
emitted via `cons_emit` AFTER the lock is released, so the UART busy-wait never
runs under `g_cons.lock`.

**`uart_putc`'s TXFF wait is BOUNDED (#67).** The TX-full spin was originally
unbounded (`while (FR & TXFF) {}` — "fine at P1-B, single CPU, no scheduler"; the
IRQ-driven TX buffer of P1-F was never built, so this IS the live TX path). A
stalled host serial consumer (a full host pty/pipe buffer) leaves the TX FIFO
full indefinitely, and an unbounded spin then goes *interrupt-dead* — the CPU
cannot take its timer tick or an IPI while it waits. That is a soundness hazard on
the print path: it composes with the crash-dump (which runs IRQ-masked on a dying
machine, see `101-halls.md`), and #66 proved that a print spinning here inside an
IRQ dispatch manufactured a *seconds-long* per-INTID stall (an interrupt-dead cpu0
misdiagnosed as a scheduler bug for days). `uart_putc` now bounds the wait with a
wall-clock deadline (`UART_TX_SPIN_MAX_NS` = 20 ms, via `timer_now_ns()`) plus an
unconditional iteration backstop (`UART_TX_SPIN_MAX_ITERS`), dropping the byte on
timeout — a bounded, lossy console is strictly sounder than a wedged CPU. The
20 ms deadline tolerates even a slow 9600-baud line: a per-call spin ends when
TXFF *clears*, and each FIFO slot frees at the baud rate — one byte-time, ~1.1 ms
at 9600 8N1 — far under (~17×) the 20 ms deadline, so no legitimate output is
dropped (the driver configures no RTS/CTS or XON/XOFF flow control, so nothing
else can stall TX). The deadline fires only for a genuinely *wedged* consumer
that is not draining the FIFO at all. `timer_now_ns()` returns 0 before
`timer_init` (the deadline is inert during the earliest boot prints; the iteration
backstop covers that window and a frozen counter) — the RNG-audit F1 / #101 F2
idiom. Healthy case: TXFF is clear on entry, the loop never spins, and no timer is
read. Regression: `uart.putc_tx_bounded` (points the driver at a scratch region
with FR stuck-full and proves `uart_putc` returns + drops; reverting to
`while(TXFF){}` hangs the boot inside the test).

**The boot default is `CONS_ISIG` only** — byte-at-a-time, Ctrl-C cooked, no
echo, no translation == EXACTLY the pre-LS-8b behavior. So LS-8b **breaks
nothing**: `ut` and foreground commands are unchanged; the mechanism is inert
until a consumer opts into cooked mode (login for cooked-echo prompts; `ut` for
its raw line editor — LS-8c). A *cooked* default is coupled to `ut`'s raw/cooked
dance (its editor needs raw per-keystroke input), so it lands with LS-8c.

**The ECHO-off hard guarantee.** With `CONS_ECHO` clear, NO input byte reaches
the console output — every echo (the typed char, the erase `"\b \b"`, the NL) is
gated by the flag. The password mask is thus a kernel property, not a
cooperative one (a consumer cannot accidentally echo via the cooked erase/redraw).

### `/dev/consctl` — the control surface (the Plan 9 idiom, not ioctl)

`cons_set_mode_cmd` parses one write: whitespace-separated `+name`/`-name`
tokens (`name` in `{icanon,echo,isig,icrnl,onlcr}`); `+` sets, `-` clears. ALL
tokens are parsed before any is applied — a single malformed token rejects the
whole write (`-1`, no change), so a multi-flag write is atomic (the `tcsetattr`
seam). `cons_render_mode` renders the current mode back as five `+name`/`-name`
tokens + `'\n'` (34 bytes; the symmetric `tcgetattr` seam). Phase-8 Pouch maps
`tcsetattr`/`tcgetattr` ↔ these strings at the boundary-line. Today `devdev.c`'s
`/dev/consctl` leaf routes its write→`cons_set_mode_cmd` and read→`cons_render_mode`
(offset-sliced for read-to-EOF). Its **open** is still I-27 console-attach-gated
(the mint gate — only the console holder can name + open it), but since **#94-B**
its **I/O is NOT re-gated**: a delegated holder of an INHERITED consctl fd
(`/sbin/login`, the session shell) drives the line discipline without being
console-attached. Sound because the open-mint gate + `CWALKONLY`/#81 (which rejects
an O_PATH-walked handle at `sys_read`/`write`/`readdir` before `dev->read`/`write`)
mean a consctl fd reaches a non-attached Proc ONLY by deliberate spawn-inheritance
from the trusted chain (joey, console-attached, opens it pre-relinquish and hands
it down) — the inherited fd is the capability. consctl is a control surface (the
five mode flags); it can never read console INPUT, so an ungated consctl write
cannot exfiltrate a keystroke. **cons (the data leaf) keeps its full I/O re-gate**
(console-input theft is the A-5a-F2 break). See `devdev_console_gate_ok` +
`dev_kind_is_cons_io`.

**The session shell owns the line discipline (#94-B-b).** The consctl fd reaches
the session shell `ut` one hop past login: `/sbin/login` forwards its inherited
consctl fd to `ut` via the new `libthyla-rs Command::inherit_fd(fd)` (the spawn
fd_list grows from the 3 stdio slots to `3+N`, the extra fd landing at the child's
fd 3) plus the arg `--consctl-fd 3`. `ut` — a USER-identity, NON-console-attached
Proc — parses the arg, holds the fd on its `Repl` (`Env.consctl_fd`), and
establishes its prompt-mode line discipline through it (`Repl::console_apply_default`
writes the shared `PROMPT_MODE` = `-icanon -echo +isig -icrnl +onlcr`: raw
byte-at-a-time so the U-4 line editor draws its own echo; ISIG so Ctrl-C cooks to
the `interrupt` note `ut` services; ONLCR stays ON — output post-processing is
orthogonal to raw input, and a console-direct session's children write plain `\n`
line endings only the kernel's ONLCR arm can cook. The pre-fix `-onlcr` sent bare
LF to the wire for the whole session; QEMU's `mon:stdio` mux silently re-inserted
CRs for terminal viewers, so the gap surfaced only when the Aurora fbcon — the
first honest raw renderer — drew the `ls` staircase). The boot witness is `ut: consctl ok`. This is
the controlling-terminal model — the foreground session shell, not login, owns the
tty termios — and it is I-27-safe: `ut` is never console-attached (the inherited
consctl fd confers no attach), it holds the fd PRIVATELY (it never re-forwards it
to a user child it spawns), and consctl cannot read console INPUT. The
`raw/cooked dance` around a foreground child (set the mode it needs + switch its
stdin `Piped`→`Inherit` + restore `ut`'s mode after) is **LS-7** — the editor arc,
where the child's mode needs are known; `Repl::console_apply_default` is the
primitive it builds on.

A consctl write that applies a mode also **discards any half-assembled canonical
line** (resets `g_cons.line_len` under `g_cons.lock` — the `tcsetattr` TCSAFLUSH
discipline). So a `canonical → raw → canonical` flip can never strand a fragment
that then prepends the next line, and the production path matches the test hook
`cons_test_set_termios` ("a mode flip starts a fresh line"). No v1.0 consumer
flips mid-line (login flips between completed reads; `ut` at prompt boundaries),
but the kernel is unambiguous against any consctl writer (LS-8 audit F1).

## The console winsize + `tty:winch` — #55 (as-built)

Design: ARCH §23.5.3 (the scripture pass, user-voted 2026-07-22). The console
gains the pts winsize contract — one kernel-held size, one writer, one signal,
two read paths.

**State.** `g_cons.ws_cols` / `g_cons.ws_rows` (u16 pair, the Linux
unsigned-short band; `0×0` = never set — the serial posture) plus the
diagnostic `winch_events` counter, all under `g_cons.lock` beside the termios
word. `cons_winsize_get(&c, &r)` takes a coherent snapshot (one lock hold — a
reader never sees a torn pair across a concurrent verb apply).
`cons_test_reset` zeroes all three.

**The verb.** `cons_set_mode_cmd` grammar grows `winsize <cols> <rows>` (the
ptyfs PTY-2c verb, byte-identical): the keyword token, then two decimal tokens
each in `[0, 65535]` (`cons_parse_u16_token`). It stages with the flag masks —
the whole write stays atomic (a malformed winsize rejects the batch, flags
included; `"winsizeX"` is not the verb — the keyword must end at
whitespace/EOL). Flags and winsize can mix in one write.

**The signal.** Under `g_cons.lock` the apply compares the staged pair against
the current one; **iff changed** it stores, bumps `winch_events`, and captures
a local flag. AFTER the lock drops, `proc_console_post_winch()` (kernel/proc.c)
posts `tty:winch` (`NOTE_NAME_TTY_WINCH` — informational, catchable,
kernel-only-POST) to the console OWNER's PGRP under ONE `g_proc_table_lock`
hold (the `notes_post_pgrp` walk body; pgid 0 — the boot group — refused, so a
bringup winch posts nothing). The pgrp, not the owner Proc: userspace cannot
post `tty:*` (the PTY-1b F4 gate), so owner-only would strand every child; the
console has no controlling-terminal / fg-pgrp model at v1.0 (`ct_sid` is
pts-registry state), so the owner's pgid is the minimal correct set — the fg
refinement is the recorded seam. Lock discipline: no `g_cons.lock` →
`g_proc_table_lock` edge (the post runs lock-free of the cons layer); the path
is process-context-only (a consctl write — never reachable from
`cons_rx_input`/IRQ, unlike the ISIG cook, so no `console_mgr` deferral).

**Read paths.** `cons_render_mode` now ends the mode line with
`winsize <cols> <rows>\n` (the ptyfs `ctl_render` shape: parser parity — pouch
0021's `strstr(buf, "winsize ")` works on either ctl; the render is 46+ bytes,
callers grew from 40 to 64). `cons_render_winsize` renders the standalone
`winsize <cols> <rows>\n` line (max 21 bytes) that the UNGATED `/dev/winsize`
devdev leaf serves — the app-facing readback (apps cannot mint consctl). The
in-band CPR probe stays the universal fallback and the only serial path (the
host terminal answers it); the deterministic client rule is: read
`/dev/winsize`; if `0 0`, CPR.

**The writer.** The renderer (aurora) — it self-serves a consctl fd by name
under the #55 mint-gate widening (see `109-devdev.md`) and writes the verb at
first present + every reweave (AURORA.md §4). The verb is accepted from any
consctl holder like the five flags (#94-B: the inherited fd is the
capability); in practice only aurora writes it.

**The renderer-minted consctl is WINSIZE-ONLY (#55 audit F2).**
`cons_set_mode_cmd(buf, n, allow_flags)` — `allow_flags=false` rejects any
`+`/`-` termios flag token, accepting only the winsize verb. A consctl Spoor
minted by the RENDERER (the widened gate) carries `CCONSWINSZONLY` (set at
`devdev_open`), so the write handler passes `allow_flags=false`; the trusted
attached chain (login/ut's inherited consctl) is unmarked → full grammar. This
closes the F2 hole: the renderer holds consfeed (input injection it feeds),
which dominates a *geometry report* but NOT a flip of the *global* cooking word
— that word also governs the serial RX path (`cons_rx_input`), so `+echo` from a
compromised renderer would unmask a concurrent serial-typed password into the
drain it reads (the ECHO-off HARD guarantee defeated). Winsize confers no
input-domain authority, so the widening is sound once restricted to it.

**The is-a-cons stat contract.** `cons_stat_native_fill` (shared by
`devcons.stat_native` + devdev's cons-leaf arm) fills a `t_stat`: zero-fill
(I-13), `T_S_IFCHR | 0620`, SYSTEM-owned, `qid_path` carrying
`CONS_STAT_QID_FLAG` (**bit 41** — disjoint from ptyfs's `PTS_FLAG` bit 40
under the shared S_IFCHR posture; /net's bit-40 qids report S_IFREG and fail
the S_ISCHR pre-gate). `devno` is stamped by `spoor_stat_native` (#100), not
the fill. This retires the statless-cons latent: fstat on a cons fd returned
−1, pouch 0021 folded that to ENOTTY, so `isatty()` was FALSE on the console
and musl stdio ran fully-buffered — the 0021 cons arm (55c) restores isatty
truth and serves TIOCGWINSZ from `/dev/winsize`.

Tests: `cons.winsize_roundtrip` (verb + both renders + snapshot agree;
malformed-batch atomicity), `cons.winsize_winch_iff_changed` (counter: change
→ +1, rewrite → +0, flags-only → +0), `cons.stat_native_qid_contract`
(S_IFCHR + bit-41 + zero-fill over poison), `devdev.consctl_renderer_mint`
(**revert-probed**: widening the gate to `cons` fails exactly the
"cons mint STILL DENIED" assert, 1194/1195), `devdev.winsize_leaf`.

## State machines

- **The poller** (`cons_poll.tla` `Poller`): `start` → `registered` (hook
  installed + readiness sampled) → `done` (POLLIN ready) | `sleeping` (parked) →
  (re-`registered` on the mgr's wake) → `done`.
- **The console_mgr** (`cons_poll.tla` `Mgr`): `sleeping` → `awake` (woken by an
  RX flag) → drain + walk + re-`sleeping`.

## Spec cross-reference

- `specs/cons_poll.tla` — the LS-8a deferred poll-wake (I-9 across the
  IRQ→console_mgr→hook-list relay). Clean (31 states) + liveness
  (`PollerEventuallyServed`) + `BUGGY_MGR_LOST_WAKE` (`NoMissedConsPoll`
  counterexample, depth 9). Source map in `specs/SPEC-TO-CODE.md`.
- `specs/poll.tla` owns the poller-side register-then-observe + the N-fd fan +
  `NoStaleHook`; `cons_poll.tla` adds the relay's second register-then-observe.

## Tests (`kernel/test/test_cons.c`)

- `cons.poll_readiness` — POLLIN iff the ring is non-empty; POLLOUT always.
- `cons.poll_deferred_wake` — the full relay through the **real** boot
  `console_mgr` kthread: register a hook → `cons_rx_input` a byte (arms
  `poll_wake_pending`, the hook stays NOT ready — proving the deferral) → yield →
  the mgr walks → the hook is ready. A lost relay would leave it unready
  (`NoMissedConsPoll`).
- (LS-8b) `cons.termios_default` — the default is `CONS_ISIG` only; raw
  byte-at-a-time + Ctrl-C-note + no-echo (the no-breakage guarantee).
- (LS-8b) `cons.cook_canonical_line` — assemble + erase (BS) + deliver-on-Enter;
  the ring sees only the edited line + NL; echo = typed + `"\b \b"` + NL.
- (LS-8b) `cons.cook_echo_off_no_output` — the ECHO-off hard guarantee: a typed
  line echoes **zero** bytes yet still delivers to the reader (the password mask).
- (LS-8b) `cons.cook_isig_toggle` — ISIG set → Ctrl-C is the note; clear → `0x03`
  is ring data.
- (LS-8b) `cons.cook_icrnl` — input CR → NL when set; verbatim when clear.
- (LS-8b) `cons.cook_onlcr_output` — output NL → CR NL when set; bare LF when
  clear (via the `cons_emit` capture sink).
- (LS-8b) `cons.consctl_parse` — `+name`/`-name` set/clear; atomic multi-token;
  malformed (`+bogus`, missing sign, empty, one bad token) → `-1`, no change.
- (LS-8b) `cons.consctl_render` — the read-back string for default + all-set; a
  too-small buffer renders nothing.
- (LS-8b) `cons.cook_line_overflow` — a pathologically long line is bounded (the
  line buffer never overflows past `CONS_LINE_MAX`; ASAN-clean).
- (LS-8 audit F1) `cons.cook_mode_flip_fresh_line` — drives the **production**
  `cons_set_mode_cmd`: a buffered fragment is discarded by a mode change, so only
  the post-flip line delivers (pre-fix it prepended `"abc\n"`).
- (LS-8 audit F2a) `cons.cook_canonical_poll_edge` — a multi-byte canonical line
  arms the empty→non-empty poll edge **once** on the Enter flush (the chars buffer
  with the ring empty → no edge while assembling); the deferred mgr walk then makes
  the hook ready.
- (#94-B) `devdev.cons_gate` — the I-27 gate split on the **namespace** path:
  **cons** stays fully I/O-re-gated (non-attached read/write → `-1`, `poll` →
  `POLLNVAL`); **consctl** keeps its open-mint gate but its I/O is **ungated**
  (non-attached read renders the mode line, write applies + takes effect [asserted
  via `cons_test_termios`], `poll` is always-ready). The non-attached consctl
  write is restored via `cons_test_set_termios` so the probe is non-destructive.
- (A-4c) `cons.blocking_read_wakeup`, `cons.ctrlc_consumed`,
  `cons.break_sets_sak`, `cons.sak_via_console_mgr`, the SAK/owner role-split set.

## The TX ring + the writer role — #75 / P1-F (as-built)

Design: **ARCH §23.5.2**. Before P1-F, `cons_output_write` walked byte-by-byte
into a **lock-free** `uart_putc` holding no lock across the loop, so two CPUs
writing `/dev/cons` interleaved at **byte** granularity — shredding multi-byte
glyphs (the 3-byte `⊢`, U+22A2) and SGR escapes in 10 of 40 gate boots (#75).
It was **pre-existing since P4-B**: `cons_output_write`'s entire history is
`6f417e93` / `71d52541` / `966d9341`, and none ever held a lock.

The tell was an asymmetry — console *input* was serialized under `g_cons.lock`
while console *output* was serialized nowhere. And the blast radius was never
just the log: `cons_drain_tap` (G-4) appends per byte under its own lock, so
aurora's drain ring received the same **interleaved order**; a torn cursor or
alt-screen escape corrupts a live TUI.

Two separable mechanisms:

| | What it buys | Why it is needed |
|---|---|---|
| **The ring** | Decouples the writer from `uart_putc`'s bounded-but-slow TXFF spin (#67: up to 20 ms/byte against a stalled host consumer) | A push is a memory write under a leaf spinlock; the PL011 TX interrupt drains ring → FIFO. Per byte it trades an MMIO `FR` read + `DR` write for a spinlock + a store — a win, especially under HVF where each MMIO is a vmexit |
| **The writer role** | Makes a whole `cons_output_write` call atomic against other console writers | A write larger than the ring must sleep for room, dropping the ring lock — so the ring lock alone can never span the call |

The role is the audited `srvconn.c::chan_role_acquire` shape (#354 / CF-3 B)
reused in structure: park on a `poll_waiter_list` with register-then-observe,
`TSLEEP_INTR` unwind, re-contend on wake. It is a **sleeping park, not a
spinlock** — a long write makes peers wait but never pins a CPU.

**In the healthy case the interrupt is never even armed.** The post-write
`cons_tx_kick` hands the FIFO everything it will take; `TXIM` is armed only for
a remainder. So behaviour matches the old direct path minus the spin.

### The two producers have opposite blocking contracts

This asymmetry is **load-bearing** — a change that blurs it is a bug:

- `cons_output_write` runs in **process context** (`spoor_write_common` holds no
  lock across `dev->write` and documents it as *blocking*), so it **may** sleep
  for room, and does (`cons_emit_wait`).
- Echo from `cons_rx_input` runs in **IRQ context**, so it must **never** sleep:
  it pushes non-blocking and **drops** on a full ring (a tty overrun — the same
  disposition the drain ring uses).

Exactly **one** thread can ever wait on `g_cons_tx_room`, because only the role
holder pushes-with-wait and the role is exclusive. That is what makes a
single-waiter `Rendez` sound here where the role itself needs a waiter list.
**If a second waiter is ever introduced this must become a `poll_waiter_list`.**

### Invariants a change must preserve

- **`TXIM` armed iff the ring is non-empty**, decided in the *same* critical
  section as the ring mutation (`cons_tx_drain_locked`). A non-empty ring left
  with TX interrupts off is a silently wedged console.
- **`IMSC` is a shared RMW register.** RX masks `RXIM|RTIM` under
  `g_uart_rx_lock`; TX masks `TXIM` under the ring lock. Two outer locks doing
  RMW on one register lose updates, so **every** `IMSC` RMW goes through
  `uart_imsc_update`'s leaf lock (`g_uart_imsc_lock`).
- **The #67 loss discipline is inherited, not weakened.** A stalled host consumer
  stops the TX IRQ, so the room-wait is a **deadlined** `tsleep`; on timeout the
  writer drops the remainder and returns a **short write**. A bounded-but-lossy
  console beats a wedged writer — the ring must never convert a stalled
  *consumer* into a wedged *writer*.
- **The ring lock is a pure irqsave leaf.** The IRQ drain wakes *after* releasing
  it (the `cons_drain_tap` discipline), and `wakeup()` is the only IRQ-safe wake
  primitive (LS-8a).
- **Halls / extinction bypass the ring** (`cons_tx_flush_for_dump`): a dying
  machine runs IRQ-masked and cannot depend on an interrupt to drain, so the dump
  flushes by **trylock only** (a dying CPU may already hold the lock) and then
  falls back to the direct bounded `uart_putc`.
- **Arming.** The ring arms at `cons_tx_arm()`, after `gic_attach` +
  `gic_enable_irq` for the UART SPI. Every pre-GIC print takes the direct path;
  the ring is empty at arm time so the transition cannot reorder output. The
  `Thylacine boot OK` / `EXTINCTION:` tooling ABI (TOOLING.md §10) is
  byte-unchanged on both paths.

### What the guarantee is — and is not (the #75-audit F1 correction)

The role delivers atomicity **against other `cons_output_write` writers** — the
observed #75 bug (`jc-probe` vs the `ut` prompt SGR, #74's trigger). It is **not**
"no byte ever lands mid-write": the impl pushes one byte per `g_cons_tx.lock`
acquire/release, so:

- **Echo (IRQ context, `cons_emit`, one byte at a time) may interpose on a
  program write's byte stream.** In cooked `CONS_ECHO` mode a typed-and-echoed
  keystroke can land between two bytes of a program's glyph on the wire. This is
  **unchanged from pre-#75** (echo and program output already interleaved byte-wise
  through the lock-free `uart_putc`) — #75 does not regress it, it just does not
  close it. Narrow v1.0 reachability: `ut` runs raw (echoes itself), a login
  passphrase is echo-off. **Full echo-exclusion via a bulk-push fast path** (push
  a ring-fitting write's whole cooked run under one lock hold) is a v1.x
  enhancement (#79); it carries a two-ring (serial + the G-4 aurora drain)
  lock-ordering design not worth rushing onto the trusted-path surface.
- ~~**`SYS_PUTS` (`t_putstr`) bypasses the ring + role.**~~ **CLOSED at #76.**
  `sys_puts_handler` now stages the user buffer into a `u8 scratch[SYS_RW_STACK]`
  with one `uaccess_copy_in` and emits it through `cons_output_write`, so it is
  role-held, ring-buffered, drain-tapped and ONLCR-cooked — identical treatment to
  a `/dev/cons` write. See "SYS_PUTS joins the shared path (#76)" below.
- **A debug/job-control stop mid-`cons_output_write` holds the writer role** for
  the stop's duration; other console writers park (never spin/extinct) until it
  resumes. A new liveness seam (the console analog of the #89/8c-3 reader-role
  freeze), bounded to one write call — v1.x release-on-stop.
- **Kernel prints (`uart_puts`) keep the direct path**, so a kernel print can
  still interleave with an EL0 write. They must work pre-GIC, in IRQ context and
  inside extinction, where the ring is unavailable by construction. Post-boot
  kernel prints are diagnostic and rare; tightening this is v1.x.

### SYS_PUTS joins the shared path (#76, as-built)

P1-F converted `cons_output_write` but left `sys_puts_handler`'s byte-by-byte
lock-free `uart_putc` loop in place as a *documented, deliberate* seam: the
diagnostic channel, the reasoning went, must work independently of the console
Dev, and the accepted price was interleaving at the FIFO. The seam was live and
the price was underestimated on both counts.

**The predicted cost arrived, on the trusted path.** LS-CI caught a login prompt
rendered as `patapestrssyd: mworodd:e` — `"password: "` (login, via fd 1 →
`cons_output_write`, role-held) shredded byte-for-byte by `"tapestryd: mode "`
(tapestryd, via `t_putstr`). This is exactly the "two programs, one on each path"
case §23.5.2 named. SYS_PUTS is not a niche channel: **83 binaries** reach it via
`libthyla_rs::t_putstr` / `libt::t_puts`, making it *the* native diagnostic
stream. A role only some writers take excludes nobody.

**The unstated cost was the worse one.** `cons_drain_tap` fires from `cons_emit`
and `cons_emit_wait` only, so nothing written via SYS_PUTS ever reached the G-4
renderer. Under a graphical console the seam's own justification inverts: the
direct path did not make the diagnostic channel *more* reliably available, it
made it available **never** — perfectly normal on serial, absent on the
framebuffer.

**The independence rationale never required the direct path.** `cons_output_write`
is the shared cons-layer function (#57b), not the Dev: reaching it needs no fd,
Spoor, handle, namespace or open. SYS_PUTS keeps every bit of its independence by
calling it. Pre-GIC boot still falls through to the direct bounded `uart_putc`
inside `cons_tx_push_nowait`, and extinction/Halls are untouched (they use
`uart_puts` / HX-I paths, and a dying kernel issues no EL0 syscall).

Three deliberate consequences:

- **The copy-in happens BEFORE the role is claimed.** Faulting a user page can
  sleep, and holding the console role across an unbounded page-in would stall
  every other console writer behind it. Staging first bounds the role to the
  emit. (`cons_output_write` may sleep at all only because `spoor_write_common`
  holds no lock across `dev->write`.)
- **A copy-in fault now emits nothing** — whole-op EFAULT. The old loop pushed
  the readable prefix to the console before failing.
- **The return may be SHORT** where it was previously len-or-−1: `cons_output_write`
  cuts a write off on the #67 stalled-consumer deadline or a #811 death, and
  reporting that honestly beats claiming bytes that were dropped. No caller
  inspects the count — every `t_puts`/`t_putstr` use in `usr/` is for side effect.

**ONLCR is load-bearing here, not cosmetic.** Closing the tap gap newly exposes
this output to aurora, whose VT does not synthesize CR on LF (#36), so bare-LF
writes would staircase the moment they became visible. Routing through
`cons_output_write` fixes the visibility and the line endings in one stroke.

### Kernel diagnostics join the shared path (#126, as-built)

`cons_diag_puts` / `cons_diag_putdec` / `cons_diag_puthex64` are the
**non-blocking** kernel emitters: for any steady-state diagnostic issued from a
context that can neither sleep nor spin — IRQ context, or under a spinlock.

    void cons_diag_puts(const char *s);      // ONLCR-translating, NULL-safe
    void cons_diag_putdec(u64 v);
    void cons_diag_puthex64(u64 v);

Each byte goes `cons_drain_tap` → `cons_tx_push_nowait` (drop + count on a full
ring, the echo disposition), then one `cons_tx_kick` per call rather than per
byte. They never sleep, never spin, take only leaf locks, and wake outside them
— the same path `cons_emit` takes for echo from the UART RX IRQ, which is the
strongest available precedent for "legal in a constrained context". Pre-arm they
fall through to the direct bounded `uart_putc`, so boot output is byte-identical.

**Why they exist.** `uart_putc`'s #67 bound is 20 ms *per byte*, and it does not
compose. `proc_reparent_children` emitted the ~90-byte #80 orphan line while
holding `g_proc_table_lock` (taken irqsave 40 times in `proc.c`, plain 0 times),
so a stalled host consumer pinned the global process-table lock IRQ-masked for
~1.8 s per adoption — the interrupt-dead stall #67's bound exists to prevent,
reconstituted by iterating it. **A per-item bound is not a per-operation bound.**

**Ordering.** `cons_diag_*` is callable under any lock ordered above the cons
locks. It takes `g_cons_drain.lock` and `g_cons_tx.lock` (both leaves;
`g_cons_tx.lock` nests only `g_uart_imsc_lock`) and wakes after releasing them.
It does **not** touch `g_cons.lock`, so the standing "never hold `g_cons.lock`
across `g_proc_table_lock`" obligation (see `cons_service_deferred` and the #55
winch post) is untouched — that edge runs the other way.

**Deliberately NOT capture-aware.** Unlike `cons_emit`, these skip
`g_cons_echo_capture`: that 128-byte buffer exists so a test can assert exactly
what was *echoed*, and a kernel diagnostic landing in it would corrupt the
assertions it exists for.

Converted callers: `proc_reparent_children` and `proc_fault_terminate`.
`kernel/proc.c` now has zero direct `uart_*` calls.

### Coverage

`proc.orphan_diag_uses_cons_path` pins #126, by the #76 method: arm the drain,
force a real orphan adoption, require the line verbatim in the tap. The
assertion is **routing, not duration** — `uart_test_tx_stall()` gates
`uart_tx_try_putc` only and never `uart_putc`, so an "elapsed < N ms" assertion
would pass identically pre- and post-fix (satisfiable by the broken system).
Revert-probed: restoring the `uart_puts` calls yields `1237/1238 FAIL`, with the
orphan line still *present* in the log via the direct path — the test fails on
the byte's route, not on its absence, which is exactly the discrimination
wanted. It fails equally if the diagnostic is simply deleted.

`cons.sys_puts_uses_shared_console_path` pins #76. It has to spawn a **real EL0
binary** (`/hello`, whose entire output is one `t_putstr`): `sys_puts_handler`
takes a USER VA and kproc has `pgtable_root == 0`, so no in-kernel caller can
reach it at all — a unit test of the handler is not awkward but *impossible*. The
drain is the observable: the role's absence shows only under a race, the tap's
absence shows from one thread, and both live behind the same call, so proving
SYS_PUTS reaches the drain proves it took the shared path. The count is sampled
with the **non-blocking** `cons_test_drain_count()` before any `cons_drain_read()`
— that read *sleeps* on an armed-but-empty drain, so reading first would turn a
pre-fix run into a boot **hang** instead of a failed test, and a hang reports
nothing. **Revert-probed**: restoring the `uart_putc` loop gives 1216/1217 FAIL on
exactly `SYS_PUTS output reached the drain tap`.

`cons.tx_role_serializes_writers` pins the property: with the role held, a second
`cons_output_write` **parks** and emits nothing; on release it completes and its
bytes land contiguous. **Revert-probed** — deleting `cons_tx_role_acquire` from
`cons_output_write` makes the suite read 1188/1189 FAIL on exactly that test.

The ring's **steady-state** path needs no dedicated test: every byte of console
output on every boot flows through it, so a ring bug means no boot at all — the
full-suite boot, the login, and the aurora renderer are its integration proof.
The byte-interleave itself is an SMP race that no deterministic single-threaded
test can reproduce; the SMP gate and the `⊢`-tearing signature at 0/40 are its
runtime witness.

Its **back-pressure** path is a different matter, and `cons.tx_room_wait_and_deadline`
(the #75-audit F2 item, owed at the close and now discharged) covers it. The role
test above runs under echo capture, which short-circuits `cons_emit_wait` *before*
`cons_tx_push_nowait` — so it never reaches the ring, and the two legs that only
run when the ring fills had no deterministic proof at all:

- **(A) the #67 deadline.** `uart_test_tx_stall(true)` emulates a stalled host
  consumer, a `cap + 64` write is issued, and the call must return a **short
  count** having actually waited (`elapsed >= CONS_TX_ROOM_WAIT_NS`). This is the
  anti-wedge property of the trusted-path console: were it to regress, a paused
  terminal would hang every console writer rather than dropping bytes — and a
  hang here takes the console with it, so the failure would be silent.
- **(B) the room-wait I-9 wake.** A second writer parks on the full ring (proved
  by the `room_waits` counter rising, not by inferring from "it ran and has not
  finished"); the test then silently frees all but two bytes (`wake == false`, so
  the sleeper stays parked) and lets the **real** `cons_tx_drain_from_irq` move
  those two. The wake under test is therefore production's own `freed`-gated
  `wakeup()`, not a re-implementation in the test.

Both legs are **revert-probed against production code**: making `cons_emit_wait`
drop instantly instead of parking fails (A)'s elapsed assertion, and deleting the
`if (freed) wakeup(...)` from `cons_tx_drain_from_irq` fails (B).

Two properties of the test are deliberate rather than incidental. It **discards**
its ring-sized filler instead of flushing it — only the two bytes (B) genuinely
drains ever reach the wire, and those are spaces, so the boot log the gates parse
is not polluted. And every assertion sits **outside** the stalled window, because
`TEST_ASSERT` returns on failure: an assertion inside it would leave the ring
stalled for the rest of the boot, and later tests writing real console bytes would
then block 20 ms per byte against a ring that can never drain — converting a clean
FAIL into a mystery timeout. A test's failure mode must not destroy the diagnosis.

`cons_test_tx_room_waits()` exposes a counter that is also a genuine production
diagnostic: a rising `room_waits` is the console reporting that writers are
back-pressuring on a slow consumer, the sibling of `dropped` (which counts the
ones that gave up).

## Input-drop instrumentation -- #95 (as-built)

Three sites on the RX path discard a byte, and until #95 all three did it in
total silence. That mattered because of the shape the loss takes: a dropped
input byte truncates a command, which then **runs anyway**. #95 saw `sleep 30`
arrive as `sleep 3` and there was nothing anywhere in the tree that could say
whether the kernel had dropped it. (The TX side has had `g_cons_tx.dropped`
since #75/#126; the RX side had nothing.)

Each site now increments its own counter in `struct cons_input`, under
`g_cons.lock` -- which all three already hold:

**#129 renamed two of these**, because the fix changed what they measure: the
ring-full sites now *refuse* a byte (back-pressure -- the producer keeps it and
retries) rather than dropping it. A counter called `rx_drop_raw` that counts
non-losses is wrong in the most expensive way: read at 3am, believed, and
pointing at data loss that did not happen.

| Counter | Site | Notes |
|---|---|---|
| `rx_bp_raw` | `cons_rx_input`, raw/cbreak arm | **back-pressure, not loss.** The ring was full and the byte was REFUSED; the UART holds it in the FIFO or the 1-byte holdback, and `cons_feed_write` returns a short count. Non-zero is a load signal. |
| `rx_bp_flush` | `cons_rx_input`, cooked Enter-flush | the whole `line_len + 1` flush did not fit, so it was refused **as a unit** with the line left intact. Re-offering the terminator after a drain delivers the whole line. |
| `rx_drop_line` | `cons_rx_input`, line assembly | **a real drop:** a byte past `CONS_LINE_MAX`, un-echoed. Deliberately still a drop -- back-pressuring a fixed-size line buffer would wedge on a user who never presses Enter. |
| `rx_drop_ring` | `cons_ring_push` after the room check | **must stay zero.** A push failed after the under-lock room check authorized it, i.e. `cons_ring_room()` disagrees with the ring. An invariant *witness*, not a diagnostic: it has no reachable driver by construction, and that is the claim it exists to falsify. |

Read them at `/ctl/cons` (`cons_rx_counters` / `cons_tx_drops`).

### The one-shot report

Counting alone does not help an unattended gate: nobody reads `/ctl/cons` at
the moment a scenario fails. So a drop also arms `drop_report_pending`, and
`console_mgr` emits ONE line in process context (the intr/sak/pollwake deferred
relay -- the drop sites run in IRQ context under `g_cons.lock` and must not
emit themselves):

```
cons: INPUT DROP (#95) line=3 ring=0 (bp raw=0 flush=12) -- further drops counted silently at /ctl/cons
```

**Only a real loss arms the latch (#129).** The two back-pressure counters ride
along as context -- they say how hard the console was being pushed when the loss
happened -- but they never trigger the report. Back-pressure is normal operation
on a busy console, so reporting it would be a false alarm AND would *spend* the
one-shot latch, leaving a genuine loss later in the same boot silent. That is
the #95 lesson applied to the fix itself: an instrument disarmed by routine
events is disarmed exactly when it matters.

`drop_reported` then latches it off for the life of the boot: a pathological
drop storm must not become a diagnostic storm that costs more than the events
it reports (the #126 lesson).

### Why the report is gated on `boot_is_complete()`

The kernel test suite deliberately overflows this ring --
`cons.ring_full_refuses` pushes 522 bytes into 512, and
`cons.rx_drop_counters` drives every site on purpose. Ungated, every boot
would print an alarming INPUT DROP line during the test phase and, far worse,
the test would **spend the one-shot latch**, so a real drop later in the same
boot would print nothing: the instrument disarmed by its own test. Kernel tests
run before boot-complete and every real input workload runs after, so the gate
makes the test phase silent and leaves the latch armed for exactly the window
that matters. Counting is unconditional.

### What this does NOT establish

The counters are an instrument, not a diagnosis. #95 remains **unexplained**.

What #129 changed is that two of the three candidate sites can no longer lose a
byte at all, which *narrows* #95 rather than settling it. The flush site never
matched #95's shape anyway: it lost the **tail** of the line and then the
newline, so a short flush meant the line never executed -- whereas #95 showed an
interior byte lost with the terminator delivered and the command running. The
raw arm's per-byte push against a concurrently draining reader was the only
shape that matched, and it is now back-pressured on both producers.

So a recurrence of #95 after #129 would be evidence that the cause was never in
this layer. The `rx_bp_*` counters are what make that reading possible: they
say the site was *live* (exercised and handled) rather than merely untouched,
which a drop counter reading zero never could.

## Error paths

- `cons_output_write`: returns a **short count** (not an error) when the #67
  room-wait deadline fires against a stalled host consumer, or when a #811
  death-interrupt unwinds it mid-write; `-1` only if the role acquire is
  death-interrupted before any byte was written. It never hangs.
- `cons_input_read`: `-1` on NULL buf / `n < 0` / a second concurrent reader
  (single-reader guard); `0` on `n == 0` or a death-interrupt with nothing
  buffered.
- `cons_poll`: never errors (returns the ready revents; `0` if neither requested
  event is ready). `devdev_poll` returns `POLLNVAL` for a non-console-attached
  caller of `/dev/cons` (the I-27 gate, like cons read/write). `/dev/consctl` poll
  is **ungated** since #94-B (always-ready; consistent with its ungated I/O —
  consctl installs no data-readiness hook + has no input timing to leak).

## Known caveats / footguns

- **POLLOUT is always ready** (the UART never blocks). A poller that requests
  `POLLIN | POLLOUT` on the console will **always** see POLLOUT and never wait for
  input — so a consumer waiting for input must poll for **POLLIN only** (LS-8c's
  shell loop does). This is correct POSIX (an always-writable fd).
- **`devcons` (the syscall fd) poll/read are gated at `SYS_CONSOLE_OPEN`, not
  re-checked** — consistent with `devcons_read`. The O_PATH bypass that motivated
  re-gating the `devdev` path (#57b/#81) does not reach `devcons` (it is minted
  only by the gated syscall, never walked in the namespace).
- The single-console termios state (LS-8b) is **global** to the one v1.0
  console; per-fd termios needs `/dev/pts` (Phase 8). Two concurrent input
  sources interleave into one shared line buffer (under `g_cons.lock` — no
  corruption, but bytes from two typists mix); v1.0 has one console.
- **Canonical-mode line termination needs `CONS_ICRNL`.** A terminal sends CR on
  Enter; without ICRNL a CR is buffered as an ordinary char (it does NOT
  terminate the line). Cooked consumers (login) set `ICANON|ECHO|ISIG|ICRNL`
  (+`ONLCR` for a clean line break on echo) — the Unix cooked-mode convention.
- **~~A line filling the entire ring drops its terminating NL.~~ CLOSED by #129.**
  This was the deeper half of #129 and it is worth keeping visible, because the
  obvious fix makes it worse. `CONS_LINE_MAX` was == `CONS_RING_SIZE` (both 256),
  so a maximal line + NL = 257 bytes did not fit the ring **even when empty** --
  the 256 chars filled it and the NL was dropped, silently turning a command into
  a different, shorter, unterminated one. The natural admission gate
  (`count + line_len + 1 <= CONS_RING_SIZE`) is correct in form but with those
  sizes refuses 257 into 256 *forever*: RX pauses permanently and the console
  wedges — a bounded drop traded for a deadlock. The gate is only sound once the
  ring can hold what it is asked to reserve, so `CONS_RING_SIZE` is now 512 with
  a `_Static_assert(CONS_LINE_MAX + 1 <= CONS_RING_SIZE)` tying the two together.
  Regression `cons.full_line_fits_ring` (fails on the old size in one direction
  and on the naive gate in the other); the static assert fails the **build** if
  either constant moves back.
- **`/dev/consctl` open is I-27 console-attach-gated; its I/O is NOT (since #94-B).**
  The session-leader that controls termios is the **non-attached** login (it reads
  the console via an inherited `SYS_CONSOLE_OPEN` fd; it cannot open the gated
  `/dev/consctl`). The console-mode-access fork was resolved **B (inherited consctl
  fd)** (user-voted 2026-06-12) over C (`SYS_CONSOLE_MODE(fd)`, which would deviate
  from the consctl-file scripture): the I/O re-gate is dropped for consctl, and the
  getty (joey, console-attached) opens `/dev/consctl` pre-relinquish and hands it
  to each login via spawn-fd inheritance (child fd 3) + `--consctl-fd 3`. login
  does the LS-6 dance (cooked+echo username / cooked-noecho passphrase / restore).
  Sound: the open-mint gate + `CWALKONLY`/#81 mean only the trusted chain ever
  holds a consctl fd — the inherited fd is the capability. The **ut raw/cooked
  dance** for foreground children (`ut` is the console *owner*, not attached, so it
  too needs the inherited fd) is **#94-B-b** (with `Command::inherit_fd` + the
  login→ut forward), co-located with LS-7.
- The echo/output **capture sink** (`g_cons_echo_capture`) is test-only — always
  false in production (the emit path is then one never-taken branch + `uart_putc`).
  It is always-compiled, consistent with the other `cons_test_*` hooks; #71 gates
  the file's test-support uniformly under `KERNEL_TESTS` later.
- **`cons_test_mgr_hold` (#58)** — the deterministic-dance hold: while set,
  `cons_mgr_pending` reads false, so a woken `console_mgr` re-parks WITHOUT
  consuming any pending flag (the flags persist; the release path wakes the
  rendez explicitly — no lost wake, I-9 intact; production never sets it, so
  the cond is byte-identical there). Exists because the two deferred-wake
  tests' single-runnable dance was racy on SMP: a woken mgr dispatched on a
  PEER CPU consumed the pending flag between the producer byte and the assert
  (~1-in-50 HVF boots), and the failing `TEST_ASSERT`'s early return LEAKED
  the test's stack `poll_waiter` on the list — the next walk extincted on the
  reused frame's clobbered magic (`EXTINCTION: pw_wake`, poll.c's stale-hook
  guard, 2026-07-21 — the guard caught real corruption; the corruption was a
  test-lifetime leak, not a production defect). Both tests now also run their
  dance through an error-string helper so the hook is unregistered on EVERY
  exit path — the structural rule: a stack poll hook never outlives its test.
  test-lifetime leak, not a production defect). All THREE cons poll-hook
  tests (`poll_deferred_wake`, `drain_poll_deferred_wake`,
  `cook_canonical_poll_edge` — the class sweep found the third carrying the
  identical shape) now run their dance through an error-string helper so the
  hook is unregistered on EVERY exit path — the structural rule: a stack
  poll hook never outlives its test.
  Direct-drive via `cons_test_service_deferred` bypasses the cond and is never
  blocked by the hold.
- **The PL011 RX IRQ handler MUST clear ICR *before* draining the FIFO (#172).**
  `arch/arm64/uart.c::uart_rx_handler` clears `RXIC|RTIC` at entry, then drains
  (bounded by `UART_RX_DRAIN_MAX=64`). QEMU's PL011 sets RXRIS on *receive* and
  does **not** recompute it from the FIFO level on an ICR clear, so clearing
  *after* the drain races a byte arriving in the post-drain window: its interrupt
  is cleared and — once the FIFO fills, `can_receive` goes 0 — never re-raised, so
  the FIFO wedges full and console RX dies (the whole-OS "freeze under fast
  input"). Clearing first means any byte arriving during/after the drain re-raises
  the interrupt for the next handler entry instead of being stranded. The bounded
  drain is the separate fix for the unbounded-loop livelock (an IRQ that never
  returns under HVF's concurrent FIFO refill). Verified by
  `tools/interactive/freeze-172.exp`.
- **RX backpressure: a full cons ring pauses RX, it does not drop (#174).** The
  shared drain (`uart_rx_drain_locked`, used by both the IRQ handler and the
  reader-side `uart_rx_pump`) checks `cons_rx_can_accept()` *before* reading each
  byte out of the FIFO. When the ring is full it leaves the byte in the FIFO,
  masks `IMSC.RXIM|RTIM`, and latches `g_rx_paused` — so the PL011 FIFO fills,
  QEMU's `can_receive` goes 0, and the host serial buffers the overflow. **No byte
  is lost on the raw byte-to-ring path** (the #172/#174 case — ut's line editor
  and nora, where each input byte is one ring slot) under an instantaneous input
  flood (e.g. a fast trackpad-scroll mapped to arrow keys). Resumption is
  **reader-driven**, never int_level-driven (the #172 wedge trap):
  `cons_input_read`, after draining ring bytes (freeing space), calls
  `uart_rx_pump`, which drains the held FIFO bytes into the freed space and
  unmasks RX once the FIFO empties. Lock order `g_uart_rx_lock -> g_cons.lock`;
  handler-vs-pump are mutually exclusive (paused ⇒ RX masked ⇒ no handler), and
  the single-reader guard means at most one pump runs. Predicate verified by
  `cons.rx_can_accept_boundary`; the end-to-end no-loss/no-wedge by an
  instantaneous-flood repro (`tools/interactive/flood-174.exp`).
  - **Scope of "no loss" (#174-audit F1, WIDENED by #129).** `cons_rx_can_accept()`
    gates on the **ring**. #174's guarantee covered the raw path only; #129
    extended it to the cooked Enter-flush and to the second producer, so the
    remaining loss site is exactly one:
    - In **canonical** mode an ordinary input byte is routed to the line buffer
      `g_cons.line[]` (`CONS_LINE_MAX`), not the ring — so a single line longer
      than `CONS_LINE_MAX` still **truncates** (the LS-8b bound; the byte is read
      from the FIFO and dropped at the line-buffer-full check, `rx_drop_line`).
      Extending backpressure here is deliberately NOT done, and the reason is
      structural rather than a cost tradeoff: it would strand the line's
      terminating Enter behind a full buffer, so the Enter could never terminate
      the line and free it. A 256-char single line is pathological for
      interactive cooked input (login reads short username/passphrase lines).
    - The **Enter-flush** is no longer a loss site (#129). It is checked as a
      unit under `g_cons.lock` and refused whole, leaving the assembled line
      intact for the retry — see `rx_bp_flush`.
    - **`cons_feed_write` is no longer ungated (#129).** It was the one producer
      #174 never covered: the G-4 renderer's entire keyboard, returning `n`
      unconditionally while the ring dropped the overflow. The serial console had
      back-pressure; the framebuffer console had a lie. It now returns a SHORT
      count, which is the ordinary POSIX answer and needs no new ABI.
  - **The refusal is side-effect-free, and that is the load-bearing property
    (#129).** `cons_rx_input` returns `false` only after changing *nothing* — no
    ring push, no `line_len` mutation, no echo, no flag — so the producer still
    owns the byte and re-offers the identical one. Echo moved inside the accepted
    branch for the same reason: echoing a refused byte would show the user a
    character the console did not take, then show it twice on the retry.
  - **The pre-check is an optimization; the guarantee is under the lock (#129).**
    `cons_rx_can_accept()` is lockless and now reserves `CONS_LINE_MAX + 1`
    unconditionally — it consults neither `termios` nor `line_len`, both of which
    another producer can change under it. Reading `line_len` there would re-open
    the hole (a stale-low value admits a byte whose flush then overruns); gating
    on the *current* mode is unsound too, since a byte admitted under a raw check
    can meet a cooked flush. The exact check lives in `cons_rx_input` under
    `g_cons.lock`, atomically with the push, which is the only construction that
    is airtight against two independent producers.
  - **The pause is PUBLISHED then RE-OBSERVED (#129-audit F1).** Masking RX and
    latching `g_rx_paused` is a check-then-act, and the observer on the other
    side is lockless: `uart_rx_pump`'s fast path reads the flag *without*
    `g_uart_rx_lock` and returns on false. So a reader that drained between the
    room check and the store saw `false`, did nothing, and then parked on an
    empty ring — while the pauser went on to mask RX. The terminal state is
    fatal rather than slow: RX masked gates off `uart_irq_handler`'s RX arm,
    `uart_rx_pump`'s ONLY caller is `cons_input_read` (the thread now parked),
    and on a serial-only console `cons_feed_write` does not exist — so no
    producer remains to make `cons_data_ready` true. The console is dead until
    reboot, and with RX masked the PL011 never raises `DR.BE` again, putting the
    **I-27 SAK out of reach too**. `uart_rx_pause_and_recheck` re-observes after
    publishing and lifts the pause if room appeared; the retry is budget-bounded
    so a flip-flopping peer producer cannot livelock the drain. The shape was
    pre-existing in the #174 gate arm — #129 replicated it at the holdback and
    then claimed the loss window was *closed*, which is what made it load-bearing
    to fix rather than inherit.
  - **Recovery distance grew with the ring (#129-audit F7).** `cons_rx_can_accept`
    trips at the same absolute threshold as before (`count > 255`), so usable
    depth before pausing is unchanged at 255. The *recovery* distance is not:
    pre-#129 a pause meant `count == 256 == CONS_RING_SIZE` and one byte read
    re-opened the gate, whereas `count` can now reach 512 (a cooked flush of 257
    from `count == 255`), and the gate re-opens only at `count <= 255`. A reader
    draining one byte at a time therefore needs up to 257 reads before RX
    unmasks — so a BREAK queued behind a pause waits longer. Bounded and
    reader-driven (not a hang), and every real reader drains in bulk, but it is a
    quantitative weakening of an already-imperfect I-27 property. A low-water
    resume mark would close it if it ever matters.
  - **The 1-byte RX holdback (#129).** Because the pre-check is lockless, a peer
    producer can take the room between the check and the push, and by then the
    byte is already out of `PL011 DR` and cannot be put back — the one way #174's
    "leave it in the FIFO" guarantee can be escaped. `uart_rx_drain_locked` parks
    such a byte in `g_rx_held_*` (under `g_uart_rx_lock`, like `g_rx_paused`),
    pauses, and re-offers it **before** touching the FIFO again so ordering is
    preserved. Without it the fix would merely narrow the loss window instead of
    closing it — and it would go on being invisible in the way #95 was.
  - **RX resume is gated on the console reader (#174-audit F3).** `uart_rx_pump`
    runs only from `cons_input_read`, so a paused RX resumes only when *some* Proc
    re-enters the console read. A console holder that stops reading (e.g. a
    foreground child that never reads stdin) parks RX paused — input is **buffered
    in the FIFO + host, never lost**, and flows the instant a reader returns. This
    is correct backpressure, not the #172 wedge (the kernel is not stuck; only the
    paused console waits on its consumer).
