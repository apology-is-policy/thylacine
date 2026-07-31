---
id: fnd-16c-r2-f3
type: fnd
title: "Transport header described the pre-fix deadline story"
round: adt-16c-r2
severity: P3
status: fixed
surface: [sub-kernel-ninep-transport]
threatens: []
fixed-by: chg-2026-05-26-16c-attach-srv
created: 2026-07-31
---
## Prosecution

The header claimed dev9p call sites set per-op deadlines; the R1-F2
auto-arm was the actual mechanism.

## Disposition

Fixed: header rewritten. (Rewritten AGAIN at #841 when the mechanism
inverted -- doc-currency on this block has now been wrong in both
directions, which is exactly why the deadline story lives in the dossier's
Caveats as a history note.)
