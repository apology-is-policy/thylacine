---
id: inv-i11
type: inv
title: "I-11 — per-9P-session fid identity stable for the fid's open lifetime"
number: I-11
guards: [sub-kernel-ninep-client, sub-kernel-ninep-session]
validated-by: [spec-9p-client]
strength: spec
created: 2026-07-31
updated: 2026-07-31
---
## Statement

Within one 9P session, a fid names one server-side file identity for the
fid's whole open lifetime. The client unbinds a fid at `Tclunk` SEND time
(no further ops on it even while the Rclunk is in flight), and fid numbers
are never reused (the monotonic allocator) — so a late reply can never bind
onto a recycled fid, and a stray/duplicated reply's only reachable fid
mutation is a walk-family bind onto a FRESH never-reissued `new_fid`.

## Enforcement

`p9_session_send_clunk` (send-time unbind BEFORE storing the outstanding
entry) · `p9_client_alloc_fid` (monotonic, non-wrapping) · walk send-time
preconditions (src bound, new not bound, no other in-flight op on new).

## Validation

[[spec-9p-client]]: `FidStability` — the `fid_after_clunk` buggy cfg is the
executable counterexample. **blind-to:** server-side fid state (the server's
own table is beneath the model); the abandoned-walk server-side fid leak is
the documented residual (a dead Proc can't clunk what its late Rwalk bound —
bounded per shared client, #845 F3).
