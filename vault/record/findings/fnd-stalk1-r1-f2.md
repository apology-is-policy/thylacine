---
id: fnd-stalk1-r1-f2
type: fnd
title: "The reuse-nc-violation + nqid!=1 branches are untested defense-in-depth"
round: adt-stalk1-r1
severity: P3
status: documented
surface: [sub-kernel-stalk]
threatens: []
created: 2026-08-01
---
## Prosecution

The `w->spoor != nc` (reuse-nc contract violated) and dev9p-shape
`nqid != 1` cleanup branches are unreachable with every real Dev (all
honor the contract; the fixture too) — inherited verbatim from the
audited walk-open equivalents. If a future Dev violated the contract,
the branch must pick detach-vs-clunk correctly or it clunks the
parent's shared fid.

## Disposition

Documented in the test-file header rather than covered: a deliberately
self-cloning fixture was judged not worth the contrived
leak-of-the-other-spoor. The branches were re-traced sound at RW-4 and
the POUNCE added the sharpened `shape_ok` twin (whose violation shapes
ARE exercised via the fixture's query/partial forms).
