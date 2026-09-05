---
id: fnd-16c-r1-f4
type: fnd
title: "kernel_attached set too late (handle-alloc race window)"
round: adt-16c-r1
severity: P2
status: fixed
surface: [sub-kernel-ninep-attach]
threatens: []
fixed-by: chg-2026-05-26-16c-attach-srv
created: 2026-07-31
---
## Prosecution

The flag was set after the blocking handshake; a peer Thread closing the
KOBJ_SRV handle in the window ran srvconn_teardown under the live
handshake.

## Disposition

Fixed: the setter hoisted to immediately after the adapter's init commits
-- before any blocking op. Preserved in today's
`srvconn_attach_dev9p_root` ordering (step 2 of the attach sequence).
