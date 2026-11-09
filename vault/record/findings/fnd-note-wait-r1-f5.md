---
id: fnd-note-wait-r1-f5
type: fnd
round: adt-note-wait-r1
severity: P3
status: fixed
title: "An in-handler suspend ends on notes no handler takes"
surface: [sub-kernel-notes, sub-pouch-signal]
threatens: []
fixed-by: chg-2026-11-03-note-wait
regression: "notewait.suspend_in_a_handler; sabotage K10"
created: 2026-11-03
---
## Prosecution

A default-ignored note queued while in a handler stays queued and read as admitted, so a loop around sigsuspend spun hot.

## Disposition

Fixed before the report returned, in 057b24967: the scan asks notes_proc_has_live_handler.
