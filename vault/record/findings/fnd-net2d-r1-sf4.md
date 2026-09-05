---
id: fnd-net2d-r1-sf4
type: fnd
title: "Cross-session connection liveness: any session that can name /net/<proto>/N holds it live"
round: adt-net2d-r1
severity: P3
status: documented
surface: [sub-netd-server]
threatens: []
created: 2026-07-31
---
## Prosecution

A self-audit finding (beyond the round's headline counts): the
refcount keys on fids, and any 9P session whose namespace reaches /net
can walk to a live N and hold it — cross-session liveness is a shared
property, not per-opener.

## Disposition

Closed justified: this IS the Plan 9 shared-namespace model, bounded by
MAX_SLOTS=16 and the I-1 namespace firewall; teardown reconciles a
dying session's refs. Carried as the [[sub-netd-server]] cross-session
caveat.
