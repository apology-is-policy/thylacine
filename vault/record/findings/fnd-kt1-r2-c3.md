---
id: fnd-kt1-r2-c3
type: fnd
title: "the battery's console-leaf control is vacuous when `foreign_pane` returns None, and has no positive control in its own gate"
round: adt-kt1-r2
severity: P3
status: fixed
surface: [sub-tapestryd]
threatens: []
fixed-by: chg-2026-09-05-kt1-audit-close
regression: "ls-gfx-panes: console leaf tiled / backgrounded by the declaration / restored"
created: 2026-09-05
---
## Prosecution

**File**: usr/tapestry-battery/src/main.rs:573-588 (the leg: `if let Some(cid) = foreign_pane(&layout, &[a.id, b.id]) { ... }` with no else arm), :119-137 (`foreign_pane`), :1190-1206 (the move leg's `console` detection via the DIFFERENT parser `leaf_surface`, :261-268)
**Invariant**: the witness discipline (a negative assertion must be paired with a positive control one variable away; a check that cannot fail proves nothing).
**Prosecution**:
1. The leg proves "an undeclared client leaves the console leaf tiled" by reading `pane/<cid>/geometry` and failing on `cw == 0 || ch == 0` (fail-closed on a parse failure -- good). But `cid` comes from `foreign_pane`, and `if let Some(cid)` skips the whole leg when it is None: a format drift in the `layout` text (`<id>[*] leaf surface=N [..]`) that breaks `foreign_pane` -- it uses `it.next()?` and `t["surface=".len()..]` -- makes the leg print nothing and `structure OK` follows unconditionally (589). ls-gfx-panes asserts only `structure OK` + any `tapestry-battery: FAIL` line (ls-gfx-panes.exp:99-100); it never asserts `battery: console leaf tiled`. The move leg proves the console leaf EXISTS (it fails when `console == None`, 1191-1206) but through `leaf_surface`, so `foreign_pane`'s own parse is proven by nothing.
2. The positive control (a DECLARED client zero-rects the same leaf) lives only in ls-gfx-session -- a different client, a different gate, a different layout. Within the battery, "tiled because undeclared" is indistinguishable from "tiled because backgrounding is absent/broken": the leg cannot fail on a compositor that never backgrounds anything.
**Suggested fix**: (a) make the None arm FAIL when the layout carries a foreign surface (`fresh.lines().any(|l| leaf_surface(l).is_some_and(|n| n != a.id && n != b.id))`), or assert `foreign_pane(..).is_some()` right after the layout dump on the gfx image; (b) the one-variable control: the battery is a Session principal under ls-gfx-panes, so write `session on` on the driver conn, re-read the console geometry (expect `0 0`), write `session off`, re-read (expect the original nonzero rect); (c) have ls-gfx-panes assert the `battery: console leaf tiled` line so a silent skip is visible.

## Disposition

Fixed in the round-2 close: (a) a None from `foreign_pane` while the layout carries a foreign surface FAILS the battery (parser drift, never a skip); (b) the one-variable control -- the battery's own conn writes `session on`, reads the same console leaf at the ZERO rect, writes `session off`, reads its column back; (c) ls-gfx-panes asserts all three lines in sequence. The battery + its scenario remain [[seam-tapestry-battery-unowned]].
