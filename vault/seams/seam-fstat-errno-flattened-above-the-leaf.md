---
id: seam-fstat-errno-flattened-above-the-leaf
type: seam
title: "A comment claims two callers propagate; one of them is the one still owed"
status: open
surface: sub-kernel-ninep-dev9p
opened-by: chg-2026-08-16-dev9p-errno-class
tracker: "unfiled -- yip to main 2026-08-16"
created: 2026-08-16
updated: 2026-08-16
---
## Owed

Nothing behavioural. **Two false sentences in one comment**, sitting on the exact
path the error-rollout's own staging document lists as still owed — so the reader
most likely to arrive here is the one scheduled to fix it, and the comment tells
them it is already done.

The comment sits on the native-stat arm of the 9P device, above the call that
fetches attributes from the server. It makes two claims.

**Claim one: the collision value cannot arrive here.** It says the block-integrity
invariant bounds a server's error code into a window that excludes the generic
failure sentinel, so this path can never see it.

It can. The client's error mapper rejects exactly two things — a zero code, and
anything above the window's top — and passes everything else through by value.
The bottom of the range is not excluded. A server answering the permission error
whose numeric value collides with the generic sentinel therefore arrives here
intact, which is the whole reason the sibling name-op path grew a folding helper
in the same rollout.

The mapper's own comment names the window one value narrower than the clamp it
implements, which is probably where the belief came from.

**Claim two: both named callers propagate the value.** It names the resolver and
the stat syscall.

The resolver does — it has a dedicated converter for precisely this return, and
that converter **guards explicitly against the value claim one says cannot
arrive**. The syscall does not: its inner helper collapses every non-zero return
to the flat sentinel before it reaches userspace.

## What closes it

Correct the comment to say what is true: the sentinel value *can* arrive, the
resolver consumes the precise code, and the stat syscall's own propagation is a
separate scheduled item that has not landed.

Whether to *land* that item is a different decision and not this seam's business.
The seam is only that the comment currently asserts it is already done.

**Not a vault edit.** The file is on the implementation branch.

## Risk while open

**The downstream guard is the proof the upstream assertion is wrong.** The
resolver's converter defends against exactly the value the leaf's comment says
cannot occur — so the two frames disagree in writing about whether a case is
reachable, and the one that handles it is the one that says it happens. A reader
who trusts the leaf has been told the guard below it is dead code.

The scheduled harm is narrower and more likely: the staging document names this
syscall in the still-owed half. Someone picking that item up reads this comment,
sees both callers described as propagating, and either declares the work done or
goes looking for the gap somewhere else.

**A half-true claim is worse than a false one here**, because the true half
(the resolver really does propagate) is verifiable in seconds and reads as
confirmation of the whole sentence.

## Why this was not caught earlier

The rollout that would have caught it *did* run over this file — it is the change
that added the folding helper next door, and its own commit message enumerates
which paths it left flat. But it enumerated them by **handler**, and this is a
claim in a **leaf comment about** a handler. The list was of things to change,
not of things that describe what changed.

Nothing was skipped by that rollout. The comment simply was not in the category
it was sweeping — the same shape as a rule stated over an enumerated part reading
as protecting the whole ([[seam-scripture-invariant-mirror-drift]] recorded the
invariant-table instance of it).
