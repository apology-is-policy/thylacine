---
id: inv-i35
type: inv
title: "I-35 -- Supported mandates and live revocation"
number: I-35
guards: [sub-corvus-authority, sub-corvus, sub-kernel-caps]
validated-by: [spec-mandate, spec-mandate-commit, "usr/lib/corvus-authority/src/tests.rs"]
strength: spec
created: 2026-09-24
updated: 2026-09-24
---
## Statement
Durable grants are bounded by explicit delegation envelopes, immutable support
provenance and domain generations. Administrative records are eligibility,
exercised only by a live trusted administrative legate. Effective authority
requires live support to a founding root. No scalar clearance rank or possession
of operational capabilities authorizes delegation. Restrictive changes close
admission and revoke dependent sessions/resources before completion; no stale
generation may publish a child, redeem, or survive a completed revocation barrier.
Baseline use, temporary elevation and administration are distinct. Principal
identity never changes. Ratified in USER-AUTHORITY-DESIGN (operator approval
of 44d158c3; ratification 4c889a13).

## Enforcement
Only the isolated pure policy subset exists in [[sub-corvus-authority]]. Kernel
Admin scopes/admission, Corvus album transactions and qualified resource-owner
barriers remain required. No end-to-end enforcement claim yet.

## Validation
[[spec-mandate]] and [[spec-mandate-commit]] bounded models +36 pure
policy/codec host tests. **blind-to:** runtime
wiring, durable replay, full runtime quota/memory behavior, account/group resolution,
trusted kernel peer binding, backend resource revocation and physical seat.
