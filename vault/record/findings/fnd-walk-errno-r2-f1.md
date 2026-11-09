---
id: fnd-walk-errno-r2-f1
type: fnd
round: adt-walk-errno-r2
severity: P3
status: documented
title: "The union readdir dedup reads a member that could not answer as not holding the name"
surface: [sub-kernel-stalk]
threatens: []
regression: "stalk.union_walk_failure"
created: 2026-11-03
---
## Prosecution

`stalk_union_has_child` is the one union path left on the raw vtable. If an earlier member's lookup of a name fails, a later member's same-named entry is listed beside it: the name shows twice. No wrong create or unlink follows, since both ask `stalk_union_member_holding`, which records the failure.

Closed with a stated reason, not changed: emitting is the safe answer. Suppressing would lose a name outright when the earlier member's own readdir failed too, and a duplicate in a listing beside a member that cannot answer is the truthful reading of what is known. The function's comment and the stalk dossier say so, and `stalk.union_walk_failure` now asserts it.
