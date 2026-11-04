---
id: chg-2026-11-04-query-stops-at-its-limit
type: chg
title: "The account query stops at the nearest limit"
date: 2026-11-04
arc: arc-halcyon-interaction
commits: []
touched: [abi-memory-account, sub-kernel-accounts]
established: []
closed: []
opened: []
mirrors-checked: [kernel/include/thylacine/account.h, kernel/include/thylacine/syscall.h, usr/lib/libt/include/thyla/account.h, usr/lib/libt/include/thyla/syscall.h, usr/lib/libthyla-rs/src/account.rs, usr/lib/libthyla-rs/src/lib.rs]
depth: skeletal
created: 2026-11-04
---
MM-1c audit r1's open finding, answered by vote 65 ([[dec-2026-11-04-query-stops-at-its-limit]]). `account_query_fill` reads each class up to its nearest limit and no further, and the pool only for a RAM class that met none. No record's shape changes, so the six mirrors stand as they are; what `headroom` means changes in the contract and the note. Kernel test `account.query_stops_at_the_nearest_limit` and two legs in `capacity-probe`. Written, not booted ([[chg-2026-11-04-mm1c-audit-r1]]).
