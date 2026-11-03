---
id: fnd-thread-usr-r1-f3
type: fnd
round: adt-thread-usr-r1
severity: P3
status: fixed
title: "Stale widths in the dossier's caveat, ARCH 7.6.5 and a comment"
surface: [sub-kernel-notes]
threatens: []
fixed-by: chg-2026-11-02-usr-notes
regression: "none: documentation"
created: 2026-11-02
---
## Prosecution

The caveat read 0x7f and 0x2f in the present tense (now 0x3ff and 0x3af). ARCH 7.6.5 had the usr rows naming `SYS_POSTNOTE_TID` while `term`'s did not, for the same mechanism; one sentence now names all six families. `notes_release_claims`' comment said two bytes.
