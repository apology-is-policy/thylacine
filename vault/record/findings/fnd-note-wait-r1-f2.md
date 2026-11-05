---
id: fnd-note-wait-r1-f2
type: fnd
round: adt-note-wait-r1
severity: P3
status: fixed
title: "The one-shot leg passes with the kernel reset deleted"
surface: [sub-kernel-notes, sub-pouch-signal]
threatens: []
fixed-by: chg-2026-11-03-note-wait
regression: "pouch-hello-signals one-shot-row; sabotage G04"
created: 2026-11-03
---
## Prosecution

With the native row's reset deleted the dispatcher still resets the table, and a second SIGUSR1 reaches the dispatcher's default arm and dies the same way, so the leg's second half is satisfied by Pouch and not by the kernel.

## Disposition

Fixed before the report returned, by a leg on a default-ignored note: with the row reset the post cannot unwind the read, and with it left caught the read reads EINTR. The reviewer's proposed leg was not needed.
