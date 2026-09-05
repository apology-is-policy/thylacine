---
id: fnd-stalk3c-r1-f1
type: fnd
title: "handle.c's KOBJ_SRV release comment cited the deleted connect core; the SrvConn arm went defensively dead"
round: adt-stalk3c-r1
severity: P3
status: fixed
surface: [sub-kernel-devsrv]
threatens: []
fixed-by: chg-2026-06-03-stalk3c-retire
created: 2026-07-31
---
## Prosecution

Post-retirement the only `handle_alloc(KOBJ_SRV)` is
`devsrv_post_listener` (obj always a SrvService); connection endpoints
are KOBJ_SPOOR Spoors released via `spoor_clunk` → `devsrv_close`. The
release branch's comment still cited `srv_conn_open_for_proc` and
described a KObj_Srv obj as possibly a client SrvConn — a maintainer
would reason from a world that no longer exists.

## Disposition

Fixed: comment reworded to the as-built; the `SRV_CONN_MAGIC` arm
RETAINED as a UAF/corruption canary (with the
`else if != SRV_SERVICE_MAGIC → extinction` close) even though it is
structurally dead — tightening it to extinction-on-SrvConn was
considered and declined as a behavior change. The defensive-dead posture
is now a [[sub-kernel-devsrv]] caveat.
