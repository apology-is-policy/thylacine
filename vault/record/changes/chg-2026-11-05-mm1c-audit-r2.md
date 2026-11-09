---
id: chg-2026-11-05-mm1c-audit-r2
type: chg
title: "MM-1c audit r2's close: a class's limit is its nearest limit"
date: 2026-11-05
arc: arc-halcyon-interaction
commits: []
touched: [abi-memory-account, sub-kernel-accounts]
established: []
closed: []
opened: []
mirrors-checked: [kernel/include/thylacine/account.h, kernel/include/thylacine/syscall.h, usr/lib/libt/include/thyla/account.h, usr/lib/libt/include/thyla/syscall.h, usr/lib/libthyla-rs/src/account.rs, usr/lib/libthyla-rs/src/lib.rs]
depth: skeletal
created: 2026-11-05
---
MM-1c's second audit round, after its first boot. Vote 65 says a sandbox whose child has a limit never sees the pool. `headroom` obeyed that and the record's `limit` did not: a class with no limit of its own read the pool even below a limited parent, so a sandbox could read the machine's figure by making a child, and a program asking its budget read a ceiling that did not bound it. A class's `limit` is now its nearest limit, the account's own or an ancestor's, and the pool only when it meets none. The layout does not change; the value of one field does, before this has landed anywhere. Two clauses of the vote had no reading that could fail, and now have them. capacity-probe's tails leg now checks that the cwd and environment records arrived, and says what its child reported.
