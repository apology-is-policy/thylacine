---
id: fnd-note-wait-r1-f3
type: fnd
round: adt-note-wait-r1
severity: P3
status: fixed
title: "Nothing pins that a handler entered out of a suspend builds on the wait mask"
surface: [sub-kernel-notes, sub-pouch-signal]
threatens: []
fixed-by: chg-2026-11-03-note-wait
regression: "pouch-hello-signals suspend-mask, exit 143; sabotage G11"
created: 2026-11-03
---
## Prosecution

Basing the native handler's mask on the caller's mask rather than the wait's passes every leg, because the legs' two masks agree on what they read.

## Disposition

suspend-mask blocks SIGTERM in the caller and not in the wait, and asserts what the handler read.
