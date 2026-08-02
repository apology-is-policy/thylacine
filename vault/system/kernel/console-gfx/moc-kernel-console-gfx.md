---
id: moc-kernel-console-gfx
type: moc
title: "The console and its front doors"
parent: moc-kernel
created: 2026-08-02
updated: 2026-08-02
---
The machine's original interface and its trusted path: the UART console with
its line discipline, the `/dev` Dev that makes it a walkable path, and the
gates that keep both doors equivalent.

## The organizing fact

**The console's producer is an interrupt handler that may not do the work, and
its consumer is a trust boundary.** Those two facts together explain the whole
area.

The first produces the *shape*: every action the receive handler wants to take
— wake a poller, post a note, perform the trusted-path transition, wait for
room to echo — is illegal in interrupt context, so each is flagged and relayed
to a manager kthread. The relay occurs four times and is the one part of the
console with a model ([[spec-cons-poll]]).

The second produces the *rules*: three separate console roles that never
substitute for one another, an attention key that is a line condition rather
than a byte, and two front doors that must gate identically ([[inv-i27]]).

## The three roles

| Role | Conveys | Gate lives in |
|---|---|---|
| **attached** | elevation, and minting a console fd by name | [[sub-kernel-devdev]] open |
| **owner** | the target of the interrupt and window-change notes | [[sub-kernel-cons]] deferred posts |
| **renderer** | the output drain and the input feed, nothing more | [[sub-kernel-devdev]] open + every I/O |

Holding one never confers another, and each separation was established by a
specific failure — [[inv-i27]] carries them.

## Children

- [[sub-kernel-cons]] — the console proper: four rings, the line discipline,
  the transmit ring and its writer role, the renderer drain and feed, the
  control grammar, and the deferral machinery behind all of it.
- [[sub-kernel-devdev]] — `/dev`: the leaf table, the trivial Unix furniture,
  and the two-tier trusted-path gate.

## Cross-cutting

- Invariants: [[inv-i27]] (the trusted path — this area is both its enforcement
  sites) and [[inv-i9]] (no lost wake, across the deferral).
- Specs: [[spec-cons-poll]] models the relay. The console's other three
  deferred actions share its structure and are prose plus test.
- Locks: [[lock-cons]] and [[lock-cons-tx]]. Both are leaves, deliberately —
  a leaf is what an interrupt handler can take, and keeping them leaves *is*
  the deferred design.
- Hazards: [[haz-single-waiter-rendez]] — the console has three single-waiter
  Rendez, each sound for a different reason, and each one line of refactoring
  away from an extinction.
- Gates: [[gate-interactive]] drives a real login over a real console;
  [[gate-smp]] is the multi-boot witness for the interrupt-context work.

## Scope note

`/dev`'s trivial leaves (the bit bucket, zero, full, the randomness aliases)
live in this area because they share the one Dev with the console leaves, and
splitting that file's gate story across two areas would fork it. The hardware
device drivers are a separate area and land in `system/kernel/devices/`; the
graphical side — the compositor, its surfaces and its present path — is
userspace and lives under `system/userspace/`. What is *here* is the kernel
console and the namespace door onto it.
