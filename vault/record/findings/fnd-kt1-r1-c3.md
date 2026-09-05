---
id: fnd-kt1-r1-c3
type: fnd
title: "Direct scanout is chosen for a single foreground tile whose layout content rect is INSET (the zero-rect backgrounded leaf still counts as a visible leaf) -- pointer routing maps through a placement the display does not use (introduced at d-1b, milder after F2)"
round: adt-kt1-r1
severity: P1
status: fixed
surface: [sub-tapestryd]
threatens: []
fixed-by: chg-2026-09-05-kt1-audit-close
regression: "ls-gfx-session lone-tile leg (active=1 min_w=disp_w sum_w=disp_w); pointer arm unconstructed"
created: 2026-09-05
---
## Prosecution

**File**: usr/tapestryd/src/pane.rs:1244-1258 (visible_leaf_count), :1307-1312 (the inset decision), :1441-1445 (the ZERO-rect leaf is `visible = true`), :1436-1438 (`p.content = c` inset), usr/tapestryd/src/server.rs:5914-5927 (the Direct predicate), :7085-7111 (ptr_hit), :429-442 (surface_at)
**Invariant**: "THE ONE GEOMETRY AUTHORITY: blit_composed_pixels' forward map and ptr_hit's inverse both derive from this, so they cannot drift apart" (7050-7054) -- the inverse now derives from a placement the Direct scanout ignores; TAPESTRY pointer routing D5 (surface-relative coordinates must be exact).
**Prosecution**:
1. State: `root = SplitH [aurora_leaf(bg), S]` with S display-sized (the session bootstrap `fullscreen_on`, or any user-run fullscreen SDL app in the DEFAULT boot, see F6). `layout_pane` gives aurora_leaf `Rect::ZERO` but sets `p.visible = true` (1364-1365 via 1443).
2. `recompute` pass 2: `let inset = if self.visible_leaf_count() > 1 { gaps + chrome } else { 0 };` (1308-1312) -- the zero-rect leaf counts, so `inset = 4`; S gets `content = (4, 4+20, 1272, 772)` (1330-1346, the tag bar carved too).
3. reconcile: `active_vis.len() == 1 && active_nleaves == 1` (5914-5915: `nleaves(2) - bg_now.len()(1)`), `full = s.w == dw && s.h == dh` (5922) -- TRUE, the surface is 1280x800 -> `Scanout::Direct(S)`. The display shows S at (0,0,1280,800); the layout says (4,24,1272,772).
4. Pointer: `ptr_move` -> `ptr_commit` -> `ptr_route` -> `ptr_hit` -> `surface_at` (content-rect hit test, 429-442) -> `letterbox(1280, 800, 1272, 772)` -> `(ox=18, oy=0, dw2=1235, dh2=772)`; `sx = ((px-4).saturating_sub(18)) * 1280 / 1235`, `sy = (py-24) * 800 / 772` (7102-7109). Display (640,400) -> surface (640,389); display y=24 -> surface y=0 (true: 24); the top 24 rows and the left 4 columns hit nothing (no PTR_MOVE at all). Click-to-focus (`ptr_btn` 7273-7285) uses the same hit test.
5. At d-1b (9a4b9c0b) the same predicate ran with aurora holding a REAL half-width column, so the Direct surface's content rect was the right half -- the mismatch was a factor of 2 in x. F2 narrowed it to the inset. The pre-existing invariant that made Direct exact -- one visible leaf => no inset => content == display -- is what `active_nleaves` decoupled from `visible_leaf_count`. (The status-bar case was recognized at H-3d and both Direct arms were conjoined on `status.is_none()` for precisely this reason: "a leaf above the carve is smaller than the display", 5902-5904; the backgrounded-leaf case reproduces that hazard and was not conjoined.)
6. Observes: absolute-pointer clients (SDL tablet, any GUI) under Direct get coordinates off by up to the ring+tag bar (~24 px vertically) and a dead band at the top/left. ls-gfx-session drives keys only; no gate drives the pointer in a bg configuration.
**Suggested fix**: (a) make the inset decision count only leaves with a non-empty rect (or `!p.backgrounded`), so a lone foreground leaf is borderless and its content == display; (b) belt-and-braces, make the Direct arm require `active_vis[0].2 == Rect{0,0,dw,dh}` (content, not just surface size) exactly as the status-bar conjunct does. Witness: a battery leg that probes `ptr_move` at a known display point under Direct-with-bg and asserts the surface-relative event equals the display point.

## Disposition

Fixed in 062efe18 with C-F4: `foreground_leaf_count` (visible AND not backgrounded) drives the inset decision, and the Direct predicate additionally requires the lone foreground leaf's CONTENT rect to equal the display rect -- Direct is never chosen against an inset layout. Proven by geometry (the lone-tile leg reads a full-width content rect); the pointer-routing arm under Direct-with-a-backgrounded-leaf stays unconstructed.
