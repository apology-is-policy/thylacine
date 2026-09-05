---
id: fnd-29-r1-f3
type: fnd
title: "The multifile regression proved page_hash relink but never qhash relink after slot reuse"
round: adt-29-r1
severity: P3
status: fixed
surface: [sub-kernel-larder]
threatens: []
fixed-by: chg-2026-07-11-fid-lifecycle
regression: larder.page_invalidate_multifile
created: 2026-07-31
---
## Prosecution

`larder.page_invalidate_multifile` exercised the two-index install and
the discriminating invalidate, but after a victim-slot reuse it asserted
only the primary-hash serve — a qhash relink regression (a reused slot
linked in page_hash but not page_qhash) would have passed, leaving the
next invalidate incomplete (a stale page surviving an own-write).

## Disposition

Fixed in-round: the test gained a second `larder_page_invalidate` after
the re-install, asserting the invalidation count increments AND the
re-installed page then misses — closing the qhash-relink coverage. The
coverage-gap class: a two-index invariant needs a probe per index, not
one probe that both indexes happen to satisfy.
