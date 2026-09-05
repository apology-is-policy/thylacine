---
id: fnd-sockets12-r1-f2
type: fnd
round: adt-sockets12-r1
severity: P1
status: fixed
title: "A tombstone-then-rebind with a mode change could land a wrong-mode SrvConn in the new poster's backlog"
surface: [sub-pouch-net]
threatens: []
fixed-by: chg-2026-05-23-p6-sockets
regression: "`srv_client.byte_mode_mode_change_rebind_refused`"
created: 2026-08-01
---
## Prosecution

`srv_conn_open_for_proc` captures the service's mode under the registry
lock at the LIVE check, then later pushes the minted SrvConn into the
backlog under the lock again — without re-validating that the LIVE state
still carries the captured mode. If the service tombstoned and rebound
with a DIFFERENT mode in between, a wrong-mode conn lands in the new
poster's backlog: a 9P server would receive a raw byte stream, or the
reverse.

## Fix

`srv_reserve` refuses a rebind of a TOMBSTONED entry whose mode differs
from the new one. Simpler than per-push re-validation, and it formalizes
what the design already intended: the mode is part of a service's
IDENTITY, so a different mode requires a different name — immutable
across LIVE -> TOMBSTONED -> LIVE.
