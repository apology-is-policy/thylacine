---
id: fnd-c9-r1-f5
type: fnd
round: adt-c9-r1
severity: P3
status: fixed
title: "`sub-kernel-thread` had its date moved and nothing else"
surface: [sub-kernel-thread]
threatens: []
fixed-by: chg-2026-11-04-c9-r1
regression: "none: a dossier"
created: 2026-11-04
---
## Prosecution

The commit listed the dossier as touched and only bumped `updated:`. Its body said nothing of V state.

## Disposition

It now says what the zero ASID field means to the switch.
