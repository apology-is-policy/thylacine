---
id: fnd-note-wait-r1-f1
type: fnd
round: adt-note-wait-r1
severity: P3
status: fixed
title: "A one-shot reset can erase a concurrent re-install"
surface: [sub-kernel-notes, sub-pouch-signal]
threatens: []
fixed-by: chg-2026-11-03-note-wait
regression: "none: a window of a few hundred instructions with no deterministic witness; the store under the lock is read, not run"
created: 2026-11-03
---
## Prosecution

Thread A's tail reads a row caught with RESETHAND under q->lock. Peer B's SYS_NOTE_DISPOSE stored its row with no lock, so A's reset could land on it, and the program held a default row after B's sigaction returned 0. In Pouch the dispatcher's compare cannot tell the handler the reset was decided for from a newer one.

## Disposition

The kernel half is fixed: SYS_NOTE_DISPOSE and rt_sigaction store under q->lock. The reviewer also proposed writing Pouch's table after the dispose; not taken, because a caught row whose table entry is not yet written sends the note to the dispatcher's default arm, which is the death the ordering exists to prevent. Reading sa_flags in the dispatcher is not taken either: its read set stays {sa_handler}. So the Pouch residue stays, stated in sub-pouch-signal's Concurrency section: a sigaction racing a one-shot delivery.
