---
id: lock-cons
type: lock
title: "g_cons.lock — console input state"
kind: spin (irqsave)
guards: "the receive ring and its indices, the deferred-action flags, the termios word, the canonical line buffer, the window size, and registration on the console's poll-hook list"
orders-before: [lock-poll-list]
created: 2026-08-02
updated: 2026-08-02
---
## Discipline

Irqsave, because its primary producer is the UART receive interrupt.

**It is a leaf, and keeping it one is the console's entire deferred-work
design.** Two things may not happen under it, and both are the reason the
console has a manager kthread at all:

- `poll_waiter_list_wake` — [[lock-poll-list]] is non-irqsave and nests a wake
  inside itself. *Registration* under this lock is fine and required (it is the
  register-then-observe atom); the *walk* is not.
- Anything reaching [[lock-proc-table]] — the interrupt-note post and the
  attention-key transition both do.

So the interrupt path only ever mutates state under this lock and calls
`wakeup`, the one wake primitive that is interrupt-safe. Everything else is
flagged and relayed ([[spec-cons-poll]]).

The window-size verb obeys the same rule from the other direction: it applies
under this lock and posts the change note **after** releasing, so no
`g_cons.lock → g_proc_table_lock` edge exists in either direction.

**Echo is emitted with the lock released.** The cooking stages at most a few
bytes into a caller-stack array; those go to the transmit path afterwards, so
the console lock is never held across a UART wait.

## Position in the chain

`g_uart_rx_lock` → **g_cons.lock** → [[lock-poll-list]] (register only).

The reader closes the cycle by construction: it releases this lock *before*
calling the receive pump, so there is no `g_cons.lock → g_uart_rx_lock` edge
and the order stays acyclic. That release is not optional — the pump re-enters
the cooking path, which takes this lock.

## Cross-lock reads

The ring count and the deferred flags are mutated only under this lock but read
**locklessly inside sleep conditions**, which run under a Rendez lock instead.
A plain cross-lock read of a field written under another lock is a data race,
so every such field is a relaxed atomic.

The atomics are for well-definedness, **not** for the wake guarantee. The
no-lost-wakeup property comes from the Rendez lock — the producer's `wakeup`
acquires the same lock the sleeper's cond-check and sleep transition hold — so
a stale relaxed read costs one extra sleep/recheck cycle and never a lost wake.
Do not read the atomics as the ordering mechanism.

## Siblings

The drain lock (`g_cons_drain.lock`) is the same discipline applied to the
renderer's output ring: irqsave, leaf, flag-and-relay, with the identical
lockless-cond atomics. The transmit ring's lock is [[lock-cons-tx]], which has
a different shape because it is *waited on*.

## Prosecution

- A new interrupt-context readiness source must relay, never widen
  [[lock-poll-list]] to irqsave for a console-only need.
- Nothing that can reach [[lock-proc-table]] may be called under this lock.
- A new field read from a sleep condition must become a relaxed atomic, or it
  is a data race the sanitizer will not see (the reads are in a different
  translation path than the writes).
