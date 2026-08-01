---
id: fnd-signals13b-r1-f10
type: fnd
round: adt-signals13b-r1
severity: P3
status: fixed
title: "__restore_rt fell off the end of .text if SYS_NOTED ever returned"
surface: [sub-pouch-signal]
threatens: []
fixed-by: chg-2026-05-24-p6-signals-b
created: 2026-08-01
---
## Prosecution

The rewritten asm restorer is `mov x8,#46; mov x0,#0; svc 0` with nothing
after it. `SYS_NOTED(NCONT)` never returns on success — but on a -1
(called outside a handler) execution falls off the end into whatever
bytes follow in `.text`.

The stub is unreferenced at v1.0 (pouch's `sigaction` never installs
`sa_restorer`), so this is defense-in-depth on a defense-in-depth path.

## Fix

`b .` after the `svc` — trap rather than wander.
