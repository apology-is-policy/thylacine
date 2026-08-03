---
id: moc-kernel-entry
type: moc
title: "The EL0 boundary — exception entry, return tails, and deliberate crossings"
parent: moc-kernel
created: 2026-08-02
updated: 2026-08-03
---
Every transition between userspace and the kernel: the vector table and its
save/restore macros, the three paths that `eret` to EL0, the return tails where
the kernel acts on a thread before letting it run again, and the fixup table
that lets kernel code touch a user address without dying — plus the crash dump
the entry wrappers feed, which is what remains when a crossing goes wrong.

## The organizing fact

**The kernel does its dangerous work at safe points, and this area defines
where the safe points are.**

A privilege boundary is not one instruction, it is a set of *moments*. Three of
them live here:

| Moment | What may happen there | Why there |
|---|---|---|
| entry | save the interrupted state | the frame must land on the thread's own kernel stack ([[inv-i21]]) |
| return tail | preempt, die, deliver a note, park for a debugger | the only place with a clean frame, no locks held, and a thread about to become interruptible again |
| deliberate crossing | read or write a user VA from EL1 | a designated instruction whose fault is recoverable rather than fatal |

This is the same shape as the console's deferral to its manager kthread
([[moc-kernel-console-gfx]]) — work that cannot be done where it is discovered
gets carried to a place where it can. There the carrier is a kthread; here it
is the return tail.

**And a boundary has two descriptions, not one.** The moments above say *where*
a crossing may happen. The syscall ABI ([[sub-kernel-syscall-abi]]) says *what*
crosses: the number space, the register convention, the argument records, and
the bounds each is checked against. They are separable — the entry mechanism
would be unchanged by a completely different set of syscalls — and they fail
differently. A mechanism defect is a fault; an ABI defect is a mismatch between
two programs that each believe they are correct, which is why that half is three
files in three languages with nothing checking that they agree.

Entry does one more thing, for the case where none of this works: it publishes
the saved frame to a per-CPU slot so that a kernel death still has registers and
a stack to report ([[sub-kernel-halls]]). That slot is the only piece of entry
state deliberately allowed to go stale — a handler that never returns, or one
that resumes on a different CPU, leaves it wrong — and the dump defends itself
with a plausibility check rather than the wrappers defending the slot.

## The tails are the substance

Four actions want to run before a thread re-enters EL0: the preemption check,
the group-terminate die-check, note delivery, and the debugger stop-check. They
are ordered so that **death wins over a stop**, and so that a Proc
group-terminated *during* the preempt is still caught before any EL0
instruction runs.

Three of the four run on both EL0 return paths. The fourth — note delivery —
runs only on the return from a syscall or a fault, which is
[[seam-el0-irq-tail-no-notes]] and the finding this area was swept on.

## Children

- [[sub-kernel-exception]] — the vector table, the two EL0 return tails, the
  three `eret`-to-EL0 paths and the window rule they all obey, and the
  translation from an EL0 fault to a per-Proc termination.
- [[sub-kernel-uaccess]] — the fixup table: how a kernel-mode fault on a user
  address becomes either a demand-page-and-retry or a clean `-1`, and never an
  extinction.
- [[sub-kernel-halls]] — the crash dump: what the entry wrappers hand it, how it
  survives its own faults, and the live-thread backtrace that reuses half of it
  under an opposite safety argument.
- [[sub-kernel-syscall-abi]] — the contract: 100 live numbers in a
  never-reused space, thirteen offset-pinned argument records, the bounds
  registry, and the two userspace mirrors that say "MUST mirror" twenty-two
  times between them with no mechanism behind the word.
- [[sub-kernel-syscall-dispatch]] — the marshalling: the switch, the
  user-pointer validator, the two staging tiers and their budget, and the
  handler/inner split that decides where an authority gate is allowed to live.
  Scoped to the mechanism around the handlers; each handler's own policy stays
  with its subsystem.

## Cross-cutting

- Invariants: [[inv-i13]] (kernel/user isolation) is enforced here on its
  *crossing* half — the fixup table and the register sweep before each `eret`.
  Its address-space half lives in the MMU, which is not yet swept. [[inv-i21]]
  (uniform EL1h) becomes mechanical here: the two vector slots for
  "current EL with `SP_EL0`" are wired to the unexpected-vector diagnostic, so
  the model's violation is a loud extinction rather than a silent wrong-stack
  write.
- The EL0 fault path hands off to the page-fault handler, which belongs to
  memory rather than to this area — the audit-trigger table draws the same
  line, listing "exception entry" and "page fault + COW + W^X" as separate
  surfaces.
- Scope: the boot-time installation of the vector base, and the alternatives
  patcher that rewrites atomics before secondaries start, belong to boot.
