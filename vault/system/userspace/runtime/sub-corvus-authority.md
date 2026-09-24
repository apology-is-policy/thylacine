---
id: sub-corvus-authority
type: sub
title: "Corvus authority policy engine"
parent: moc-userspace
code: [usr/lib/corvus-authority/src/lib.rs, usr/lib/corvus-authority/src/tests.rs, usr/lib/corvus-authority/src/abi.rs, usr/lib/corvus-authority/src/codec.rs, kernel/include/thylacine/authority_wire.h, tools/check-authority-abi.py, usr/lib/corvus-authority/Cargo.toml]
audit: hard
guarded-by: [inv-i35]
validated-by: [spec-mandate, spec-mandate-commit, "usr/lib/corvus-authority/src/tests.rs"]
locks: []
hazards: []
abis: [abi-user-authority]
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
Activation and Time. The record/action ABI is reserved by [[abi-user-authority]]; no runtime endpoint yet.

## Concurrency
No shared statics, syscall or lock. Exclusive mutation via Rust borrowing.
Runtime must serialize commit and fetch fresh kernel-authenticated inputs.

## Invariants enforced
[[inv-i35]] policy subset only: supported, attenuated, attributed grants and
policy-level revocation. This does not implement process or resource teardown.

## Error paths
Canonical decode rejects bad versions/tags, reserved bytes, impossible lengths,
trailing bytes and malformed vectors. No partial record escapes. A decoded
record is untrusted data; policy insertion still needs live source authority.

Unknown actions, malformed/noncanonical selectors, stale generation/revision,
expiry without trusted UTC, missing support, over-limit graphs and unsupported
issuance fail closed before mutation. Global revision overflow refuses mutation.

## Performance
A dense 4096-record host fixture uses 4,127,808 bytes of retained Vec payload
(excludes allocator bookkeeping and guest RSS); closure is about15ms in a macOS
debug build. This is not a guest timing or end-to-end revocation result.

4096 physical records including tombstones, 256 live/revoking per subject,
16 selectors, 8 supports and depth 16. Tombstones currently consume the physical
limit: safe but stricter than the design's 4096-live target. No compaction yet.
Binary ID lookup; iterative depth-memo support walk avoids exponential diamond
paths. Revocation uses one forward pass through topologically sorted IDs. Codec buffers, traversal scratch and ledger insertion allocations are fallible;
the complete transaction reservation is still required before authentication.

## Prosecution
36 host tests pass, bare-target check passes, host Clippy -D warnings passes.
TLC models pass 154 and 3768 states; fourteen named mutants fail as intended.
Self-review only, under operator direction. No independent audit claimed.

## Seams
Kernel activation inputs must never be decoded from client bytes. Backend view
containment, durable replay, account/group resolution, transaction binding,
complete pre-auth transaction reservations and Corvus integration remain implementation work.

## Caveats
Exact selector sets only: no guessed path-prefix containment. Clock is an
explicit trusted input. Revocation completion here is a pure state transition,
not proof of runtime acknowledgements. Canonical MDTM v1 codec is present. No discovery/runtime gate yet.

## Provenance
