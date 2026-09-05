---
id: inv-i39
type: inv
title: "I-39 — debug authority is bounded, stopped-only, and never strands its quarry"
number: I-39
guards: [sub-kernel-devproc, sub-kernel-proc, sub-kernel-hwdebug]
validated-by: [spec-debug-stop, spec-debug-step, prose, gate-smp]
strength: spec
created: 2026-08-02
updated: 2026-08-03
---
## Statement

Debugging another Proc requires **naming it** (it is reachable in the
debugger's namespace) **and** passing a two-axis gate: the debugger is the
target's owner, or holds the host-owner or the cross-identity debug capability.

Given authority, three further clauses bound what it may do:

- **Stopped-only.** Reading or writing a target's user memory and registers, and
  all execution control, require the target to be *fully stopped*. A running
  target is not inspectable.
- **No escape.** No debug operation writes executable text, leaves the target's
  address space, or grants privilege — breakpoints are hardware registers, and
  a register write can never set the saved program status.
- **No strand.** Detach, closing the control fd, or the debugger's death always
  releases an **attached** target. A debugger-**launched** target instead dies
  with its launcher, rather than orphaning to init to run forever.

Two targets are refused outright, before the axes: the kernel process, and any
Proc marked no-trace. No capability reaches either.

## Enforcement

`kernel/devproc.c` is the authority surface; `kernel/proc.c` owns the stop
machinery it drives ([[sub-kernel-proc]]).

**The gate.** Computed directly, like [[inv-i26]]'s, and with the same deliberate
omission: the filesystem admin capability is not a debug axis. The host-owner
capability *is* one, on the argument that it already kills and chowns any target
and debugging is strictly less invasive than killing. Slot ownership is then a
**stricter** gate layered on top — only the attached debugger drives run state, so
a stranger who could attach but has not cannot stop a target another debugger
owns.

**Stopped-only is a conjunction**, and each conjunct closed a real failure: the
target is alive, a debug stop is pending, no group termination is published, and
every non-exiting Thread is parked on its own debug rendez **and** off-CPU. The
off-CPU spin is required because a Thread mid-context-switch still reads as
running while its saved frame is being written; the parked check runs under each
peer's own wait lock, the same lock the park's register-then-observe takes, so it
can never confirm a Thread about to proceed to EL0; and the termination check is
what keeps a dying target — whose Threads go exiting, which the parked scan skips,
and then write their context outside the table lock — from reading as stopped.

**Death wins, everywhere.** The EL0-return tail checks death *before* the stop, so
a death unwinds a Thread while a stop parks and re-parks it. The one place this
had to be re-established rather than inherited is the elected 9P reader: its
receive is frame-atomic, so a stop unwinds it only at a frame boundary and blocks
through mid-frame — an unwind with a partial frame consumed would desync the
shared stream for every other Proc on that client.

**The privilege guard** is that an edited register frame writes the general
registers, the stack pointer and the resume address, and never the saved program
status — an arbitrary one would return the target to kernel privilege. It holds
at any write offset, because the write path rebuilds the whole frame and overlays
the caller's slice before applying.

**No-escape is literal, not argued, on the hardware leg** ([[sub-kernel-hwdebug]]).
A breakpoint is a debug register, so arming one never writes the target's text and
cannot violate write-xor-execute — there is no patched instruction to get wrong.
That leg carries its own share of the gate too: a debug exception fires
asynchronously with respect to a detach, so all three exception arms deliver the
stop through the attach-gated path under the process table lock rather than
setting the flag directly. That is a fix, not the original design — the ungated
version parked targets whose debugger had already left, which is the model's
`StopImpliesOwned`.

**No strand** is the control-fd close hook, which releases the slot on every path
including the debugger's death (its handles close at exit). Its soundness rests on
something external: the Spoor close hook runs exactly once, on the last reference
drop, *before* the storage is freed — which is what lets the attach slot be a bare
pointer compared only by identity, safe against pid reuse and never dangling.

**One relaxation, deliberate.** The settled-thread kernel backtrace drops the
stopped-only requirement: any authorized caller may read a Thread's kernel stack
whenever it is off-CPU. That is what lets it show a Thread blocked *deep* inside a
syscall, which a debug stop — parking only at the EL0-return tail — structurally
cannot. It controls no execution and is bounded to the Thread's own stack. Its
output then splits on capability, because raw kernel addresses disclose the
address-space randomization offset: the capability tier sees them, the owner tier
gets the link-relative symbolic form.

That split closes the disclosure and opens a smaller honesty gap, because the
symbol lookup it leans on has no upper bound: every address above the last text
symbol resolves to that symbol. The capability tier can see this — it has the
raw address beside the name — and the owner tier, which has only the name,
cannot. See [[sub-kernel-halls]].

## Validation

[[spec-debug-stop]], written **model-first** and gating every change to the
protocol: `NoLostStop`, `NoEL0AfterStopped`, `ExactlyOnceResume` and
`StopImpliesOwned` as safety, `EventuallyResumed` as the no-strand witness,
`EventuallyLaunchedDies` for the die-with-launcher refinement, and
`DeathWinsOverStop` for the tail ordering — each with a buggy cfg that violates
exactly it.

[[spec-pty-stop]] carries the composition with job control's stop, the second
owner of the same park.

**blind-to:** the model proves the protocol, not the data path. The register
privilege guard, the address-space confinement of cross-Proc access, the
capability split on kernel addresses, and the hardware breakpoint machinery all
rest on prose plus the focused audits. The relaxed settled-thread read is a gate
*condition* rather than a state-machine transition, so it left the model
unperturbed by construction. The full multi-Proc interleaving — a stop landing
while a peer holds the shared filesystem reader role and a third Proc is being
reaped — remains the same cross-Proc SMP harness the 9P client has owed since its
own restoration.
