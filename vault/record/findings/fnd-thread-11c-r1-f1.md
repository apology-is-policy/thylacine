---
id: fnd-thread-11c-r1-f1
type: fnd
round: adt-thread-11c-r1
severity: P2
status: fixed
title: "A caught row outlived its taker across a fork, and the note stranded"
surface: [sub-kernel-notes, sub-kernel-proc, sub-pouch-signal]
threatens: [inv-i19]
fixed-by: chg-2026-11-03-item-11c
regression: "notes.table_token_needs_a_taker and rendez.tail_discards_dead_note; K50-K53 red on them"
created: 2026-11-03
---
## Prosecution

The write-time gate is all that tied a caught row to a handler or a notes fd. A fork breaks it: the table and `PROC_FLAG_NOTE_TABLE` cross while `PROC_FLAG_SELF_MANAGING_NOTES` does not, and three separate loads in rfork could tear against a peer's first `sigaction`. `notes_proc_has_live_handler` then said caught from the token alone, so no terminate latch armed, the terminate scan skipped the note, the tail left it queued and `SIGTERM` and `^C` could not end the child. `notes_row_has_taker` now answers in both places, and a stale token takes the discard, terminate and stop arms. rfork takes one flag load, then the handler. Pouch's two flags are gone: it calls `SYS_NOTIFY` on every real install. The rfork tear has no deterministic test.
