---
id: fnd-b1-r1-f1
type: fnd
title: "A third-actor stale-fid repopulate re-satisfies snapshot coverage at the OLD cvers — a torn cached-open view"
round: adt-b1-r1
severity: P2
status: fixed
surface: [sub-kernel-larder, sub-kernel-ninep-dev9p]
threatens: [inv-i38]
fixed-by: chg-2026-07-11-b1-loose
regression: larder.pages_snapshot_gen_witness
created: 2026-07-31
---
## Prosecution

The page populate tags pages with the READING FID's open-time qid.vers,
not the server's version of the bytes. So after an own-write invalidate
(gen bumped, pages dropped), a fid opened PRE-write can repopulate
POST-write bytes tagged the OLD cvers — re-satisfying `pages_cover` at
that cvers. The cached-open snapshot then mints a TORN view: post-write
bytes paired with the pre-write size/attr — a view no fresh RPC could
ever return (I-38 NoWrongRead). PRE-EXISTING since the fid-lifecycle
keeper, on BOTH strict and loose paths (strict's window spans the wga
RPC; B1 merely re-hosted it between two lock holds) — puncturing the
fid-lifecycle round's "snapshot atomicity SOUND" entry.

## Disposition

Fixed: `larder_pages_snapshot` takes the caller's pre-decision gen
snapshot (`seq0`) and FAILS CLOSED under its own lock hold if any
invalidation event named this file since — coarse by design (any
same-file invalidate in the µs window → fallback to the wire;
correctness-neutral). The regression constructs the exact interleave:
coverage re-satisfied, the witness fails it, a fresh capture serves. G4
later scoped the witness per-file so unrelated write churn stops killing
cached-opens. The durable lesson (the round's export): a two-party
serialization argument is incomplete until every THIRD party that can
re-establish the observed state is enumerated.
