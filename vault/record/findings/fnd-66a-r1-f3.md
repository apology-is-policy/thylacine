---
id: fnd-66a-r1-f3
type: fnd
title: "#66a F3: scripture named phantom helpers (the pre-rename names)"
round: adt-66a-r1
severity: P3
status: fixed
surface: [sub-kernel-path]
threatens: []
fixed-by: chg-2026-06-12-66a-spoor-path
created: 2026-08-01
---
## Prosecution

Scripture and comments referenced `spoor_path_walked` /
`spoor_path_crossed` — design-draft names; the as-built helpers are
`spoor_path_extend` / `spoor_path_transplant`. The RW-10 phantom-name
class: a maintainer greps the documented symbol and finds nothing.

## Disposition

Fixed: renamed across ARCH §25.4 + CLAUDE.md + spoor.c + path.h. The
class lesson (docs must name AS-BUILT symbols; symbol citations over
file:line is the vault's R-rule for the same reason) is why dossiers
cite symbols.
