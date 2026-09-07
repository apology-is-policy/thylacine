---
id: chg-2026-09-05-h4c-layout-gesture
type: chg
title: "H-4c: the layout gesture rides the verb menu (obj type=layout), halcyon layout list/delete, and the session startup script (halcyon.rc or the device default) run by the per-user compositor"
date: 2026-09-05
arc: arc-tapestry
commits: ["26f903a0"]
touched: [sub-beacon]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
no-dossier-change: "sub-beacon's delta is one enum variant (ObjType::Layout -> `type=layout`) plus three rules in verbs.default; the vault peer folds it from this record (yip 0052's inheritance pattern). halcyon (the tool) and halcyond are unowned: docs/reference/151-libhalcyon.md + 150-halcyond.md carry the prose."
created: 2026-09-05
---
The H-4 arc's third chunk (HALCYON.md 13.7 AS-BUILT; design memory design-h4-layouts). The gesture needed no renderer code: `halcyon layout list` presents each saved layout as `obj type=layout` (BEACON.md 12.2, a version note: a value of an existing key, handled by string in halcyond) and `/lib/beacon/verbs` offers `restore` / `save` / `delete` on it. The tool gains `list` (both tiers, rich table or plain lines, shadowed device rows marked) and `delete` (session tier only), and a name never begins with `-` or ends in the save's `.tmp`. The startup script: `halcyond --session` runs `$home/lib/halcyon.rc` under `ut --home` once after its first present, else `halcyon layout restore default` when the image ships one (the H-4d welcome's slot), else nothing -- rio's `-i initcmd`, the compositor being the user's rio since KT-1; the child runs as the user under the tile cap mask, stdin /dev/null, is reaped with a bounded idle poll, killed at logout (`halcyond::session_init`, host-tested). Gates: ls-gfx-restore's list/delete legs; ls-gfx-session's rc leg (a saved two-tile layout + the rc, logout, re-login: the init line, `restored 0 of 0`, three tiles, the exit line). Found while writing that leg and OWED at H-4d: the session tool and the compositor's empty-leaf tile spawn race for TAGGED leaves (same principal, last claim wins) -- the compositor must leave a tagged empty leaf to whoever tagged it. Unaudited by the double-the-distance rule; the prosecution notes ride AUDIT-TRIGGERS' H-4b row.
