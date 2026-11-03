---
id: chg-2026-11-03-memory-account-number
type: chg
title: "The memory account call moves to 140"
date: 2026-11-03
arc: arc-halcyon-interaction
commits: ["*(pending)*"]
touched: [sub-kernel-syscall-abi, sub-kernel-syscall-dispatch, sub-kernel-vivarium]
established: []
closed: []
opened: []
mirrors-checked: [abi-memory-account]
depth: skeletal
created: 2026-11-03
---
Corrects [[chg-2026-11-03-memory-account-reservation]], which took 139. Votes 47 and 48 set
the call's shape and no number, and 139 belongs to SYS_NOTE_DISPOSE on aux-3 by its own
vote, so SYS_ACCOUNT takes 140 and aux's next call 141 (yip call 0279). Linux's 140 is
setpriority with no row, argued per number; 139 is empty on this tree and the gap pin holds
it until aux-3 merges. Nothing behaves differently, and no consumer exists.
