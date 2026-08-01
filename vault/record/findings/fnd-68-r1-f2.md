---
id: fnd-68-r1-f2
type: fnd
title: "The close-time Tclunk is never sent — a server-side fid leak per fd on every Go exit"
round: adt-68-r1
severity: P2
status: fixed
surface: [sub-kernel-death]
threatens: []
fixed-by: chg-2026-07-14-68-last-thread-out-close
regression: "shares the F1 fix; no direct assert"
created: 2026-08-01
---
## Prosecution

The same root as [[fnd-68-r1-f1]], on a different consequence. With the
closer reading as dying, `dev9p_close`'s Tclunk is refused by the 9P client's
self-dying send gate, so the server's fid is never released.

Every Go binary exits multi-thread through `SYS_EXIT_GROUP`, so this leaked
one server-side fid per open dev9p fd on every Go process exit — a
monotonically growing resource cost on exactly the workload the project runs
most.

## Disposition

FIXED by the same `exit_close_active` flag. Recorded separately because the
FAILURE MODES differ: F1 is data loss, this is resource exhaustion, and a
future change that partially restores the dying-read would likely reopen one
without the other.
