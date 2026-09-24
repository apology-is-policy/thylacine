---
id: sub-corvus-authority
type: sub
title: "Corvus authority policy engine"
parent: moc-userspace
code: [usr/lib/corvus-authority/src/lib.rs, usr/lib/corvus-authority/src/tests.rs, usr/lib/corvus-authority/Cargo.toml]
audit: hard
guarded-by: [inv-i35]
validated-by: [spec-mandate, "usr/lib/corvus-authority/src/tests.rs"]
locks: []
hazards: []
abis: []
design: ["docs/USER-AUTHORITY-DESIGN.md", "docs/USER-AUTHORITY-STATUS.md"]
created: 2026-09-24
updated: 2026-09-24
---
## Purpose
UA-1 pure, bounded policy evaluation, isolated from Corvus runtime integration.
This is not yet a usable administrative endpoint or persistent database.

## Contract
Use/Activate records convey no delegation power. Issuance requires a live,
kernel-derived Admin activation, exact policy revision and one complete source
envelope. Domains, canonical principal sets, owner-issued resource views,
operations, authentication floor, terms and delegation depth may only narrow.
Supports are conjunctive, immutable ID/revision references leading to founding
roots. Cycles, transitive self-grants and ID reuse fail closed.

## Mechanism
`Ledger::check_issue` authorizes before `issue` mutates. `is_live` traverses all
supports. `begin_revoke` closes the complete dependent graph before
`finish_revoke`; runtime must supply the durable/kernel/backend barrier.
`install_founding` is a trusted installer/replay entry, never an ordinary verb.

## Data structures
Sorted immutable-ID ledger; domain generations; typed Scope, Envelope, Mandate,
Activation and Time. Numeric actions are internal, not a public ABI reservation.

## Concurrency
No shared statics, syscall or lock. Exclusive mutation via Rust borrowing.
Runtime must serialize commit and fetch fresh kernel-authenticated inputs.

## Invariants enforced
[[inv-i35]] policy subset only: supported, attenuated, attributed grants and
policy-level revocation. This does not implement process or resource teardown.

## Error paths
Unknown actions, malformed/noncanonical selectors, stale generation/revision,
expiry without trusted UTC, missing support, over-limit graphs and unsupported
issuance fail closed before mutation. Global revision overflow refuses mutation.

## Performance
4096 physical records including tombstones, 256 live/revoking per subject,
16 selectors, 8 supports and depth 16. Tombstones currently consume the physical
limit: safe but stricter than the design's 4096-live target. No compaction yet.
Binary ID lookup; iterative depth-memo support walk avoids exponential diamond
paths. Allocation is bounded logically but not yet reserved fallibly end to end.

## Prosecution
24 host tests pass, bare-target check passes, host Clippy -D warnings passes.
TLC composition model passes 154 states; seven named mutants fail as intended.
Self-review only, under operator direction. No independent audit claimed.

## Seams
Kernel activation inputs must never be decoded from client bytes. Backend view
containment, durable replay, account/group resolution, transaction binding,
fallible pre-auth reservations and Corvus integration remain implementation work.

## Caveats
Exact selector sets only: no guessed path-prefix containment. Clock is an
explicit trusted input. Revocation completion here is a pure state transition,
not proof of runtime acknowledgements. No codec/discovery/runtime gate yet.

## Provenance
