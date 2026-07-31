---
id: fnd-16c-r1-f7
type: fnd
title: "Server-side close tears down even when kernel_attached"
round: adt-16c-r1
severity: P2
status: documented
surface: [sub-kernel-ninep-transport]
threatens: []
created: 2026-07-31
---
## Prosecution

`devsrv_close` + `srv_proc_exit_notify` run srvconn_teardown
unconditionally; a buggy stratumd closing its accept Spoor shoots down
joey's mounted FS.

## Disposition

Defended as INTENTIONAL asymmetry: server-side close IS the legitimate
end-of-connection signal; suppressing it would only delay the failure
surface, strictly worse for debuggability. The immediate FS-unmount
symptom is the right signal for a server bug.
