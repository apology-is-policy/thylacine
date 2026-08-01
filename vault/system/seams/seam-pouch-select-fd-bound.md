---
id: seam-pouch-select-fd-bound
type: seam
title: "`select`/`pselect` reject valid fds >= 64 (stale PROC_HANDLE_MAX)"
status: open
surface: [sub-pouch-net]
opened-by: chg-2026-06-24-355-poll-decouple
tracker: "#355 companion"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

`select()` bounds fd VALUES at `POUCH_POLL_NFDS_MAX` = 64 and returns
`-EBADF` for any fd >= 64 set in an input set, commented as "unreachable
through any Thylacine syscall -- PROC_HANDLE_MAX". That was true when
`PROC_HANDLE_MAX` was 64. Since #355 the per-Proc fd table is **256**,
and only the `SYS_POLL` *nfds count* is bounded at `POLL_MAX_NFDS` = 64
-- deliberately, as a stack-frame bound ([[seam-poll-heap-waiters]]).

So a pouch program holding fds >= 64 has valid fds wrongly `EBADF`'d by
`select()` and `pselect()`. `poll()` is unaffected: it passes fd values
through and bounds only the count, matching the kernel.

Latent -- no in-tree pouch consumer holds more than 64 fds -- but
reachable by any ported program with a large fd population, and it fails
in the worst direction (a correct call rejected, not an incorrect one
accepted).

## The lift

Cap the fd VALUE at `FD_SETSIZE` and the COUNT at 64 (or chunk the
poll). Sweep the stale `PROC_HANDLE_MAX = 64` name in the 0005 / 0015 /
0018 comments to `POLL_MAX_NFDS` while there -- name-stale, value-correct
in the count sense, and the confusion is what let the fd-value bound
survive the table growth.
