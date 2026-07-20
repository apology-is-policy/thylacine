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
    u8          ring[CONS_RING_SIZE];  // 256, power-of-two, mask-indexed
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

## Error paths

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
- **A line filling the entire ring drops its terminating NL.** `CONS_LINE_MAX`
  == `CONS_RING_SIZE` (256), so a 256-char line + NL = 257 bytes; the 256 chars
  fill the ring and the NL is dropped (bounded, never corrupting). A real line is
  far shorter; this is the pathological edge (`cons.cook_line_overflow`).
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
  - **Scope of "no loss" (#174-audit F1).** `cons_rx_can_accept()` gates on the
    **ring**, so the no-loss guarantee covers the raw path. In **canonical**
    (cooked) mode an ordinary input byte is routed to the line buffer
    `g_cons.line[]` (`CONS_LINE_MAX`), not the ring — so a single line longer than
    `CONS_LINE_MAX` still **truncates** (the LS-8b bound; the byte is read from the
    FIFO and dropped at the line-buffer-full check, not held by backpressure).
    Extending backpressure to the cooked line buffer is deliberately NOT done: it
    would strand the line's terminating Enter behind a full buffer (the Enter
    could never terminate the line). A 256-char single line is pathological for
    interactive cooked input (login reads short username/passphrase lines); the
    raw-path flood is the real loss vector and is the one #174 closes.
  - **RX resume is gated on the console reader (#174-audit F3).** `uart_rx_pump`
    runs only from `cons_input_read`, so a paused RX resumes only when *some* Proc
    re-enters the console read. A console holder that stops reading (e.g. a
    foreground child that never reads stdin) parks RX paused — input is **buffered
    in the FIFO + host, never lost**, and flows the instant a reader returns. This
    is correct backpressure, not the #172 wedge (the kernel is not stuck; only the
    paused console waits on its consumer).
