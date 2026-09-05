---
id: fnd-kt1-r1-c13
type: fnd
title: "a zero-rect backgrounded leaf is 'showable' to `surface_target`, so a backgrounded client's presents count as animation pressure and pay the GPU transfer"
round: adt-kt1-r1
severity: P3
status: fixed
surface: [sub-tapestryd]
threatens: []
fixed-by: chg-2026-09-05-kt1-audit-close
regression: "none (read in source; the present path of a bg client is unconstructed)"
created: 2026-09-05
---
## Prosecution

**File**: usr/tapestryd/src/server.rs:3885-3890 (`surface_target` returns `Some(p.content)` for a visible leaf even when content is ZERO), :6853-6862 (note_present), :3851-3853 (compose_visible)
**Prosecution**: `if !p.visible { return None; } Some(p.content)` -- a bg leaf is visible with `content == Rect::ZERO`; `note_present`'s `self.surface_target(n).is_some()` is true, so a bg client presenting unpaced holds the #164 clock; `compose_visible` is true so the GPU path transfers before `compose_geometry` returns None (4114-4116 guards the zero content, so no pixel work). Inert for aurora (dormant: it only renders after an event, aurora/main.rs:684) -- live for any SYSTEM client that presents on its own clock. **Fix**: `if !p.visible || p.content.is_empty() { return None; }`.

## Disposition

Fixed in 062efe18: `surface_target` returns None for a leaf whose content rect is empty, so a backgrounded client's presents are neither animation pressure nor a GPU transfer.
