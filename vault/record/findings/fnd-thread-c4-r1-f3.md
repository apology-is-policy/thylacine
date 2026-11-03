---
id: fnd-thread-c4-r1-f3
type: fnd
round: adt-thread-c4-r1
severity: P3
status: fixed
title: "Unconsume could put a bit back on a Proc turned self-managing"
surface: [sub-kernel-notes]
threatens: [inv-i19]
fixed-by: chg-2026-11-02-thread-model-c4
regression: "notes.direct_unconsume_after_the_mark_goes_to_the_queue; K24"
created: 2026-11-02
---
## Prosecution

The flag can be set before the tail's hold, with the word's exchange inside it. After `SYS_NOTIFY(0)` nothing consumes the word. Fixed: unconsume's self-managing arm goes to the queue.
