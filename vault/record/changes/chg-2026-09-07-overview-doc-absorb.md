---
id: chg-2026-09-07-overview-doc-absorb
type: chg
title: "absorb docs/reference/00-overview (the bird's-eye index): redirect to the vault dashboard + area MOCs + views"
date: 2026-09-07
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-07
---
00-overview (the docs/reference bird's-eye index). A Phase-0 scaffold (2026-05-04)
whose per-concern sections were never filled and whose figures froze at Phase-1-
in-progress (still read "Test suite count: 0", "Phase 2 planned"). No code owned.

The as-built bird's-eye view is now the vault, generated from code: vault/
dashboard.md (live status), the five area MOCs (moc-kernel/-userspace/-substrate/
-boundary/-stratum), and the cross-cutting views (view-invariants for W^X/I-12 +
lock order + KASLR + 9P lifecycle, view-code-coverage, view-roadmap, view-seams).
The per-file retirement status of the old tree is tracked in docs/REFERENCE.md.

CLEAN REDIRECT, zero fold. A stale scaffold; every figure was frozen at Phase 0,
the reserved cross-cutting sections are now code-verified vault notes, and the
"target end state" layer cake is the shipped system the MOCs describe. Redirect
stub. Zero code change.
