---
id: fnd-pouchb0-r3-f1
type: fnd
title: "the new ppoll prover leg is decided by a scheduling race, and its usual green is the kernel calling the client's OWN unread request 'readable'"
round: adt-pouchb0-r3
severity: P1
status: fixed
surface: [sub-pouch-net]
threatens: [inv-i9]
fixed-by: chg-2026-09-21-srvconn-two-endpoint-poll
regression: "pouch-hello-sockets legs `ppoll` (barrier-sequenced: the client polls only after the server consumed its request; EXACTLY POLLIN) and `ppoll-eof` (EXACTLY POLLIN|POLLHUP, then EOF); joey matches the census string from usr/pouch-hello/pouch-census.h"
created: 2026-09-21
---
## Prosecution

**File**: `usr/pouch-hello/pouch-hello-sockets.c` (the ppoll block in `stdio_over_socket`); `kernel/devsrv.c` `devsrv_poll`; `kernel/srvconn.c` `srvconn_poll`
**Invariant**: a gate leg must be decided by the system under test, not by the scheduler
**Prosecution**:
1. The leg: client `fputs` + `fflush` (7 bytes into c2s), then `ppoll(POLLIN, 10 s)`.
2. `devsrv_poll` calls `srvconn_poll` regardless of `CSRVCLIENT`, and `srvconn_poll` is server-endpoint: `if (cn->c2s.count > 0) revents |= POLLIN`. `srvconn_server_send` wakes no poller.
3. Schedule A (client samples before the server thread consumes): `c2s.count == 7` -> POLLIN -> PASS, with s2c EMPTY.
4. Schedule B (server consumed first): sample 0 -> parks -> the server's reply wakes nobody -> `close(conn)` -> teardown wake -> `POLLHUP|POLLERR` (0x18), no POLLIN -> `_exit(1)` -> boot-fatal.
5. It passed the one HVF boot that gated it. Three same-day sentences were false: 0039's header "one that poll()s works", sub-pouch-net "pins both halves", the closed list's r2-F2.
**Suggested fix**: make the leg deterministic and honest -- sequence it with barriers so it is RED on today's kernel naming the kernel defect and GREEN on a fixed one.

## Disposition

Fixed, and the gate measured it before the report did: under TCG the server consumes first about 98 % of the time, under HVF usually not. The leg is sequenced by barriers, requires exactly `POLLIN`, and has a second leg for the EOF shape; both sides `_exit(1)` on failure so a bailed thread cannot strand the other at a barrier. The false sentences were rewritten to say what was claimed, what was true, and what is true now. The underlying kernel defect is [[fnd-pouchb0-r3-f2]].
