---
id: inv-i38
type: inv
title: "I-38 — Larder cache coherence (close-to-open)"
number: I-38
guards: [sub-kernel-ninep-dev9p]
validated-by: [spec-fs-cache]
strength: spec
created: 2026-07-31
updated: 2026-07-31
---
## Statement

A Larder hit returns exactly what a fresh RPC would return under
close-to-open consistency: Open revalidates, Read serves, OwnWrite
invalidates — including the staged legs (write-behind runs and cached-open
snapshots are refinements of the same discipline, sound only under the
loose-mount single-writer premise). A cache that can serve a byte or
attribute a fresh RPC would not is the violation.

> Backfill note: the guard set is PARTIAL — the Larder mechanism itself
> (`kernel/larder.c`) joins as `sub-kernel-larder` at its sweep; dev9p is
> the policy half (every serve/populate/invalidate call site).

## Enforcement

On [[sub-kernel-ninep-dev9p]]: the per-mutation invalidate/downgrade
pairing (create/write/wstat/rename/unlink/OTRUNC arms), the gen-guarded
populates (capture before the RPC, check at install — the
populate-after-invalidate resurrection close), the cacheability latch
(only a Twalkgetattr-speaking server is ever cached), the wb flush's
attr-invalidate + own-page-install coupling (`err == 0` only), and the
cached-open forced-wire revalidation on strict mounts.

## Validation

[[spec-fs-cache]] — clean + wb + external + liveness cfgs green; five
buggy cfgs are the per-clause counterexamples. **blind-to:** the model
works on content tokens, not bytes — byte-granular overlay arithmetic,
page alignment, and budget accounting are kernel-test territory
(`dev9p.wb_*`, `dev9p.page_cache_serve_and_gate`); the single-writer
premise (`EnableStaging => ~EnableExternalWriter`) is asserted, not
proven, and is exactly what the loose-mount opt-in (B1) scopes.
