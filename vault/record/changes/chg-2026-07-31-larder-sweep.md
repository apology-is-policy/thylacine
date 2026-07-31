---
id: chg-2026-07-31-larder-sweep
type: chg
title: "The Larder sweep: the guest-FS-cache mechanism dossier + the full L1/B1/D44/term audit backfill"
date: 2026-07-31
arc: arc-vault
commits: ["*(pending)*"]
touched:
  - moc-kernel-ninep
  - sub-kernel-ninep-dev9p
  - inv-i38
  - spec-fs-cache
  - arc-go-build
established:
  - sub-kernel-larder
  - lock-larder-l-lock
  - seam-larder-shrinker
  - seam-larder-loom-bypass
  - seam-larder-stale-child-attr
  - seam-larder-reused-dir-dentries
  - seam-larder-lazy-array-robustness
  - seam-larder-cacheable-proxy
  - msr-gofmt-warm
  - view-closed-sub-kernel-larder
closed: []
opened: []
mirrors-checked: []
depth: rich
---
## What

Sweep batch 2 — the Larder (`kernel/larder.c` + `larder.h`, read in full
per the standing sweep bar), quaestor's first live consumer. Present:
the `sub-kernel-larder` mechanism dossier (the three sub-caches' O(1)
index shape, the G4/G2 gen ring, the per-sub-cache coherence
asymmetries — attr Read-ungated, dentry own-write-only with no cvers
gate, page cvers-or-own — the cached-open pure readers with the B1 gen
witness, the capacity/sizing history, the 32-test roster verified
against the tree); [[lock-larder-l-lock]]; six `seam-larder-*` notes
(shrinker / loom-bypass / stale-child-attr / reused-dir-dentries /
lazy-array-robustness / cacheable-proxy); [[msr-gofmt-warm]] (the
warm-floor series 1352 → 1147 → 367 → 249 → 195 ms with the two-harness
method caveat); `inv-i38.guards` + `spec-fs-cache.models` grown to the
mechanism (their recorded backfill hooks discharged); the dev9p
dossier's seam pointers repointed from prose to the new notes. Record:
eight retro chgs with git-verified SHAs (L1c scripture+spec+substrate /
L1d / L1e / L1f close / B1 loose / D44 read-band / term-2 dentry-name /
term-4 close) appended to [[arc-go-build]]; seven adt rounds (L1f, task
#25, task #29, B1, D44, term-2, term-4) and 21 fnd notes with frozen
prosecution chains, including the cross-chunk closures the edge model
was built for (the #25 O(cap)-invalidate fixed by the #29 qhash inside
the fid-lifecycle keeper; the #29 dentry-scan seam retired by the term-2
name-specific invalidation). `docs/reference/132-larder.md` STUBBED
(absorbed; no REFERENCE.md row existed to repoint).

## Why

The recorded batch-2 target: the Larder absorbs its reference doc and
its audit history becomes queryable (`quaestor query fnd --surface
sub-kernel-larder`; the committed preamble `view-closed-sub-kernel-
larder` renders 8 closed findings). The out-of-surface findings of the
batched rounds (term-4's Stratum F1/F2/F4; term-2's Surface-B
measurement dispositions) are recorded in the adt bodies, NOT given fnd
notes with fabricated surfaces — the batch-1 rule.

## Verification

`quaestor lint --all` green at 211 notes / 0 fails / 0 warns through the
live fail-closed gate; views re-rendered (dashboard, seams, invariants,
the new preamble); three sabotage revert-probes (a dangling `fixed-by`
edge on a new fnd, a hand-edited generated view, a dropped required
dossier section) each FAILED as designed and were restored clean. Test
names in the dossier verified against `kernel/test/test.c` declarations
(32 `larder.*` cases), commit SHAs against `git log`.
