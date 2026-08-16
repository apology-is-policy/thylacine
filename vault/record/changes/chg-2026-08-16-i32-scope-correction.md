---
id: chg-2026-08-16-i32-scope-correction
type: chg
title: "Two vault notes disagreed about I-32, and the newer one was right"
date: 2026-08-16
arc: arc-vault
commits: []
touched: [inv-i32, sub-kernel-proc]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
Found while checking the blast radius of a method error, which is the only
reason it was found at all. Not a code defect — a defect in **this vault**, on a
section-28 invariant, where one note claimed a guarantee that another note four
days newer explicitly denied.

## How it surfaced

Sweeping [[sub-kernel-thread]] I used a log query bounded by author date and it
hid a commit. Checking whether my *other* sweeps that day had the same gap, I
found the missed change in one of them was a two-line mechanical rename:
`p->vma_lock` becoming `p->as->lock`.

Two lines per file is exactly the churn a churn-ordered sweep buries. So the
question changed from "did I miss content" to "**how wide is this rename, and
does the vault still speak the old vocabulary**" — twenty notes did.

Ground truth first, from the code rather than by adjudicating between notes:
the old name survives in over a hundred places in the tree, and **every one of
them is comment text**. The field does not exist. The lock is the address
space's.

## Vocabulary was the small half

Most of the twenty are ordering chains naming the lock by its old name. Those
resolve correctly through [[lock-vma]], whose title carries the rename, so they
are stale words rather than wrong claims and were deliberately left alone —
rewriting them is churn against a link that already works. Four are Record-plane
notes and are past-tense provenance, which must not be edited at all.

The real finding was in the two Present-plane notes that reasoned **from** the
old scope.

## The contradiction

[[inv-i32]] said five axes, "all counted on `struct Proc`", three of them
charged under the process's own lock and therefore **exact**.

[[lock-vma]] — updated four days later — said the same lock does *not* serialise
that arithmetic, that the operations are compare-and-swap loops correct with no
lock held, and that what the lock buys is the cap *decision*, with concurrent
charges from outside it overshooting by at most the smaller. It even named the
stale precondition as tracked work.

Both were in the vault. Neither pointed at the other. A reader answering "how
strong is I-32?" would get a different answer depending on which note they
opened, and the *invariant's own note* was the wrong one.

## What the code actually says, which is more than either

Reading it settled the disagreement and then went past it. Three of the five
axes did not merely change lock — **they moved off the process entirely**, onto
the address space, with the mapping list they account for. That changes what the
invariant bounds: two processes sharing an address space share its pages, so one
charge is the honest count and the per-process cap becomes a
**per-address-space** cap. The fork bomb is still bounded, by a different
argument than before — N children means N address spaces, each capped.

And the cap had to move with the counter. Counting on the address space while
capping on the process was considered and rejected, in a sentence worth keeping
verbatim in spirit: two siblings sharing one counter would return *different
verdicts* about it, making the effective bound depend on which sibling faulted
first. **A resource bound whose value depends on scheduling is not a bound.**

What stays on the process is the *authorization* — what it may seed a new
address space with. The right to confer and the thing conferred are different
objects, which is the same split this system makes everywhere else, and I had
collapsed them into one table cell on the first pass through this correction
before catching it.

## Why the invariant note was the one that rotted

The lock note is *about* the lock, so a change to the lock is obviously its
business. The invariant note is about a bound, and mentions the lock only as
part of an argument for a guarantee. **A fact cited in support of a claim is
maintained less carefully than a fact that is the subject of one** — nobody
re-reads their evidence when the evidence's own owner updates it.

That is the general hazard for a knowledge base with cross-references: the
authoritative note gets fixed, and every note that *borrowed* from it keeps the
borrowed version. The borrowing is invisible from the authoritative side, which
is the same asymmetry as a diff never showing who relied on the changed code.

The vault has a check for a link pointing at nothing. It has no check for two
notes asserting incompatible things about one mechanism, and this is the second
kind — both notes lint clean, both are well-formed, and only reading them
together reveals it. Recorded as the open question rather than fixed, because
the mechanical version of that check ("do two notes disagree") is not
mechanically decidable and the useful version is probably narrower.

## What was NOT done

The title still says "per-Proc" with a correction beside it rather than being
renamed. The identifier is cited from the architecture document and from code
comments; a vault-local rename would make the vault's name for a section-28
invariant disagree with scripture's, which trades a small inaccuracy for a
larger one.
