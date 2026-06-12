# 111 — The console device (`/dev/cons`, `kernel/cons.c`)

**Status:** as-built through **LS-8a** (pollable cons + the deferred poll-wake).
The termios line discipline (`/dev/consctl`, the cooked/raw cooking) is **LS-8b**
(in progress; this doc's "Line discipline" section is a forward pointer until it
lands). The PTY master/slave pair (`/dev/ptmx` + `/dev/pts`) is **Phase 8**
(I-20).

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

## Line discipline (termios) — LS-8b (forward pointer)

The cooked/raw line discipline — `ICANON` (line buffering + erase), `ECHO`,
`ISIG` (Ctrl-C → the `interrupt` note vs a `0x03` byte), `ICRNL`, `ONLCR`, the
five independent flags set via stty-style `/dev/consctl` writes — lands at
**LS-8b** (ARCH §23.5.1). Today (`devdev.c`) `/dev/consctl` is console-attach-
gated but modeless: read → 0, write → -1.

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
- (A-4c) `cons.blocking_read_wakeup`, `cons.ctrlc_consumed`,
  `cons.break_sets_sak`, `cons.sak_via_console_mgr`, the SAK/owner role-split set.

## Error paths

- `cons_input_read`: `-1` on NULL buf / `n < 0` / a second concurrent reader
  (single-reader guard); `0` on `n == 0` or a death-interrupt with nothing
  buffered.
- `cons_poll`: never errors (returns the ready revents; `0` if neither requested
  event is ready). `devdev_poll` returns `POLLNVAL` for a non-console-attached
  caller of `/dev/cons` / `/dev/consctl` (the I-27 gate, like read/write).

## Known caveats / footguns

- **POLLOUT is always ready** (the UART never blocks). A poller that requests
  `POLLIN | POLLOUT` on the console will **always** see POLLOUT and never wait for
  input — so a consumer waiting for input must poll for **POLLIN only** (LS-8c's
  shell loop does). This is correct POSIX (an always-writable fd).
- **`devcons` (the syscall fd) poll/read are gated at `SYS_CONSOLE_OPEN`, not
  re-checked** — consistent with `devcons_read`. The O_PATH bypass that motivated
  re-gating the `devdev` path (#57b/#81) does not reach `devcons` (it is minted
  only by the gated syscall, never walked in the namespace).
- The single-console termios state (LS-8b) will be **global** to the one v1.0
  console; per-fd termios needs `/dev/pts` (Phase 8).
