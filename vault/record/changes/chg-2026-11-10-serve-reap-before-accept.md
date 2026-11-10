---
id: chg-2026-11-10-serve-reap-before-accept
type: chg
title: "tapestryd and nocturned service the connections that hung up before they accept, so a slot just left is not refused"
date: 2026-11-10
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-tapestryd
  - sub-nocturned
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-10
---
Found on the Pi by vote 81's C-6 leg (P image at f1ea9b958): `tapestryd: warp connect refused -- the pool is full (8 of 8, 3 warp; 1 refused so far)`, and the DESTROY arm's open of `/srv/warp` answered -5 after 4 ms, right after the CREATE arm had closed its own connection. The serve pass accepted (refusing at a full pool) before it serviced the connections that had hung up, so the slot the client had just left still counted. Both daemons with the accept-and-close refusal had that order; netd and diorama gate the accept on room, so there the connect waits a pass and is not refused. Now the pass services first. The witness is the leg's REOPEN arm (fill the pool, then close one connection and open another, 32 times, none refused), with an accept-first mutant that must go red. A second cause of the same red was the witness program's own: its BO `map` fds were never closed, and an open fd keeps the session, so each arm that minted a BO left tapestryd's conn standing; the arms now close them with their connection.
