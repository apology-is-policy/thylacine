---
id: chg-2026-08-16-mirror-drift-closed
type: chg
title: "Main took both filed findings, and verified rather than inferred the second"
date: 2026-08-16
arc: arc-vault
commits: ["a119f0cc"]
touched: [inv-i32]
established: []
closed: [seam-scripture-invariant-mirror-drift]
opened: []
depth: rich
created: 2026-08-16
---
The two findings this vault filed against the invariant tables both landed on the
implementation branch. Recording the closure, and the one thing about how it was
closed that is worth more than the closure itself.

## The drift, restated at the source

The always-loaded mirror now carries the two granularities, the
authorization-versus-enforced-bound distinction, **and the rejected inverse** —
the shape a future reader is most likely to "simplify" back into existence.

That last part is the addition I would not have thought to ask for. Recording
*what was rejected and why* is what makes a correction durable, because the
pressure that produced the wrong version is still there after the wrong version
is gone. The rejected shape here is a single counter on the shared object with
the cap on the per-process one: two siblings sharing an address space would then
return different verdicts from one counter, so the bound would depend on which
sibling happened to fault.

And it is recorded with the detail that generalizes: the charging function must
never take the cap as a caller-supplied parameter, **because that signature IS
the rejected shape wearing different clothes.** A parameter is a shape. A rule
that names only the data layout leaves the door open at the interface.

## The status flip was verified, not inferred

The second finding was that a reserved invariant's own stated trigger had fired —
the arc had landed and its close audit had come back clean — and nobody had moved
the row. I deliberately left the declaration to the implementation track, since
"is this now enforced" is theirs to say and not a documentation question.

They did not take the commit titles for it. Every precondition the row set for
itself was checked in the tree: the structure the row said did not exist yet, the
system call, the break arm, and the model's three counterexample configurations
**named individually and found individually**.

**That is the right response to a finding of this kind and it is not the
automatic one.** The tempting path is to treat a well-argued report as
established and act on it; the report was about a row being wrong, so trusting it
uncritically would have replaced one unverified status with another. The
generalization they drew — *a status field whose flip is nobody's step stays
unflipped* — only holds up because they checked the flip was actually owed.

## What made it a commit rather than a sweep

The framing that moved it was the inverted risk: **the stale copy is the one that
loads into every session automatically, and the correct one has to be opened on
purpose.** So the wrong text had near-total readership and the right text had
almost none — backwards from ordinary staleness, where the neglected document is
also the unread one.

Worth keeping because it is a priority rule, not an observation: **rank a
documentation defect by who reads it, not by how wrong it is.** A small error in
an auto-loaded file outranks a large one in a file people open deliberately, and
this vault's own note inherited the wrong version by exactly that route — two
summaries agreeing with each other rather than with the source.
