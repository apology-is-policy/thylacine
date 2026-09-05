---
id: chg-2026-09-05-h4d3-welcome
type: chg
title: "H-4d-3: the first-launch welcome -- the baked device default layout (tour LEFT, the session's shell RIGHT + focused), `halcyon welcome` (a Beacon tour, then the user's shell exec'd in place), the session gate's first-login witness"
date: 2026-09-05
arc: arc-tapestry
commits: ["4f4e7a9f"]
touched: [sub-substrate-build, sub-halcyond]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
no-dossier-change: "sub-substrate-build's delta is one populate step under the session lever: `/lib/halcyon/layouts/default` (mkdir + write + sync + readback cmp, the /lib/beacon/verbs shape) -- no target, ledger or guard change, so chg-2026-08-15-build-targets' co-update set is unaffected. sub-halcyond: none in code (the compositor hosts the tag by H-4d-1's rules); 150-halcyond.md 'The welcome' carries the prose. usr/halcyon (unowned) gains `Cmd::Welcome` + `welcome()`; 151-libhalcyon.md carries it. tools/interactive/ls-gfx-session.exp (unowned; pinned by abi-boot-banner as a banner consumer) gains the welcome + menu legs and re-derives its tile counts; no banner string is touched, so the pin's co-update set is unaffected."
created: 2026-09-05
---
The image ships `/lib/halcyon/layouts/default` = `splith n=2 active=1 / leaf tag="halcyon welcome" / leaf env`; a first login with no rc restores it (H-4c's init): the tool tags the left leaf and exits, the compositor hosts `halcyon welcome` there, the anchor rules put the tour LEFT of the session's own shell and hand it the focus. `halcyon welcome` is a Beacon transcript at the tile's tier (a heading, two lines of how, a "try this" table of PATH objects whose verb menus do the demonstrating, the chords, the lineage line), says `halcyon: welcome shown (tier rich|none)` on the serial console, then `t_execve`s `/bin/ut --home <HOME>` in place (the pts, the pgrp and the pid stay the tile's). Delta from the ratified pitch (HALCYON 13.7): the tour's objects run IN PLACE, not in the right pane -- a cross-tile command channel is v1.x. The gate's first run lost all three attempts to arm order (the init line landed before the ingest witness and was consumed there); the second run folded every cross-process marker into one alternation and saw the ingest witness arrive AFTER the init's exit -- the order is unknowable. Unaudited by the double-the-distance rule (the H-arc round; AUDIT-TRIGGERS row 142).
