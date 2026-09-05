---
id: fnd-kt1-r1-c4
type: fnd
title: "a session that goes from 2 tiles to 1 never returns to Direct/borderless -- the lone tile keeps a chrome ring + tag bar and composes, because the zero-rect console leaf counts toward `visible_leaf_count() > 1`"
round: adt-kt1-r1
severity: P2
status: fixed
surface: [sub-tapestryd]
threatens: []
fixed-by: chg-2026-09-05-kt1-audit-close
regression: "ls-gfx-session lone-tile leg"
created: 2026-09-05
---
## Prosecution

**File**: usr/tapestryd/src/pane.rs:1307-1312, usr/tapestryd/src/server.rs:6072-6081 (the structural fan offers the inset content), :5922 (Direct needs the surface display-sized)
**Invariant**: pane.rs:23-25 "the Daylight chrome ring per pane iff more than one pane is visible (the single-fullscreen root leaf keeps the stage-0 borderless look)"; HALCYON.md 14.12 step 4 "the root collapses to the session (Direct)".
**Prosecution**:
1. State after ls-gfx-session's d-3 leg: `root = SplitH [aurora(bg), A, B]` (Composed, A acked 632x772). B exits -> `close` -> `root = SplitH [aurora(bg), A]`.
2. `visible_leaf_count()` = 2 (aurora zero-rect visible) -> inset -> A.content = 1272x772; the structural fan `emit_configure_to(A, 1272, 772)` (6078); halcyond acks (`handle_configure`, session.rs:482) -> A = 1272x772.
3. reconcile: `active_vis.len()==1 && active_nleaves==1` but `full` is false (1272x772 != 1280x800) -> `Scanout::Composed` (5925-5926). The single session tile is drawn with a bevel ring + a 20px header strip on the composed screen, forever (nothing ever re-offers display size).
4. Observes: the stage-0 borderless rule and the "root collapses to the session (Direct)" claim hold only for the FIRST tile (whose surface happened to be minted display-sized). Same root cause as F3; the exp's `session-tiling` witness ignores `active<2` lines, so the 1-tile geometry is unasserted.
**Suggested fix**: the F3 fix (a) -- exclude backgrounded/zero-rect leaves from the inset count; then the lone tile's content is the display, the fan offers 1280x800, and the Direct predicate holds. Assert `session-tiling active=1 min_w=<disp_w>` after the first close in ls-gfx-session.

## Disposition

Fixed in 062efe18 (the C-F3 fix): the zero-rect backgrounded leaf no longer counts toward the inset -- a session going from 2 tiles to 1 returns to a borderless, Direct-eligible tile.
