---
id: fnd-thread-11c-r2-f2
type: fnd
round: adt-thread-11c-r2
severity: P3
status: fixed
title: "SYS_NOTE_OPEN gave a stale token a taker with no re-read"
surface: [sub-kernel-notes]
threatens: [inv-i19]
fixed-by: chg-2026-11-03-item-11c
regression: "notes.table_token_needs_a_taker; K57 red on it"
created: 2026-11-03
---
## Prosecution

Opening the notes fd makes a stale token live again, and a note that had already been read as dead stayed that way until another post. `notes_mark_self_managing` now re-reads when a table governs, and not otherwise, so a no-table self-managing Proc keeps the path it had.
