---
id: inv-i26
type: inv
title: "I-26 — cross-process control is explicitly two-axis"
number: I-26
guards: [sub-kernel-devproc]
validated-by: [prose, gate-smp]
strength: prose
created: 2026-08-02
updated: 2026-08-02
---
## Statement

A Proc may terminate or job-stop another Proc only by one of exactly two
axes: it is the target's **owner** (same principal), or it holds a capability
that names process control — the unified host-owner authority, or the
cross-identity kill capability.

Nothing else qualifies. In particular **no identity short-circuits** it: there
is no principal, not even the system principal, that may kill by virtue of who
it is. That is [[inv-i22]] applied to process control, and it is the reason the
gate is computed directly rather than through the filesystem permission check.

## Enforcement

`kernel/devproc.c`, at the write site of `/proc/<pid>/ctl` — the only place in
the tree where one Proc can terminate another. Containment is namespace
visibility ([[inv-i1]]): a Proc that cannot name `/proc` cannot reach the gate at
all.

Four things about the shape are decisions rather than incidentals:

- **The gate is at the write, not at the open.** The shared open chokepoint
  hard-rejects before the Dev's own open runs, so the capability axis could not
  live there. The consequence is that the Dev sets `perm_enforced` false and the
  `0600` mode on `ctl` is documentation of the owner axis, not its enforcement.
- **The filesystem admin capability is deliberately NOT an axis.** A holder of
  the generic rwx override cannot kill, mirroring Linux's split between it and
  the kill capability — fs-admin stays orthogonal to process control. The code
  says so at the gate, and the same refusal is repeated at the debug gate
  ([[inv-i39]]).
- **The owner axis is expressible as ownership, not as parenthood.** Killing your
  own child is permitted because you own it, not because you spawned it, so the
  rule needs no separate parent case.
- **The kernel process is refused before the axes are consulted**, and so is any
  target that is not alive. A capability holder cannot kill the kernel.

`suspend`/`resume` — the job-control stop — ride the **same** gate, on the
argument that stopping is strictly weaker than the killing it already permits.
So the invariant covers them without widening: no new authority, no new
invariant.

The verbs dispatch uniformly through the group-terminate primitive rather than
posting a note, because after the universal death-wake landed that is the only
termination whose wake is total — a single-threaded target blocked in a
non-notes sleep would not wake on a bare note post ([[inv-i24]]).

## Validation

Prose plus the predicate's own unit test, which drives it with synthetic
caller/target pairs — necessary because the in-kernel test runner holds every
capability, so the *deny* leg is otherwise unreachable and a regression that
dropped the gate would leave every test green. The dispatch tests then exercise
the verb path end to end.

**blind-to:** there is no model. The authority decision is a pure predicate over
two immutable fields and one atomically-read capability word, which is why prose
suffices; what a model would add is the *interleaving* of a kill against a
concurrent exit or reap, and that is [[inv-i24]]'s territory and
[[spec-death-wake]]'s. The composition of a job stop with a debugger's stop is
[[spec-pty-stop]]'s.
