---
id: chg-2026-08-16-sched-addrspace-install
type: chg
title: "The one TTBR0 write outside a switch, and a save path that reads the hardware"
date: 2026-08-16
arc: arc-vault
commits: ["99c737b2"]
touched: [sub-kernel-sched, sub-kernel-rendez]
established: []
closed: []
opened: []
depth: rich
created: 2026-08-16
---
Three commits landed in the dispatch file: the address-space extraction, the
execve immediate install, and the recursion-guard repair. The middle one is
worth the most, and for a reason that is not about address spaces.

## A save path that reads the hardware makes an update a critical section

Every translation-root change in this kernel happens at a context switch, where
the incoming value is loaded from the thread's saved context. Exactly one path
needs otherwise: **image replacement swaps a live thread's address space and
then returns straight to user mode through the exception frame.** No switch
occurs, so nothing would ever load the new root, and the return would land the
new image's entry address inside the *old* translation.

So there is a primitive that installs it immediately. Interrupts are masked
across the compose-and-write pair, and the second of the two reasons is the one
I would not have derived:

**The context-save path reads the register back out of the hardware** rather
than trusting the struct it is saving into. So a preemption landing between the
struct write and the register write saves the *stale hardware value* over the
freshly-composed one — and the thread resumes translating through an address
space that is being torn down.

That generalizes cleanly and I have written it into the dossier in the general
form: **wherever a save path reads a register instead of the struct, any
"update the struct, then the register" sequence is a critical section**, because
a preemption inside the window makes the hardware win. The failure is quiet in
the worst way — it looks like a correct install that silently reverts, with no
faulting instruction anywhere near the cause.

## The barrier belongs to the caller, not to the writer

The trailing synchronization barrier reads like ordinary hygiene for the thread
doing the install. It is more than that: **after it, nothing on this processor
can still be walking the old root, which is precisely the precondition the
outgoing address space's teardown depends on.**

So removing it does not merely risk a stale fetch for this thread. It removes
the *licence to free the old tables* — and the code that frees them is
somewhere else entirely, does not mention the barrier, and would keep working
for a long time.

**A barrier can be load-bearing for a caller that does not name it.** Recorded
in the prosecution list in exactly those terms, because the tempting
relaxation ("this processor will just fault harmlessly") is an argument about
the wrong party.

## Depth cannot separate recursion from interleaving, at any threshold

The recursion guard's repair is already recorded in full on the entry surface
from an earlier sweep. What belongs here is *why the reset lives in the
scheduler*, which is the part a reader of the dispatch file needs.

A per-processor counter of kernel synchronous-exception entries goes high for
two unrelated reasons: a genuine runaway (a fault whose handler faults), and
several threads each legitimately *asleep* inside a handler while time-sharing
the processor. **One number, two causes — so no threshold discriminates.**
Raising the cap moves the false trip; lowering it moves the miss.

The second axis is forward progress. A context switch proves the handler chain
is advancing, and a real runaway is synchronous and interrupt-masked and
therefore never reaches the scheduler at all. So the scheduler clears the
counter — **not because the scheduler owns it, but because the scheduler is
where the distinguishing fact exists.**

Written into the prosecution list as a constraint on *moving* it: relocating
the reset to a wake, a tick, or an interrupt return would clear it on paths a
real runaway does reach, and the guard would silently stop discriminating while
still appearing to work.

## The extraction moved a test's meaning without moving its words

The interactive-band promoter gates on "is this a user thread". It used to ask
whether the translation root was non-zero; it now asks whether the address
space pointer is non-null.

Same verdict, different sentence, and the dossier's citation was the old one.
Worth correcting rather than silently updating, because the *reason* the
in-kernel test runner stays out of the band changed: it is not that its root is
zero, it is that it has no address space at all. A reader reasoning from the
stale citation about some future thread with a zeroed root would reach a
conclusion about a field that no longer sits where they think.

## A dossier flagged stale with nothing owed, and why that is a real disposition

The wait/wake dossier shares this file and was flagged for the same churn.
Every hunk landed in the dispatch half — none touched the function set it owns.

I checked by hunk context against that function set rather than by reading the
whole diff for anything familiar, and recorded the check in the dossier so the
next sweep does not repeat it.

**Churn is per FILE; ownership is per SURFACE**, and they diverge for every
file carrying more than one layer. Leaving it flagged forever would be the
worse outcome: a permanently-red entry in a triage list trains the reader to
skip it, which costs exactly the attention the list exists to direct. So the
date moves and the note says what kind of check moved it.
