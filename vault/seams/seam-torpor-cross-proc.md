---
id: seam-torpor-cross-proc
type: seam
title: "No futex between Procs sharing a page at their own VAs"
status: open
surface: [sub-kernel-torpor]
opened-by: chg-2026-05-23-torpor
tracker: "POUCH-DESIGN section 7, Tier-2 burrows"
created: 2026-08-01
updated: 2026-11-03
---
## Owed

The bucket key is `(AddrSpace *, VA)`. Procs sharing one space wake each other
(thread-model C8). Two spaces mapping one page (a Weft ring, a weave share, a future
shared Tier-2 burrow) cannot futex each other through it — a WAIT in one Proc is invisible to a
WAKE in the other even on the same physical word.

## The lift

The Linux split: a private key (as today) plus a shared key derived
from the backing object + offset (here: the Burrow identity + page
offset — the #847 object is the natural key). Becomes real work the
first time a cross-Proc lock wants to live in a shared page rather
than be brokered by a server.
