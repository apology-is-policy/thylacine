---
id: chg-2026-08-16-boot-entry-trampoline
type: chg
title: "The guard held because the emulator supplied the value it checked for"
date: 2026-08-16
arc: arc-vault
commits: []
touched: [sub-kernel-boot-entry]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
One commit since this dossier, and it is the first multi-processor boot on real
silicon — which froze, and unpacked into four stacked defects that no emulated
substrate could have shown.

The dossier already described the primary path's fix for this exact hazard, in
detail, correctly. It never asked whether the *other* entry path in the same
file did the same thing.

## The root, and why the dossier missed it

Three per-CPU registers have architecturally unknown values at reset, so
clearing the zero-initialized data section does not cover them. The boot CPU's
stub zeroes all three and carries a comment explaining why.

The secondary trampoline — same file, a few hundred lines down, entered by every
other CPU — zeroed none of them. The argument applied verbatim; the code was
never copied across.

**The comment on the primary is part of the cause.** A site that explains itself
reads as considered, and considered reads as handled — so the question "does the
sibling path do this too?" is exactly the question a well-documented fix
suppresses. This is the shape already recorded elsewhere in this vault as *the
fix that exists on site N stops you asking about site N+1*, and it is worse when
site N is eloquent.

## Why a decade of testing could not see it

The downstream code has a null check on the thread pointer, and that check had
been passing forever.

It was passing because every emulator the project runs on resets those registers
to zero. The check tested for a value **the platform happened to supply**, not
one the code established. Under a hypervisor that deliberately poisons
unknown-reset registers with a recognizable non-zero pattern, the pointer came
back non-null, the first dereference walked into the poison, and the machine
died.

**A guard that passes because the environment supplies the value it checks for
is indistinguishable from a guard that works.** No test on the usual substrates
can separate them, because the substrate *is* the thing being relied on. That is
a strictly harder case than an assertion that cannot observe its failure — here
the assertion observes correctly and the observed value is a coincidence.

Concrete argument for keeping real hardware in the loop, which is otherwise the
kind of policy that gets justified in the abstract and skipped in practice.

## The amplifier is the part worth remembering

The bad dereference faulted. The fault handler dereferenced the same poison
again. That fault re-entered the handler, which re-entered the fault.

Each iteration built an exception frame on the same stack, so the recursion
marched *downward* through mapped memory, writing frames across physical RAM
until page tables themselves held exception frames and every processor's vector
fetch died. A single bad pointer became whole-memory corruption because the
reporting path shared the assumption that had just been violated.

The bound is now a depth counter that gives up at three, prints one raw banner
naming the killing frame, and parks with the evidence intact — plus a
re-entrancy guard on the terminal path itself. Recorded in
[[sub-kernel-exception]], not here; this dossier owns only the root.

Two details in that repair are worth keeping. The depth counter **resets to zero
on unwind rather than decrementing**, because a decrement would strand the
increment of a thread that migrated. And the terminal guard degrades in stages —
second entry prints without the expensive dump, third parks silently — rather
than choosing between "always print" and "never print".

## What the failure looked like from outside: nothing

It froze. It did not report.

The bring-up wait had a timeout, and the timeout counted a tick variable advanced
only by the boot processor's timer interrupt — which, in that state, was not
advancing. The timeout **had never fired once in the project's history**, because
secondaries had always arrived first, so its own dependency had never been
exercised.

**A timeout that has never fired is untested code in the position of last
resort.** It now rides the architectural counter, and a partial bring-up is
treated as fatal rather than continued — on the stated grounds that a machine
running on fewer processors than were asked for is a lie waiting to be verified
around. That reasoning is the project's "never verify around an instability"
rule turned into a mechanism, which is the only form of it that survives.

Both live in [[sub-kernel-sched-smp]], which had already recorded them. Checked
rather than assumed: the two halves of this failure are split across dossiers
with no gap between them, and the boot-entry side now carries a pointer so the
trail does not stop at the root.

## A caveat that came true

The bring-up notes had already recorded that the trampoline's writes, executing
with the memory management unit off, are device-typed and go straight to the
point of coherency — while the processor reading them uses a cacheable mapping.
Written as a caveat before it had caused anything.

It came due here, and the handshake is now an explicit protocol with the flag
isolated to its own cache line. Worth noting as the case where the *documentation
predicted the bug*: the note existed, was correct, and did not prevent it. What
it did was make the diagnosis fast once the symptom appeared, which is a real
return on writing caveats down even when they read as speculative.

The same hazard bounds what may be *called* across the point where the memory
system is enabled — a frame pushed as a device write and popped as a cacheable
read is silent corruption — so a helper used there must be genuinely leaf, and
that was confirmed against the generated code rather than inferred from the
source, since leaf-ness is a property of what the compiler emitted.
