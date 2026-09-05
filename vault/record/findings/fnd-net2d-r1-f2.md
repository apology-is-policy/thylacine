---
id: fnd-net2d-r1-f2
type: fnd
title: "h_attach accepted fid == P9_NOFID — the no-fid sentinel bindable as a live fid"
round: adt-net2d-r1
severity: P3
status: fixed
surface: [sub-netd-server]
threatens: []
fixed-by: chg-2026-06-17-net2-netd-birth
created: 2026-07-31
---
## Prosecution

NOFID is the 9P "no fid" sentinel, never a live fid; binding it lets a
later op address it as real. Protocol laxity, no corruption (treated as
an opaque u32 everywhere), reachable only from a non-conformant client.

## Disposition

Fixed in the close: `h_attach` rejects `fid == P9_NOFID` (E_INVAL) and
`h_walk` rejects `newfid == P9_NOFID` — fail-closed sentinel guards.
