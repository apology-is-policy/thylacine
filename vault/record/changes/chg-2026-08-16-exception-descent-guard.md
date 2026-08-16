---
id: chg-2026-08-16-exception-descent-guard
type: chg
title: "The guard that fabricated a kernel death, and the fourth eret that isn't one"
date: 2026-08-16
arc: arc-vault
commits: ["d6324261"]
touched: [sub-kernel-exception]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
Four commits arrived on this surface since the dossier — two from the
process-creation arc, two from the real-silicon bring-up and its audit. Swept
immediately after [[sub-kernel-boot-entry]], which owns the root of the failure
whose *amplifier* lives here.

## A guard that shipped with a false premise, and the premise was the interesting part

The kernel synchronous handler now counts its own depth per processor and gives
up at three, because a fault whose handler faults on the same bad state recurses
— each iteration building a frame on the same stack, marching downward through
memory until the page tables themselves hold exception frames.

The guard's reasoning was that legitimate depth is **one**: a kernel synchronous
handler runs to completion without yielding.

**It does not.** A kernel-side access to a cold file-backed page blocks in the
filesystem client — a perfectly healthy handler that *sleeps*. Independent
threads time-sharing one processor can each be asleep inside one. Three of them
reach the threshold, and the guard parks a healthy processor and prints a
**fabricated extinction line**: the exact string the entire test harness reads as
"the kernel died".

Under a parallel build. Not an exotic condition — the ordinary load this project
runs constantly.

That is a nastier failure than the one it was written to catch. The bug it
prevents corrupts memory loudly; the bug it introduced **manufactures evidence of
a bug that did not happen**, on the one channel every automated gate trusts. A
guard on the reporting path can lie about the thing it reports.

## The fix is a discriminator, not a bigger number

The scheduler now clears the counter at **every context switch**.

A switch *proves the handler chain is making forward progress*. A genuine runaway
— a fault whose handler faults, synchronously, with interrupts masked — never
reaches the scheduler at all, so its count survives to trip.

**Depth alone conflates recursion with interleaving**: three frames because one
handler re-entered twice, and three frames because three threads each entered
once, are the same integer. Nothing about the count distinguishes them. Adding
"did we yield?" does, and it does so without knowing anything about what the
handlers are doing — which is what makes it robust to the next legitimate
sleeping path nobody has thought of.

Raising the threshold instead would only have moved the load at which the false
report appears. That is the tempting fix and it is the wrong one, because the
quantity was never the problem.

Two further details worth keeping. The reset-on-unwind is a **separate**
mechanism from the reset-at-switch and both are needed — unwinding resets to zero
rather than decrementing, so a handler that migrated mid-flight cannot strand a
foreign processor's increment. And the residual is written down rather than
hidden: a recursive chain that *unmasks* interrupts partway could be
preempt-cleared and evade the count, which is bounded by the terminal path's own
re-entrancy guard beneath this one, and whose failure mode is a spin rather than
corruption.

**A guard with a known hole and an argued containment is worth more than one
whose hole nobody has looked for** — and this one earned that posture by having
its first premise disproved by a reviewer rather than by a crash.

## Two refusals in the giving-up sequence

Neither is obvious and both are right.

It does **not** run the crash dump, because the dump machinery is the most likely
amplifier — the thing you instinctively reach for at the moment of failure is the
thing most likely to fault again on the same bad state. Instead: flush whatever
diagnostics are already staged, with a bounded try-lock so a held lock cannot
hang the report, print one raw banner, park with the corpse intact for an
external autopsy.

And the banner prints at *exactly* the threshold rather than at-or-above, so if
printing the banner itself faults, the next entry parks **silently** instead of
looping through the print. The degradation has stages, which is what you want
from something running after the machine has already failed once.

## The fourth way to EL0, which is deliberately not a fourth `eret`

The fork trampoline lives in the vector file rather than beside the other
trampolines for one reason: from there it can branch to the shared return's own
local label, handing the child to the single audited return-to-user path.

**That is the standing rule satisfied by refusing to create the situation it
governs.** The rule says any hand-rolled return to userspace owes a masking
argument across its link-register window. A new one would have owed that, the
tail-ordering argument, and a review; branching into the existing one owes none
of them.

The precondition that buys it is that the child's frame was built at exactly the
address and layout that path expects — which is why the frame is carved at the
top of the child's own stack rather than anywhere convenient.

## And it sweeps no registers, which is the same rule inverted

The dossier's prosecution said the register sweep before returning to userspace
must stay complete. This path zeroes nothing.

Both are correct, and the reason is worth stating because the rule as written
would condemn it. The other trampolines **construct** an EL0 context out of a
kernel context, so any register left untouched carries kernel residue across the
privilege boundary. This one **restores** a saved EL0 frame copied from the
parent's, so every register already holds a userspace value — there is nothing to
leak, and sweeping would destroy the fork, since the frame pointer and return
address are precisely the state the child must keep.

**The sweep is not the rule. "No kernel state crosses" is the rule**, and the
sweep is how one family of paths achieves it. A prosecution line stated as the
mechanism rather than the property would have flagged a correct path as a
violation — the third time in this sweep that a rule named an implementation
where it meant a property.
