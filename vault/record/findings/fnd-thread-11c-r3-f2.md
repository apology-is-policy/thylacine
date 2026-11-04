---
id: fnd-thread-11c-r3-f2
type: fnd
round: adt-thread-11c-r3
severity: P3
status: fixed
title: "An attach handshake a caught note cancelled reported EIO, so no restart could follow"
surface: [sub-kernel-devsrv]
threatens: [inv-i19]
fixed-by: chg-2026-11-03-item-11c
regression: "none: no witness. It needs a 9P server that answers Rflush to a Tattach through /srv, and Pouch sets OINTR only on the data and listen files of netd"
created: 2026-11-03
---
## Prosecution

The same class as the walk: a wait that unwinds inside a step with no errno channel. `devsrv_open_connect` dropped the handshake's error on the floor and the open read the generic failure. It now carries -T_E_INTR through `open_errno` and leaves every other failure as it was. Unreachable in-tree today.
