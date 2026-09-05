---
id: chg-2026-09-05-beacon-layout-fold
type: chg
title: "The H-4c layout gesture folded into sub-beacon: ObjType::Layout (type=layout) + the three verbs.default layout rules"
date: 2026-09-05
arc: arc-vault
commits: ["b5a65d18"]
touched:
  - sub-beacon
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-05
---
main's H-4c layout gesture (`26f903a0`, [[chg-2026-09-05-h4c-layout-gesture]])
touched sub-beacon; its chg carried `no-dossier-change` naming the exact delta
and deferring the prose to the vault peer (the KT-1 inheritance pattern). This is
that fold, verified against the landed code -- not a re-derivation.

## The delta (as main's record specified it)

"One enum variant plus three rules in verbs.default." Both verified:

- **`ObjType::Layout`** (sink.rs 83, rendering `"layout"` at 94) -- the sixth
  obj type after Path/Pid/Url/Commit/User. Unlike the others it is canonically
  named by the saved layout's NAME rather than a path/id. Folded into the Data
  structures ObjType list.
- **The three `verbs.default` layout rules** (verbs.default 28-30):
  `layout restore|save|delete  halcyon layout restore|save|delete {}`. So
  `halcyon layout list` presents each saved layout as an `obj type=layout` the
  transcript menu offers restore/save/delete on -- with NO renderer code, because
  `layout` is a new value of the existing `type` key (BEACON.md 12.2), handled by
  string in halcyond, not a new frame op. Folded into the verbs Mechanism section.

## Not a re-derivation

The gesture's narrative (the tool's list/delete, the session startup script, the
tagged-leaf claim race found while writing its gate) is main's record's, and its
audit rides AUDIT-TRIGGERS' H-4b row per the double-the-distance rule. beacon is
`audit: light` (it carries no capability; the H-3c menu's hard gate lives in
[[sub-tapestryd]]), so this fold adds a data row and its verbs, nothing
authority-bearing. halcyon (the tool) and halcyond are unowned -- their prose is
docs/reference/151-libhalcyon.md + 150-halcyond.md, per main's record.
