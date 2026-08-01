---
id: fnd-stube2-r1-f6
type: fnd
title: "The comment that made the NUL terminator look optional"
round: adt-stube2-r1
severity: P3
status: fixed
surface: [sub-kernel-stalk]
threatens: []
fixed-by: chg-2026-05-21-p5-chroot
regression: "none (documentation; its consequence is pinned by fnd-stube2-r1-f1's test)"
created: 2026-08-01
---
## Prosecution

The comment above the name-staging code claimed `dev9p_walk` uses
"strlen-like length discovery on the `names[]` array, paired with the
length array", and that "the terminator is defense-in-depth".

Both clauses are false. The `Dev` walk vtable has no length array at
all — the NUL IS the length — so the terminator is REQUIRED, not
belt-and-braces. The comment described a safety net that does not
exist, which is precisely what licensed writing the terminator
conditionally.

## Disposition

Fixed: rewritten to state that the terminator is required and why.

Filed as its own finding rather than folded into the P0 because the
causal direction matters. The bug was not a slip that a comment failed
to prevent; the comment was the reason the slip looked safe. A wrong
claim about why something is optional is how it becomes optional — and
a reviewer reading the code with the comment is reading an argument for
the bug.
