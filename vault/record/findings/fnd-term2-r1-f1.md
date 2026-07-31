---
id: fnd-term2-r1-f1
type: fnd
title: "A reused DIR qid's cached CHILDREN dentries survive rmdir + ino-reuse"
round: adt-term2-r1
severity: P3
status: deferred
surface: [sub-kernel-larder]
threatens: [inv-i38]
seam: seam-larder-reused-dir-dentries
created: 2026-07-31
---
## Prosecution

A cached negative `(Q, "x")` survives `rmdir(Q)` + reuse-of-Q-as-a-new-
dir: the rmdir's drop keys on the CONTAINER parent (it drops
`(container, name)`), and dev9p_create's reuse defense invalidates the
reused child's attr + pages but touches nothing keyed on Q-AS-PARENT. A
walk through the NEW directory at the recycled qid can serve the prior
occupant's stale name bindings. PRE-EXISTING and IDENTICAL under both
the whole-parent drop and the name-specific invalidation — the round
proved the narrowing did not widen it (neither version keys on the dead
qid as parent).

## Disposition

Deferred to the seam (fix candidates: a parent-keyed secondary dentry
index enabling drop-children-of(Q) at the create-reuse site, or
accepting close-to-open staleness for the corner as attr/pages do).
Bounded: needs rmdir + ino-reuse + a same-name walk under the recycled
dir — not driven by the go-build oracle; a stale negative is a
fail-noisy spurious ENOENT, a stale positive resolves to a qid whose own
attr/pages the reuse defense already dropped.
