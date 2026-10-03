---
id: sub-halcyond-interaction-record
type: sub
title: "Halcyon interaction transaction records"
parent: moc-userspace-shell-tui
code:
  - usr/halcyond/src/apprecord.rs
  - tools/test-app-records.py
audit: hard
guarded-by: []
validated-by: [prose]
locks: []
hazards: []
abis: []
design: ["docs/HALCYON-INTERACTION.md", "docs/HALCYON-INTERACTION-ABI.md"]
created: 2026-10-03
updated: 2026-10-03
---
## Purpose

Own one application's HIN1 transaction fid above the shared Receiver. This
prepared component is not a public endpoint and supplies no clipboard authority.
[[sub-halcyond]] owns controller/admission decisions; [[sub-halcyond-service-wire]]
owns the outer nonblocking 9P stream. Native peer sampling, full operation dispatch,
HSC output retirement and complete allocation accounting remain integration work.

## Contract

The connection owner supplies a unique local fid incarnation and remaining
input/output allowances after subtracting every other fid and transport buffer.
Offsets are contiguous from zero. Only a complete envelope and valid typed body
produce Dispatch. Pending requests refuse concurrent writes without destroying
admission. The exact incarnation/request ticket is required for completion.

Retain the previous request and result until a new complete header replaces it.
An exact immediate retry compares bytes against the retained request, without a
second body allocation, and returns Replay. Changed same-ID bytes and older IDs
are invalid. The owner delivers the cached result rather than repeating effects.
A cancelled record releases bytes but preserves the request-ID watermark, so a
late completion or old ID cannot revive the operation. A reused numeric fid
must receive a new incarnation from its connection owner.

## Mechanism

A fixed new-header prefix distinguishes replacement from replay while the
Receiver retains the prior body. Newer headers retire the old cache before body
allocation. Typed decode returns borrowed data only after complete validation.
Replies may be small semantic results or references to admitted snapshots; the
CachedReply trait reports retained capacity. Errors keep their ABI meanings:
unsupported, too-large, invalid and allocation failures remain distinct.

## Data structures

Record stores fixed identity/phase/header metadata, one Receiver and an optional
caller-defined cached reply. Input reservation includes both fixed wire prefixes
and Receiver body capacity. Output reservation uses CachedReply capacity, not
live length. Eight fids share the connection allowance; this is not a per-fid
32KiB entitlement. The total connection/session ledger belongs to the adapter.

## Concurrency

Only the dedicated service owner mutates records. There are no locks, syscalls,
threads or shared mutable globals. Request borrows cannot outlive mutable state
changes. The caller routes async completions through exact tickets and retires
caches plus transport output before acknowledging HSC cancellation.

## Invariants enforced

- Partial or malformed bodies never dispatch.
- Same-ID retries cannot alter the cached request or dispatch another mutation.
- A reused numeric fid cannot accept its predecessor's completion.
- Cancellation preserves monotone ID history and drops retained payloads.
- Input prefixes, body capacity and cached reply capacity remain accountable.

## Error paths

Framing errors poison assembly until explicit cancellation. Busy on a pending
write preserves that request. Stale completion returns Gone; budget failure
refuses the result without replacing the pending operation. The caller must
reserve a cacheable semantic result before any external mutation. No cached
result substitutes for live authority checks or snapshot lifetime ownership.

## Performance

Assembly and replay comparison are linear in received bytes; each record is
bounded to32KiB. Replay borrows the existing body and performs no new body
allocation. Eight fixed fid slots and caller-supplied aggregate allowances bound
storage. No hard latency or full service-memory measurement is claimed here.

## Prosecution

Actual-source host fixtures exercise every fragment boundary, bytewise changed
replays, stale IDs/fids, pending cancellation, aggregate eight-fid budgets and
exact errno mapping. Named mutations prove the relevant assertions discriminate
broken implementations. These tests are blind to native fd sampling, 9P Tflush
mapping, actual output retirement and clipboard client integration; those remain
activation gates. Review is single-agent.

## Seams

Full9P operation dispatch must map fid incarnation, request/tag and Tflush to
this record. Native kernel peer sampling, admission completion routing, pinned
read results and cache/output retirement before HSC ACK belong to the service
adapter. Its budget must include every record plus transport capacity.

## Caveats

This prepared component is not a public clipboard service. Its injected
allowances and incarnations must come from the trusted connection owner.
No independent audit, native clipboard, Pi or minimum-display qualification is
implied by pure host tests or the existing native transport regression.

## Provenance

(generated -- incoming touched backlinks)
