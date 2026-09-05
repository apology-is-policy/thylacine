---
id: fnd-16c-r1-f5
type: fnd
title: "attached_destroy_inner freed the adapter without destroying it"
round: adt-16c-r1
severity: P2
status: fixed
surface: [sub-kernel-ninep-attach]
threatens: []
fixed-by: chg-2026-05-26-16c-attach-srv
created: 2026-07-31
---
## Prosecution

`kfree(adp)` with the magic still pristine: a concurrent observer of the
freed adapter passes the magic check and derefs freed memory.

## Disposition

Fixed: both `p9_spoor_transport_destroy` and `p9_srvconn_transport_destroy`
run before the kfree (each magic-guarded; exactly one matches). The
justification was corrected at R2 (F4R2) and the discipline pinned by
compile-time asserts (F5R2).
