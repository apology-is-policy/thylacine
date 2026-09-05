---
id: fnd-kt1-r1-c8
type: fnd
title: "the tiling exclusion is one level deep -- a container whose leaves are all backgrounded keeps its full division share (and a Tab container of only backgrounded leaves shows nothing)"
round: adt-kt1-r1
severity: P3
status: fixed
surface: [sub-tapestryd]
threatens: []
fixed-by: chg-2026-09-05-kt1-audit-close
regression: "unconstructed (a Tab container of bg leaves); pane.rs host tests cover the leaf arm"
created: 2026-09-05
---
## Prosecution

**File**: usr/tapestryd/src/pane.rs:1544-1547 (`is_bg_leaf` is leaf-only: "a container's backgrounding is its leaves'" -- but nothing computes it for a container), :1429-1440 (the Split arm's `fg` filter is over direct children), :1393-1414 (the Tab arm: `eff` empty -> `shown` None -> no child laid out)
**Prosecution**: `root = SplitH [S, A(session)]` with `S = SplitV [aurora, sys2]` (two SYSTEM-hosted leaves nested): S is a container so `is_bg_leaf(S)` is false -> S is in `divide` -> half the display; inside S the all-bg guard divides among both -> two visible bg leaves with real rects, `Surface.backgrounded` true, never composed/CONFIGUREd -> half the display is BG_COLOR (paint_chrome 5001-5005) for the whole session. With `S = Tabbed [aurora, sys2]` the Tab arm lays out no child at all (a blank half). Needs two SYSTEM-hosted leaves nested, which only a system client beside aurora produces (unreachable in the shipping boot; reachable with the battery-as-SYSTEM or a system demo). **Fix**: define backgrounding for a container as "all its leaves backgrounded" (a recursive predicate) and use it in the Split/Tab filters, or document the one-level bound.

## Disposition

Fixed in 062efe18: `is_bg_subtree` (a leaf: its flag; a container: non-empty and all children bg) replaces `is_bg_leaf` at every child filter (the Split arm, both Tab arms, `tab_cycle`, `visible_strips`). Unconstructed by a gate (a Tab container of only backgrounded leaves).
