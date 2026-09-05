---
id: fnd-29-r1-f2
type: fnd
title: "The dentry whole-parent invalidate scan is O(dentry_cap) — the same class just fixed for pages"
round: adt-29-r1
severity: P3
status: fixed
surface: [sub-kernel-larder]
threatens: []
fixed-by: chg-2026-07-12-term2-dentry-name
regression: larder.dentry_invalidate_name
created: 2026-07-31
---
## Prosecution

The symmetric hazard: the dentry invalidate-parent walked all
dentry_cap slots per directory mutation — the exact O(cap)-per-own-write
class the round's page_qhash fix removed, left un-indexed for dentries
(measured negligible for the build: ~1 ms vs the page path's
~193M-scan tax, hence P3).

## Disposition

Dispositioned at the round as fixed-by-justification (a measured-
negligible comment) + tracked as the era's task #30 (a parent-keyed
secondary index, the page_qhash twin). CLOSED two days later by a
STRONGER move: the term-2 name-specific invalidation retired the
whole-parent scan entirely — a single-name mutation drops only the
mutated (parent,name) binding, O(1) via the serve's own hash, which is
also the semantically faithful per-token OwnWrite (siblings preserved,
ending the cold-band sibling re-walk thrash). The scan and its seam
died together.
