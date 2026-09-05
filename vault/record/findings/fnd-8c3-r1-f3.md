---
id: fnd-8c3-r1-f3
type: fnd
title: "A real transport break concurrent with a stop is DEFERRED to resume"
round: adt-8c3-r1
severity: P3
status: documented
surface: [sub-kernel-ninep-client]
threatens: [inv-i9]
created: 2026-07-31
---
## Prosecution

A genuine error/EOF arriving while debug_stop_req is set routes to the
stopped arm (not mark_dead); the error surfaces only when the reader
resumes and re-recvs.

## Disposition

Documented safe for the trusted server: the only reachable break is a
peer-gone EOF, which is STICKY (re-manifests on resume or on a survivor ->
mark_dead; no strand). The untrusted malformed-frame facet folds into the
frame-atomicity work.
