---
id: inv-i13
type: inv
title: "I-13 — kernel and userspace are separated, and every crossing is deliberate"
number: I-13
guards: [sub-kernel-uaccess, sub-kernel-exception]
validated-by: [prose, gate-smp]
strength: prose
created: 2026-08-02
updated: 2026-08-02
---
## Statement

Userspace cannot reach kernel memory, and the kernel reaches user memory only
at a fixed, enumerated set of places.

The two halves are usually stated as one line about the split translation
tables, which understates it. A split address space says *userspace cannot
reach in*. It says nothing about the kernel reaching out — and the kernel does
reach out, constantly, because that is how a syscall gets its arguments. So the
invariant has a second half:

1. **Separation.** The kernel's mappings live in the upper half and a Proc's in
   the lower, in separate translation-table roots, and a Proc's root is swapped
   on every context switch.
2. **Deliberate crossing.** Kernel code touches a user address only through a
   designated instruction whose fault is recoverable. Every *other* kernel-mode
   fault on a user address is what it looks like — a corrupted pointer — and is
   fatal.

The second half is the one that is easy to lose, and the one this vault has a
swept home for.

## Enforcement

**The crossing set is enumerated, not conventional.** Each kernel-mode user
access is an instruction with an entry in a link-time table. The table *is* the
enumeration: an access without an entry is not a crossing, it is a bug, and it
extincts. See [[sub-kernel-uaccess]].

**The recognition is a conjunction of three conditions** — the fault came from
the kernel, the address is in the user half, and the instruction is in the
table. Dropping any one of the three converts real memory corruption into a
silently absorbed error return, which is why the check is prosecuted as a unit
rather than as three independent guards.

**Nothing crosses in registers.** Both hand-rolled paths into userspace zero
every general-purpose register before the `eret` — all of them on the exec
path, all but the argument register on the thread-spawn path. See
[[sub-kernel-exception]].

**The bound is pinned, not repeated.** The user-half constant used by the fault
recognizer is tied by compile-time assertion to the one the memory layer uses to
reject mappings. Drift between them would mean a kernel fault on an address the
mapping layer would have refused could still be treated as a legitimate
crossing.

**Failure is closed.** A crossing that cannot be satisfied returns an error to
the caller, which reports a bad address. It never silently reads zero, never
half-succeeds without saying so, and never extincts.

## Validation

Prose, plus the unit tests that assert the fixup table is well-formed, that the
lookup resolves the exact designated instructions and rejects everything else,
and that a copy to an unmapped address faults on **every one** of its three arms
rather than just the first — the last being the one that would catch a new arm
added without its table entry.

**blind-to:** the separation half. Its enforcement is in the memory layer — the
translation-table split, the root swap on context switch, and the address-space
identifier that keeps stale translations from leaking between Procs — and that
area is not yet swept, so this note currently carries only the crossing half
with a real guard behind it. Also blind to the alignment case: an unaligned
kernel access to a user address is outside the recoverable set and extincts, so
the callers' own alignment gates are load-bearing for this invariant without
being described by it.
