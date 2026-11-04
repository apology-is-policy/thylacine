---
id: fnd-walk-errno-r1-f1
type: fnd
round: adt-walk-errno-r1
severity: P2
status: fixed
title: "A union member that could not be asked read as absent"
surface: [sub-kernel-stalk]
threatens: [inv-i9]
fixed-by: chg-2026-11-03-walk-errno
regression: "stalk.union_walk_failure, stalk.create_refuses_on_a_failed_walk"
created: 2026-11-03
---
## Prosecution

A member whose cross or X-stat failed was a silent skip, so with no other member holding the name the failure stayed 0, the name read ENOENT, and open-or-create created the file in the MCREATE member. It contradicted the text this chunk added to ERRORS.md and the tree's own precedent in stalk_union_create_member. Fixed: both skips record the failure through union_walk_failure, reported only when no member holds the name.
