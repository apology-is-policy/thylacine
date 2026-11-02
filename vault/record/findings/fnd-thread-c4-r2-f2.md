---
id: fnd-thread-c4-r2-f2
type: fnd
round: adt-thread-c4-r2
severity: P3
status: fixed
title: "The move's unreachable overflow arm returned silently"
surface: [sub-kernel-notes]
threatens: [inv-i19]
fixed-by: chg-2026-11-02-thread-model-c4
regression: "K23's extinction on notes.direct_move_survives_a_full_ring, landing lease"
created: 2026-11-02
---
## Prosecution

`notes_move_to_queue_locked` dropped the note if the bound were ever wrong. Fixed: it extincts, as kill's re-enqueue does. The landing lease proved the line: K23 ended the boot on 'notes_move_to_queue_locked: no spare slot -- N-2 violation'.
