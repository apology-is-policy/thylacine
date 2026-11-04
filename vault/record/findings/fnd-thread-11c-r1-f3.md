---
id: fnd-thread-11c-r1-f3
type: fnd
round: adt-thread-11c-r1
severity: P3
status: fixed
title: "A handler that longjmps out left a Pouch program spinning on -1"
surface: [sub-kernel-notes, sub-pouch-signal]
threatens: [inv-i19]
fixed-by: chg-2026-11-03-item-11c
regression: "pouch-hello-signals leg escape; G13 red on it"
created: 2026-11-03
---
## Prosecution

A `siglongjmp` out of a handler never reaches `SYS_NOTED`, so `in_handler` stays set. A default-row fatal note then arms the latch, every wait returns at once, and the tail's guard sits above the terminate scan: the Proc never dies and spins. `thread_note_handler_escaped` stays Linux-only, because a native runtime may run its handler on another stack. The guard now asks the terminate scan and ends the Proc. The residue, that no later note reaches a handler in that thread, predates this work and is in OPEN-BUGS.
