---
id: dec-2026-11-04-query-stops-at-its-limit
type: dec
title: "The account query stops at the nearest limit, per class"
date: 2026-11-04
status: standing
decided-by: user-vote
affects: [sub-kernel-accounts, abi-memory-account]
created: 2026-11-04
---
## Fork

Vote 59 said a sandbox that should see less is given its own child, and then sees only itself ([[dec-2026-11-04-account-control-sandbox-sak-rerun]]). MM-1c's first audit found the code did not do that: `account_query_fill` walked to the root and read the pool, so a child with no limit saw other sessions' pages move in `headroom` and `limiting`, and a tight limit hid them only while its room was below the pool's.

## Research

cgroup v2's `memory.max` and `memory.current` report no machine figure, and `/proc/meminfo`'s leak is the known container channel lxcfs exists to close. Fuchsia gates `ZX_INFO_KMEM_STATS` behind a resource handle, which vote 59 already refused. Plan 9's `/dev/swap` is global.

## Options

Stop at the nearest limit per class, as cgroup v2 reads; or keep the walk and correct the sentence to "a child with a limit tight enough, until the machine is nearly full".

## The call

Vote 65 of 2026-11-04, the recommended option: stop at its own limit. The query reads no further up than the nearest limit, per class. A sandbox whose child has a limit sees only its own use and limit, never the pool or its ancestors; an unlimited child still reads the chain. Accepted knowingly: `headroom` can overstate the room on a nearly full machine, and the refusal at allocation is what tells the truth. The counter behind account incarnations stays a small machine-wide channel either way.

## Rationale

The sentence vote 59 rested on becomes true, and a program can still ask its budget. As written, "per class" means a limit in one class stops that class only, and device storage has no pool, so it needs no limit before the pool goes unread. MM-2's refusal has to read every ancestor and the pool regardless.
