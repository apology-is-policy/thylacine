---
id: chg-2026-07-31-stalk-sweep
type: chg
title: "The stalk sweep: the resolver + the Path substrate + the namespace audit backfill"
date: 2026-08-01
arc: arc-vault
commits: ["e3a1ab5b"]
touched:
  - moc-kernel
  - haz-single-waiter-rendez
  - arc-identity-detour
  - arc-go-build
  - seam-848-pivot-walk-race
established:
  - moc-kernel-namespace
  - sub-kernel-stalk
  - sub-kernel-path
  - inv-i28
  - inv-i33
  - arc-holotype-rw
  - seam-372-latched-double-xcheck
  - seam-posix-pathname-form-gates
  - seam-fid-monotonic-reclaim
  - seam-932-devsrv-readdir
  - seam-9p-tag-block-on-full
  - view-closed-sub-kernel-stalk
  - view-closed-sub-kernel-path
closed: [seam-848-pivot-walk-race]
opened: [seam-372-latched-double-xcheck, seam-posix-pathname-form-gates, seam-fid-monotonic-reclaim, seam-932-devsrv-readdir, seam-9p-tag-block-on-full]
mirrors-checked: []
depth: rich
---
## What

Sweep batch 5 — the namespace/resolution area (`kernel/stalk.c` 778 +
`stalk.h` + `kernel/path.c` 129 + `path.h` 80 + the `spoor.c` Path
hooks, read in full per the standing sweep bar). Present: the new
[[moc-kernel-namespace]] spine + [[sub-kernel-stalk]] (the trail
lifetime's three failure shapes, cross-on-descent + the STALK_MOUNT
carve-out, the POUNCE run gather / fail-ordering post-scan /
mount-mid-run split / carried attrs / logical_depth, the STALK_STAT
walk-query, the FID-LIFECYCLE cached-open arm, the open=connect
adoption) + [[sub-kernel-path]] (copy-on-walk immutability, the
three hook sites, the lifetime-subordinate refcount). TWO invariant
statement-homes established: [[inv-i28]] (containment + per-component
X-search + the POUNCE fail-ordering equivalence) and [[inv-i33]]
(name retention non-load-bearing). Record: [[arc-holotype-rw]]
(backfill-active — the RW review series' first vault entry) + 9 retro
chgs (stalk-1, stalk-2, #844's resolver-facing slice, #957, the RW-4
fix pair, #66a, #81-June, ER-1, POUNCE, #100) + 8 adts + 20 fnds with
frozen prosecution chains. FIVE seams minted, four of them
long-registered items that had no vault home:
[[seam-372-latched-double-xcheck]] (the P-5 P3),
[[seam-fid-monotonic-reclaim]] (stalk-2 F2 + RW-4 R3-F2 — the
same allocator, two rounds apart), [[seam-932-devsrv-readdir]] (#957
F2), [[seam-9p-tag-block-on-full]] (RW-4 R3-F3, a scripture-vs-impl
USER call) — plus [[seam-posix-pathname-form-gates]], the ONE genuinely
new record: the POSIX pathname-form family (#79–#87) is landed on the
UNMERGED vivarium branch, so this lineage still carries the gaps and
must not re-implement them. `docs/reference/104-stalk.md` +
`131-pounce.md` STUBBED (absorbed). One CORRECTION to a batch-1 record:
[[seam-848-pivot-walk-race]] was minted open from the #844 closed list,
but RW-4's `ns_lock` had closed it six days after that list was
written — flipped to closed with the correction note (reading current
`territory.c` is what caught it).

## Why

The recorded batch-5 target. Both reference docs were MATERIALLY STALE
against the tree: 104-stalk still called stalk-3 "pending" (landed
June), still taught the pre-#844 borrowed-start TOCTOU and the
pre-RW-4 lock-free mount table as OPEN caveats (both closed), quoted
`PGRP_MAX_MOUNTS` 8 (now 20), and its Performance section predated the
POUNCE it documents elsewhere in the same file. Reading the code also
recovered what neither doc states: the `err_code` `-1 → T_E_IO`
collapse and WHY (errno 1 must never surface from the generic
sentinel), the cached-open arm's ANY-mount discard, and the exact
`logical_depth` double-cap that keeps a `walk_attrs`-less tail from
resolving where the pure loop INVALs. The two-plane split earned its
keep again: RW-4's R3-F1 fix was itself CORRECTED by the round-2 catch,
so the two fnd notes freeze both round-time truths and the
classification lesson survives intact.

## Verification

`quaestor lint --all` green through the fail-closed hook; views
re-rendered (dashboard, seams, invariants, audit-triggers, roadmap +
the two new closed-preamble views); three sabotage revert-probes (a
dangling edge, a stale generated view, a dropped dossier section) each
failed as designed and were restored clean. Every retro SHA verified
against `git log`; the test rosters (34 `stalk.*` + 8 `path.*` +
`sys_stat.for_proc` + the 13 `territory_mount.*`) verified against
`kernel/test/test.c`; the four seam behaviors (#372's double stat,
#932's absent readdir, the monotonic allocator, the ns_lock closure)
re-verified in CURRENT code rather than trusted from the 24–59-day-old
closed lists.
