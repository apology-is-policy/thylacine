---
id: chg-2026-11-03-vote51-audit-close
type: chg
title: "Vote 51's audit: a private session's tags are paid for at CONNECT"
date: 2026-11-03
arc: arc-halcyon-interaction
commits: ["*(pending)*"]
touched: [sub-kernel-loom, sub-kernel-ninep-session, sub-kernel-syscall-dispatch]
established: []
closed: []
opened: []
mirrors-checked: [abi-memory-account]
depth: skeletal
created: 2026-11-03
---
The round on [[chg-2026-11-03-fid-pays-for-clunk]] found the READY scan growing the 9P tag
table under the ring's lock, uncharged. A private session now pays for one tag per admitted
request at CONNECT and cannot grow. A release gives a page back on the exact incarnation
alone, with a leg that fails without it. Small register records use the stack, so REAP and
ABORT are not refused for memory. The account oracle now holds all nine reasons.
