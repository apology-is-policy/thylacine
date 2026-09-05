---
id: fnd-kt1-r1-c11
type: fnd
title: "the console renderer's DISPLAY-level chrome is not backgrounded with it -- a registered status bar forces the session Composed and stays painted over it; a menu placed at handoff keeps the input grab"
round: adt-kt1-r1
severity: P3
status: deferred
surface: [sub-tapestryd]
threatens: []
regression: "seam-console-chrome-on-handoff"
seam: seam-console-chrome-on-handoff
created: 2026-09-05
---
## Prosecution

**File**: usr/tapestryd/src/server.rs:5905-5918 (both Direct arms require `self.status.is_none()` / `self.menu.is_none()`), :7002-7010 (the grab)
**Prosecution**: only when the retired halcyond-console lever is the renderer (14.12 retires it; the code path stands): its `Role::Status` bar is `Comp.status`, keyed on the display, not on a leaf -> the session's layout is carved (`layout_h = dh - sr.h`, 5768-5771) and never Direct; a `menu place`d at the moment the session leaf appears keeps `Comp.menu` -> every key routes to the dormant console renderer's menu until Esc/chord. Not reachable with aurora (no chrome). **Fix**: on a bg transition of the renderer's content leaf, dismiss its menu and treat its status bar as hidden (or document the aurora-only assumption in 14.12).

## Disposition

Deferred: aurora registers no display-level chrome, so the hole is unreachable on the shipping image; a renderer that does (the retired halcyond-console lever) must dismiss its status bar / menu on the handoff. Owed at [[seam-console-chrome-on-handoff]].
