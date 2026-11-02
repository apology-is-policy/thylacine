---
id: fnd-thread-c4-r1-f5
type: fnd
round: adt-thread-c4-r1
severity: P3
status: fixed
title: "The self-managing fallback recorded sender 0"
surface: [sub-kernel-notes]
threatens: []
fixed-by: chg-2026-11-02-thread-model-c4
regression: "notes.direct_fallback_records_its_sender; K27, K29, K30, K31"
created: 2026-11-02
---
## Prosecution

`proc_note_thread`'s fallback posted with sender NULL. Fixed: self arms pass the Proc and the walk passes its caller.
