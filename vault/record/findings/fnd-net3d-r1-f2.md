---
id: fnd-net3d-r1-f2
type: fnd
title: "poll_accepts gated its typed get on liveness only, not proto"
round: adt-net3d-r1
severity: P2
status: fixed
surface: [sub-netd-server]
threatens: []
fixed-by: chg-2026-06-17-net3-server-side
created: 2026-07-31
---
## Prosecution

The one TCP-only typed-recovery site relying on a NON-LOCAL invariant
(the fid-pins-slot argument) rather than a local proto check — the
exact line F1's cross-proto re-mint panics on.

## Disposition

Fixed — subsumed by the F1 poll_accepts proto+gen guard: the typed
recovery is now locally gated. The lesson generalized into the
proto-dispatch-completeness standing prosecution item (every typed get
locally discriminated or TCP-only by construction).
