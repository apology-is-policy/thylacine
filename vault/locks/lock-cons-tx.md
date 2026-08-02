---
id: lock-cons-tx
type: lock
title: "g_cons_tx.lock — the console transmit ring"
kind: spin (irqsave)
guards: "the transmit ring and its indices, the armed flag, the writer-role flag, the drop and room-wait counters, and registration on the role wait list"
orders-before: [lock-poll-list]
created: 2026-08-02
updated: 2026-08-02
---
## Discipline

Irqsave: the UART transmit interrupt drains the ring under it.

**It nests only the UART interrupt-mask leaf** (via the enable/disable helper,
which takes nothing further), so it is a leaf with respect to every wait lock.
Every wake — the role release and the room-available signal — happens **after**
releasing it.

**Deciding the ring state and the interrupt state happen in the same critical
section.** That is what makes *a non-empty ring left with transmit interrupts
off* — the silent console wedge — unrepresentable rather than merely avoided.
Any change that separates the drain from the re-evaluation reopens it.

## The role is not this lock

A console write larger than the ring must sleep for room, which drops this
lock — so the lock alone can never span a write call, and write atomicity needs
a separate mechanism. That is the **writer role**: a flag under this lock, with
contenders parked on a wait list, in the shape already audited for the service
connection channels. A write holds the role for its whole duration; the ring
lock is taken and dropped many times inside it.

## The single-waiter room Rendez

The room-available Rendez is **single-waiter by construction**, and the
construction is the role: only the role holder pushes-with-wait, and the role
is exclusive. That is the entire soundness argument — see
[[haz-single-waiter-rendez]] for why it matters, since a second sleeper on a
single-waiter Rendez is an extinction.

**If a second waiter is ever introduced this must become a wait list.**

## Two producers, opposite contracts

The asymmetry is load-bearing and a change that blurs it is a bug:

- The **write path** runs in process context and *may* sleep for room, and does.
- **Echo** runs in interrupt context and must *never* sleep: it pushes
  non-blocking and drops on a full ring.

The kernel diagnostic emitters share the echo contract for the same reason —
they are legal under any lock ordered above this one, which is what makes them
usable from interrupt context and from under the process-table lock.

## Cross-lock reads

The count and the role flag are written under this lock and read locklessly in
the sleep conditions and the test hooks, so both are relaxed atomics — the same
discipline and the same rationale as [[lock-cons]]: well-definedness, not
ordering.

## Prosecution

- The drain and the interrupt re-evaluation must stay in one critical section.
- Every wake stays outside the lock.
- The room Rendez stays single-waiter, or becomes a list.
- The room wait must stay deadlined. A stalled consumer stops the transmit
  interrupt entirely, and an undeadlined wait would wedge the writer — the
  console is deliberately bounded-and-lossy instead.
