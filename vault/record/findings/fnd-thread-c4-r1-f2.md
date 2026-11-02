---
id: fnd-thread-c4-r1-f2
type: fnd
round: adt-thread-c4-r1
severity: P3
status: fixed
title: "A note moved on a full ring was dropped after its poster was told 0"
surface: [sub-kernel-notes]
threatens: [inv-i19]
fixed-by: chg-2026-11-02-thread-model-c4
regression: "notes.direct_move_survives_a_full_ring; K23, K28, K32"
created: 2026-11-02
---
## Prosecution

`notes_mark_self_managing` discarded `notes_post`'s -EAGAIN, so a full ring plus a moved word lost the note. Fixed: `NOTE_RING_SPARE` slots past the admission depth and `notes_move_to_queue_locked`.
