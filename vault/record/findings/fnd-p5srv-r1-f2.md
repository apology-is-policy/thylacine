---
id: fnd-p5srv-r1-f2
type: fnd
title: "SrvService.magic + devsrv_svc_ref.magic offsets unpinned — the first-u64 discriminator rested on field order"
round: adt-p5srv-r1
severity: P2
status: fixed
surface: [sub-kernel-devsrv]
threatens: []
fixed-by: chg-2026-05-19-srv-birth
created: 2026-07-31
---
## Prosecution

The whole KObj_Srv / devsrv-aux discrimination scheme reads the FIRST
u64 of an object as its magic. `SrvConn` pinned its magic to offset 0
with a `_Static_assert`; `SrvService` and `devsrv_svc_ref` did not — a
field reorder burying either magic would silently misread garbage at
offset 0 across 8+ consumer sites (`handle_release_obj`,
`srv_handle_poll`, `devsrv_close`, `devsrv_conn_of`, `devsrv_poll`, the
accept/peer resolvers), undetected at build time, extinction (or worse,
a wrong-arm dispatch) at runtime.

## Disposition

Fixed: both `_Static_assert`s added to `devsrv.h`, mirroring srvconn's.
The four magics are pairwise distinct; the pins make the offset half of
the scheme build-time-checked.
