---
id: seam-larder-reused-dir-dentries
type: seam
title: "A reused DIR qid's cached CHILDREN dentries survive rmdir+reuse"
status: open
surface: [sub-kernel-larder]
opened-by: fnd-term2-r1-f1
tracker: ""
created: 2026-07-31
updated: 2026-07-31
---
**Owed**: dropping (or accepting-as-bounded) the dentries keyed on a
dead directory qid AS PARENT when its ino is reused. A cached negative
`(Q, "x")` survives `rmdir(Q)` + ino-reuse-as-a-new-dir: the drop hooks
key on the CONTAINER parent (the rmdir invalidates `(container, name)`),
and `dev9p_create`'s L1f reuse defense invalidates the reused child's
attr + pages but nothing keyed on Q-as-parent. A later walk through the
NEW directory at the recycled qid can then serve the prior occupant's
stale positive/negative name bindings.

**What closes it**: a parent-keyed secondary dentry index (the
`page_qhash` twin — drop-children-of(Q) at the create-reuse site), or
accepting close-to-open staleness for this corner like attr/pages
(bounded by LRU / the guest's own next mutation in the new dir).

**Risk while open**: pre-existing (identical under the whole-parent-drop
and name-specific invalidation designs — the term-2 round proved the
narrowing did not widen it); needs rmdir + ino-reuse + a walk of the
same component name under the recycled dir — not driven by the go-build
oracle; a stale NEGATIVE serve is a spurious ENOENT (fail-noisy), a
stale POSITIVE resolves to a child qid whose own attr/pages were dropped
by the reuse defense (a fresh RPC on first real use). Unpleasant, not
privilege-bearing.
