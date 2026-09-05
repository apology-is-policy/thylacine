---
id: fnd-l1f-r1-f1
type: fnd
title: "create at a reused ino drops the child's ATTR but not its PAGES — a stale prior-occupant page can serve"
round: adt-l1f-r1
severity: P1
status: fixed
surface: [sub-kernel-larder, sub-kernel-ninep-dev9p]
threatens: [inv-i38]
fixed-by: chg-2026-07-09-larder-l1f
regression: dev9p.create_invalidates_reused_child_pages
created: 2026-07-31
---
## Prosecution

The page cache is keyed by qid.path — the SAME key as the attr cache. A
create at a freed+reused ino gets a fresh qid.version, but dev9p_create
dropped only the child ATTR, never the child PAGES: if the fresh
qid.version COLLIDES with a cached prior-occupant page's cvers (a
collision the Thylacine tree cannot rule out — it depends on Stratum's
fresh-inode si_cvers assignment), a read of the new file serves the
dead file's bytes. The attr path already treated ino reuse as REAL; the
page path silently bet a DATA-INTEGRITY property on an unstated
cross-project guarantee.

## Disposition

Fixed in the arc-close commit: `larder_page_invalidate` of the child
qid.path alongside the attr invalidate in dev9p_create — the exact page
twin of the L1c attr defense. Regression non-vacuous (fails pre-fix).
The exemplar of the two-independent-prosecutors value: the self-audit's
SA-10 had verified the ATTR child-invalidate present and never asked
about the page twin — finding one defense is what stops you asking
whether it is the whole defense.
