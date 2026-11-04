---
id: fnd-thread-11c-r1-f5
type: fnd
round: adt-thread-11c-r1
severity: P3
status: fixed
title: "A Pouch program with no handler filled its ring with dead notes"
surface: [sub-kernel-notes, sub-pouch-signal]
threatens: []
fixed-by: chg-2026-11-03-item-11c
regression: "pouch-hello-signals leg ring-room; P01 red on it"
created: 2026-11-03
---
## Prosecution

The dispatcher used to consume `child_exit`. With no handler it stays queued in the no-table arm, coalescing only past 12, which leaves 4 slots for anything else. The reviewer proposed dropping it at the tail, which would change every native program. Pouch's constructor writes `child_exit`'s default row instead: no table is allocated, but one governs from image start, so the tail's discard runs and there is no window where a note reaches a catch-all. Governance from birth does not run `native_note_class`, so a program that only ignores `SIGPIPE` pays no stat per file.
