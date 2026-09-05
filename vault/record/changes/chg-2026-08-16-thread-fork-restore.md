---
id: chg-2026-08-16-thread-fork-restore
type: chg
title: "The constructor that restores a frame, and a number my own query hid"
date: 2026-08-16
arc: arc-vault
commits: ["b87de478"]
touched: [sub-kernel-thread]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
The LINEAGE arc gave the Thread a fifth creation shape, and the sweep that
recorded it found a stale figure that a different question would never have
reached.

## Every other constructor composes; this one reproduces

The existing four all *assemble* a starting world: choose a trampoline, load the
callee-saved slots with arguments, let the trampoline build a context. The
forked one runs backwards — it takes the parent's live exception frame and
reproduces it, because the child is not starting anything. It is continuing the
parent's C frame, at the same instruction, with the same condition flags.

The decision is a **pure function**, deliberately split out so a test can reach
it with no process in existence: the source frame with exactly two edits — first
argument register zeroed, which is the whole of "this call returns twice", and
the stack pointer replaced, which is mandatory rather than cosmetic since two
processes sharing an address space must not share a stack.

**The omissions are the specification.** The return address copied verbatim *is*
"resumes at the same instruction". The saved process state copied verbatim
carries the parent's condition flags — live if a conditional follows the call —
and cannot be forged from user mode because the hardware wrote it on exception
entry. Writing the copy field-by-field instead of as an assignment makes those
claims visible where a single `=` would hide them, and separately avoids emitting
a memory-copy call the freestanding kernel does not link.

## Two absences, and the second one inverts the first

The frame is carved at **the exact address the exception entry path would have
chosen**, which is what lets the trampoline hand the child to the shared, audited
return-to-user path rather than open-coding a second one. Reusing that path is
worth the constraint; the address is not arbitrary and the alignment is asserted
rather than assumed, because a misaligned stack pointer at the return faults with
the frame half-restored.

And the child's trampoline has **no register-scrubbing sweep**, where the
ordinary user-thread trampoline does. That is not an omission — it is the
inversion that makes it correct. A fresh thread must not inherit kernel residue;
a forked child is resuming its parent's userspace frame, so scrubbing would break
the fork rather than harden it.

Two trampolines, opposite dispositions on the same question, each right locally.
Another entry in the growing set of *pairs that look inconsistent and must stay
that way* — with the two owner pointers in [[sub-kernel-loom]] and the dot gates
in [[sub-kernel-stalk]].

## The stale state that looks like current state

Floating-point is inherited from the **live registers**, not from the caller's
saved context — because the caller is running, so its saved context holds its
last switch-out values.

This is the same trap as the note-delivery save in [[sub-kernel-notes]]: a
structure named for a thread's state is only current for a thread that is *not
currently running*, and both of these paths are exactly the case where it isn't.
Two subsystems, same misreading available, both avoided — and in both the
correct source is the hardware rather than the bookkeeping.

## The number, and why the obvious query hid it

The dossier said the descriptor was 1232 bytes. It is 1760.

It was **correct when written**. The 528-byte growth is the floating-point save
area for note delivery, authored on another branch on 31 July — the day *before*
the dossier was written — and it did not arrive here until a merge on 5 August.

That ordering is the whole point. My first move this sweep was a log query
bounded by the dossier's date, and it returned exactly one commit: the fork work.
The commit that changed the size was **authored before the dossier and arrived
after it**, so a date-bounded query on author time cannot see it. I found the
discrepancy only because I chose to verify a figure I could have carried forward
unchanged.

The registrar's staleness check dates a change by **when it arrived on this
branch**, which is why the dossier was flagged at all. That policy has been
recorded as a lesson for a while; this is the first time it has been observed
paying, and it paid against my own habits rather than someone else's.

**A commit's date is not when it became true where you are standing.** The
corollary I had not drawn until now: *the tool knew, and my ad-hoc query did
not.* Reaching past an instrument that encodes a rule, to a quicker command that
does not, forfeits the rule silently.

## A third header count

"Four creation shapes" over a five-row table. Third instance this sweep, after
the notes dossier's "four families" over five rows and the fault dossier's
"five" over six.

All three survived for the same reason: **no argument rested on them**, so
nothing downstream contradicted the count. They are not errors of reasoning —
they are what happens when a table grows and its sentence does not, and the only
defence is re-reading the sentence beside the table rather than either one alone.
