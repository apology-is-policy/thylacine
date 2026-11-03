---
id: dec-2026-11-03-memory-account-abi
type: dec
title: "Memory accounts: one operation on a typed handle, and a voucher bound to a connection"
date: 2026-11-03
status: standing
decided-by: user-vote
affects: [sub-kernel-retention]
created: 2026-11-03
---
## Fork

The interface for memory accounts: one operation on a typed handle with versioned records; a file tree, which is Plan 9's idiom and Linux cgroup v2's shape; or a syscall number per operation. And how a voucher reaches the server that spends it: bound to one /srv connection with an id sent in band, or a transferred handle, which needs `handle_transfer_via_9p` built first.

## Decision

Votes 47 and 48 of 2026-11-03: `SYS_ACCOUNT(handle, op, rec_va, rec_size)` on a pollable `KOBJ_ACCOUNT` or a self selector, a read-only `/proc/<pid>/account`, and a voucher bound to one connection. `docs/SHARED-MEMORY-ACCOUNTING.md` section 13 holds the layout. No number is taken until the layout is reserved in code.
