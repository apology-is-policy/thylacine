---
id: chg-2026-08-16-territory-verbatim-join
type: chg
title: "The redundant safety net was redundant because of the bug"
date: 2026-08-16
arc: arc-vault
commits: []
touched: [sub-kernel-territory]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
Two commits arrived here after the dossier, both authored *before* it and
carried in by a merge four days later. Third time in this sweep that an
author-date query would have returned nothing while the arrival-dated one found
real work. I stopped treating that as a coincidence several surfaces ago.

## The pathname family reaches its last layer

This is the kernel's cwd join — the first of the three layers the POSIX
pathname-form work repaired, and the last one I have swept. Having all three
recorded, the corrected principle holds: **never DECIDE, not never transform.**

The join used to canonicalize as it joined. A lexical `..` pops a component
*without proving it exists*, so a cwd-relative path traversing a directory that
was not there resolved successfully and returned a working descriptor — and
change-directory validated directory-ness against the parent it had already
massaged the path into, **checking the wrong object entirely**.

The absolute spelling of the same path answered correctly. Two code paths for
one question, disagreeing, and the shorter one was wrong.

The repair splits the two jobs rather than adding a gate: the join emits the
input **verbatim**, so dots and trailing separators reach the resolver and meet
exactly the gates the absolute spelling meets; the canonicalizer keeps one
caller, computing the string change-directory stores. The entry point was
renamed from *resolve* to *join*, because **the old name described the bug** —
it promised to resolve dots, which is precisely what it must not do.

Change-directory needs both, and the ordering is careful: join verbatim, resolve
that, then canonicalize **the already-resolved join** with no cwd seed — so the
stored string derives from the path just validated, and a peer thread's
concurrent change cannot make the two disagree.

## The finding I did not expect: my own emphasis was backwards

The dossier said the resolver's `..` clamp "becomes a redundant safety net"
because the cwd string was always cleaned.

That was **true when written and is now false**, and the way it became false is
the interesting part. The clamp was unexercised on cwd-relative paths only
because the lexical cleaning consumed every `..` before the resolver saw one. So
the thing making the clamp redundant *was the defect* — the code popping unwalked
components.

Remove the defect and a net nobody was relying on becomes the mechanism doing the
work. Nothing about the clamp changed; its status changed.

**Before calling a second mechanism redundant, check whether the thing making it
redundant is a duplicate of it.** Two mechanisms answering one question is not
redundancy, it is a disagreement waiting to be discovered — and the one that
looks superfluous may be the correct one.

Containment itself never moved: the joined path still resolves from the root and
the clamp still sits at the trail floor, which is the case the architecture
already reasoned about ("a hostile un-cleaned join cannot escape"). The verbatim
join simply *is* that case. So the invariant is unchanged and now enforced by
strictly less code.

## A leak becomes a failure when something else wants the resource

The mount cap grew again, 20 to 32, and this growth had a different driver from
the earlier ones.

A container runner **inherits** the session namespace — around sixteen entries,
including a generation of pre-pivot mounts that orphan at the pivot and stay in
the table — then adds its own root and about ten more from its recipe. At twenty
the recipe overflowed and the first over-cap mount **failed the container**.

The orphan accumulation had been a known, tolerated waste with a deferred
collection. It stopped being tidiness the moment a feature needed the budget it
was consuming: **a leak that only wastes a resource is invisible until something
else wants that resource**, and then it presents as an unrelated feature failing
at an arbitrary threshold.

## Two figures, and only one kind could rot

The struct size assertion is written as an **expression over the two array
bounds**, so growing the mount table updates it automatically. The size moved
from 920 to 1400 bytes and the assertion never needed touching.

My prose restated the arithmetic as a literal, so it was wrong the moment the cap
grew — as was the field count in the contract table. Fifth and sixth figure drift
of this sweep, and the same shape as the leaf count two surfaces back: **one
fact, expressed once as a derivation and once as a literal, and only the literal
rots.**

The pattern is consistent enough now to be a working rule for these dossiers:
where the code derives a figure, say so and cite the derivation rather than
copying the number out.
