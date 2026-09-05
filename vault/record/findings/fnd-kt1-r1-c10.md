---
id: fnd-kt1-r1-c10
type: fnd
title: "the fg/bg transition witness fires on any hide of the console leaf, not only on 'session gone' -- `Surface.backgrounded` is visibility-derived, so zooming a session tile logs `foreground surface 0 (session gone)` and unzooming logs `background ... (session took the display)`"
round: adt-kt1-r1
severity: P3
status: fixed
surface: [sub-tapestryd]
threatens: []
fixed-by: chg-2026-09-05-kt1-audit-close
regression: "the ls-gfx-session zoom leg (no spurious transition line)"
created: 2026-09-05
---
## Prosecution

**File**: usr/tapestryd/src/server.rs:5814-5850, usr/tapestryd/src/pane.rs:1272-1289 (zoom preempts the walk)
**Prosecution**: a session `zoom <own-leaf>` (allowed: `actor_hosts`) -> only that leaf visible -> `vis` lacks aurora -> `bg_now` empty -> `s.backgrounded != now` -> the "foreground ... (session gone)" line while the session very much holds the display. Every functional reader of `Surface.backgrounded` is gated on `visible_hosted()` (5210-5217, 5304-5308, 6073-6077, 6903-6909), so nothing misbehaves; but ls-gfx-session's final loop keys on exactly this text, and the two flags now disagree about what "backgrounded" means. **Fix**: derive `Surface.backgrounded` from the hosting leaf's tree flag (`is_bg_leaf`) for hosted surfaces, or reword the witness to "hidden"/"session gone" by cause.

## Disposition

Fixed in 062efe18: ONE flag. `Surface.backgrounded` derives from the tree (`bg_now` = the surfaces of `bg_tiling` leaves), so a hidden backgrounded leaf's surface stays backgrounded and the fg/bg witness fires only on a real edge (`session off` prints `session gone`).
