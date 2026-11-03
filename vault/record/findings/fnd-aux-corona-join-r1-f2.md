---
id: fnd-aux-corona-join-r1-f2
type: fnd
round: adt-aux-corona-join-r1
severity: P3
status: fixed
title: "check-lock-helpers missed multi-line calls, aliases, the _raw variants and client locks in other files"
surface: [sub-kernel-poll, sub-kernel-ninep-client]
threatens: [inv-i9]
fixed-by: chg-2026-10-29-aux-corona-join
regression: "check-lock-helpers --self-test plants each shape, with comment and string controls"
created: 2026-11-02
---
## Prosecution

The first form matched `spin_*(&lock` on one line, so a call split over lines, a pointer alias, `spin_unlock_raw`, a raw call past a helper's closing brace and a raw client lock outside `9p_client.c` all passed. Fixed in f3f5b4b7b: comments and literals blanked, calls matched across lines, named locks flagged on any mention.
