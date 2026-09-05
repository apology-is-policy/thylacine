---
id: chg-2026-09-05-caps-fork-inherit
type: chg
title: "sub-kernel-caps brought current: the Linux-clone fork-inherits-caps (rfork_forked_with_caps) + the resolved comment drift -- its first EARNED update"
date: 2026-09-05
arc: arc-vault
commits: ["c2991edb"]
touched:
  - sub-kernel-caps
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-05
---
The proc.c cluster's capability sibling. This dossier's own Provenance recorded
two consecutive intervals (2026-08-15, 2026-08-16) where its staleness was
BORROWED from co-tenant proc.c churn -- no capability token changed. `830817c4`
(2026-08-26, phenotype-fork-inherits-caps) is the FIRST interval where the churn
was genuinely caps-relevant, verified against the code:

## The fork-inherits-caps mechanism

`rfork_forked_with_caps` (proc.c 1738) is a new rfork variant the dossier's strip
discussion did not name; syscall.c 9855 calls it with `caps_mask = CAP_ALL` for a
Linux `clone` (a clone carries no caps argument). So a Linux-phenotype child
inherits the parent's whole fork-grantable set -- still `& ~CAP_ELEVATION_ONLY`,
so I-2's monotonic reduction holds on the phenotype path exactly as native.
Added to the strip Mechanism. (This is the caps view of the same fork-caps change
whose proc-core view landed in [[sub-kernel-proc]] and whose caller-side is
[[sub-kernel-syscall-dispatch]], both this session.)

## The comment drift is now resolved (a withdrawn caveat)

The Caveats carried "two comment enumerations have drifted": the
`CAP_ELEVATION_ONLY` comment said "All five" and listed six; the `CAP_ALL`
comment enumerated four and omitted DEBUG + JIT. `830817c4` FIXED both -- caps.h
now reads "All six" and enumerates all six (verified 193/194-199). Rewrote the
caveat as resolved, keeping the durable lesson (a `_Static_assert` pins an
expression, nothing pins a sentence; the macros were right throughout). The
CAP_JIT "CAP_HW_CREATE class" caveat is unaffected -- caps.h 157-162 still carries
that self-correction, so the three design docs' wrong phrase persists; kept.

## The main enumeration was already current

The dossier's own CAP_ALL / CAP_ELEVATION_ONLY bit lists (six each, DEBUG + JIT in
elevation-only) already matched the macros -- only the CODE-comment caveat and the
new rfork variant were stale. `updated:` -> 2026-09-05. guarded-by unchanged [];
devcap.c has not moved since 2026-06-10 (well before the dossier), so the cap
device is untouched.

## Remaining proc.c-cluster siblings

sub-kernel-death + sub-kernel-jobctl also claim proc.c and are flagged stale by
the same shared churn -- to be checked whether theirs is earned or borrowed.
