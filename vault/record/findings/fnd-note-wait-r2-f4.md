---
id: fnd-note-wait-r2-f4
type: fnd
round: adt-note-wait-r2
severity: P3
status: fixed
title: "In a handler the poll form cannot end for a note, and ARCH called that the same as a suspend's"
surface: [sub-kernel-notes]
threatens: []
fixed-by: chg-2026-11-04-poll-mask
regression: "none: stated"
created: 2026-11-04
---
## Prosecution

A suspend in a handler ends at a note its mask admits. The poll form sleeps until a ready fd or its deadline. After a longjmp out of a Pouch handler every masked poll on that thread is deaf to notes until the longjmp item lands.

## Disposition

Stated precisely in ARCH 8.8.3, syscall.h and the dossier. Nesting is the real fix.
