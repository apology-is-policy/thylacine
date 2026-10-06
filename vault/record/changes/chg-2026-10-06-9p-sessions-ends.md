---
id: chg-2026-10-06-9p-sessions-ends
type: chg
title: "/ctl/9p-sessions shows a row's counters only to its two ends"
date: 2026-10-06
arc: arc-identity-detour
commits: ["f33605135", "0051c458e"]
touched:
  - sub-kernel-devctl
  - sub-kernel-devsrv
  - sub-kernel-srvconn
  - sub-kernel-ninep-attach
  - sub-kernel-syscall-dispatch
  - sub-imperium
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-06
---
`/ctl/9p-sessions` showed every reader each connection's and session's
per-message counters, and a pty-served terminal carries a message per key, so
another principal could time a secret typed there (the CPU-time gate's audit,
round 1 F4). The operator chose per-row owners
([[dec-2026-10-06-9p-sessions-ends]]; IMPERIUM-DESIGN 11.3 item 10). The first
question misstated the cost of the alternative, so it was asked again.

A row's counters are now shown to the principals at its two ends, the system
principal and a hostowner; everyone else reads `-` ([[sub-kernel-devctl]]). A
connection records the connecting Proc's principal and the poster's by value
([[sub-kernel-srvconn]], [[sub-kernel-devsrv]]). A session records its
attaching Proc and, over `/srv`, the connection's server
([[sub-kernel-ninep-attach]], [[sub-kernel-syscall-dispatch]]). An end the
kernel cannot name matches no reader, and neither does a reader running as
`none`. The trusted episode never crossed this file ([[sub-imperium]]).
