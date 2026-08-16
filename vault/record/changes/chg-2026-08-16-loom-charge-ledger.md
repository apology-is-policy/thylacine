---
id: chg-2026-08-16-loom-charge-ledger
type: chg
title: "Loom's I-32 ledger: owning the ring is not paying for the pages"
date: 2026-08-16
arc: arc-vault
commits: ["7e89a3b6"]
touched: [sub-kernel-loom]
established: []
closed: []
opened: [seam-loom-sqpoll-owner-unbackstopped]
mirrors-checked: []
depth: rich
created: 2026-08-16
---
Four changes to the ring since the dossier was written, all of them the same
subject: **who gets refunded**. The dossier had the whole thing as one line
("the ring region is charged to the creating Proc's page budget"), which was
true and covered none of the mechanism that makes it true.

This is the other end of [[chg-2026-08-16-burrow-attribution]] — the Burrow side
recorded the payer, and Loom is where the wrong payer was being read.

## A fix that introduced a P1, and the sentence that did it

The sequence is worth keeping whole because each step is reasonable.

A refund was being skipped on the ordinary teardown order, because the
predicate sampled the reference count *before* the drop. The repair was right:
only the drop itself knows whether it freed the pages, so the drop-primitives
now report it.

That repair then had to name a Proc to refund — and teardown, uniquely in the
tree, has no Proc argument. So it used the ring's owner, and justified it:
*registering requires a ring handle from this Proc's own table, and the handle
is neither transferable nor dup-able.*

**That sentence is true and proves the wrong proposition.** It establishes who
owns the *ring*. The refund needs who paid for the *region*. Buffer
registration accepts any writable anonymous region of the owner — including one
another process allocated and shared in, which is exactly what the shipped
zero-copy network flow registers. So a consumer could be refunded for its
supplier's pages: an under-count, on a non-exempt Proc, through five ordinary
API calls.

The generalization I want to keep: **a proof of the adjacent proposition is the
most dangerous kind of wrong**, because it is *load-bearing about something* and
survives review by being locally correct. The comment was doing its job — it
just answered the question next to the one being asked. Same shape as the
severity-keyed-to-one-link and category-not-property lessons: the reasoning is
sound, the subject is off by one.

The fix moves the question from the ring to the region. Each eager charge stamps
its payer on the Burrow; a settler *claims* it — a read that also clears — and
refunds only what comes back. A region this Proc never paid for returns zero.

## Every tie broken the same way, deliberately

The clear-on-read makes the refund exactly-once, so two racing settlers cannot
both win. The claim must happen *before* the drop, since a freeing drop takes
the record with it; if the drop turns out not to free, the claim is put back.

That leaves a real window between claim and restore, and the chosen failure mode
is a charge that outlives its region until the payer's next release point.

**An over-charge caps a Proc early; an under-charge inflates its budget, which is
the bound failing.** Every tie in this mechanism resolves toward over-charging,
and stating that once explains every local decision in it — including why the
one defect above mattered enough to be a P1 while its mirror-image (a charge
that leaks and is never refunded) was the *milder* half of the same gap.

## The duplication that must not be removed

The ring stores the creating Proc twice, for the two ledgers, bound at opposite
ends of setup. This looks exactly like something to tidy up.

The page owner binds **last**, after the final failure path, so rollbacks — which
refund explicitly — cannot be double-refunded by teardown. The thread owner
binds **first**, at the charge, because rollbacks deliberately *don't* settle it
and teardown must.

Same goal, inverted discipline, because one ledger is settled by the rollback and
the other by the teardown. Merge the pointers and one of them breaks; move either
binding toward the other and the same. **Nothing in the code marks them as a
matched pair** — this is the [[sub-kernel-stalk]] dot-gate shape again, two
things that look redundant and must stay apart, which is the argument for a
dossier rather than a comment.

## The gap I found reading it

Both pointers rest on the *same* lifetime argument. The page one is validated at
every use against magic and pid, with the code saying plainly that this is an
argument rather than an enforced invariant and should degrade to a skipped refund
if it ever breaks.

The thread one is dereferenced bare — same function, same call site, forty lines
apart — and its use is a *write*. [[seam-loom-sqpoll-owner-unbackstopped]].

Nothing is reachable today. What it costs is a reader's attention: **applying
defense-in-depth to one of two identical arguments makes the unprotected one look
examined.** Finding the check on the first pointer is positive evidence that
someone thought about lifetime here, and that evidence is misleading about the
second.

## Two smaller things, both about numbers

A bound on the registered-handle table used to be justified by matching the
per-Proc handle limit. That limit was lifted; the match stopped being a reason.
The repair **left the value alone and rewrote the rationale** — the table is
charged per ring, and a Proc may hold many rings, so the bound was never about
what a Proc can hold. A stale-constant sweep that re-checks only values finds
nothing here, which is the point: *the value was never the stale part.*

And a caveat of mine that I checked rather than trusted. An overflow-safety
comment over-estimates the completion array "at twice its real size" — correct,
and the route there is two errors in opposite directions: the submission ring's
entry count where the completion ring's is double, against the uniform 64-byte
entry size where a completion entry is 16. Four times too big over two times too
small. **Only the compensation makes it conservative**, so widening a completion
entry turns the same comment into an under-estimate. Verifying my own prior
caveat is what surfaced that; re-asserting it would not have.
