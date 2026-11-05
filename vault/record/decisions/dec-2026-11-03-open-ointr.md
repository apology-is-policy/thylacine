---
id: dec-2026-11-03-open-ointr
type: dec
title: "The one open that waits for a peer says so: SYS_WALK_OPEN_OINTR"
date: 2026-11-03
status: standing
decided-by: user-vote
affects: [sub-kernel-syscall-abi, sub-kernel-notes]
created: 2026-11-03
---
## Fork

A Pouch TCP `accept` waits in the open of netd's `listen`, a `connect` in the open of its `data`. `open` is not on signal(7)'s list, and the kernel cannot tell these from a regular file's open, so a daemon's `SIGTERM` handler would not run until a connection arrived.

Options: an omode bit; netd marks the files and the kernel stats before each open; leave them uninterruptible.

## Decision

Vote 20 of 2026-11-03, as recommended: `SYS_OPEN` gains `SYS_WALK_OPEN_OINTR`, set by Pouch's `accept` and `connect` on that one open. The wait unwinds and restarts under `SA_RESTART`. Regular opens are unchanged and pay no round trip. The stat would have cost every open of every Proc with a table, for two callers.
