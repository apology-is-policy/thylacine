---
id: fnd-zoom-r1-f1
type: fnd
title: "the #56 patchwork latch keys on damage COVERAGE, a proxy for the property it exists for (slot ROTATION) -- a single-slot SDL client presenting partial rects (DOSBox-X) is latched an accumulator and CROPPED at the content origin instead of letterboxed; zoomed, it shows native at the display's corner on black"
round: adt-zoom-r1
severity: P1
status: fixed
surface: [sub-tapestryd]
threatens: []
hazard: haz-latch-keyed-on-proxy
fixed-by: chg-2026-09-05-fullscreen-zoom
regression: "ls-gfx-panes `singleslot` (the zoomed 2x letterbox line + three pixels + the rotating control that still latches)"
created: 2026-09-05
---
## Prosecution

**File**: usr/tapestryd/src/server.rs (the latch site in the present handler: `if !rects_cover_full(&rects, w, h) { s.patchwork = true }`; `placement_rect`: `if s.patchwork || (s.w == content.w && s.h == content.h) { crop at the content origin }`; `compose_geometry`: the letterbox arm gated on `!patchwork`; the `Surface.patchwork` doc: "A latched surface is an ACCUMULATOR (aurora's cell-diff over rotating weave slots) ... Full-frame presenters (the SDL class, the battery) never latch"), usr/ports/sdl2/thylacine/thyla_tap.h ("Single-slot discipline: ... a synchronous client draws and presents slot 0 only"), aux-3 third_party/dosbox-x/src/gui/sdlmain.cpp (`SDL_UpdateWindowSurfaceRects(sdl.window, &menu.menuBox, 1)`; the overscan's four rects; the changed-lines bands of `OUTPUT_SURFACE_EndUpdate`)
**Invariant**: none of section 28 (placement policy, not authority or lifetime); the fork-2 letterbox contract (a fixed-size client that declines the CONFIGURE offer is aspect-fit into its pane) as stated in the SDL backend's CONFIGURE arm
**Prosecution**:
1. State: DOSBox-X's window is pinned non-resizable on Thylacine (port patch 0005), so every CONFIGURE offer is declined and the surface keeps 640x417 in a 632x772 tile and on the 1280x800 zoomed display. The fork-2 contract says the compositor letterboxes it.
2. DOSBox-X presents partial damage as a matter of course: the menu bar alone, the four overscan border rects, the changed scanline bands. The FIRST such present fails `rects_cover_full` and sets `patchwork = true` (one-way).
3. From then on `placement_rect` / `compose_geometry` / `ptr_hit` take the crop arm: the surface at the content ORIGIN, native size, damage-clipped. In the tile: 640x400 of it at the pane's top-left, the rest of the pane the floor (aux's `fs-1-tiled.png` shows exactly this; the centered `@(0,188)` line in the log was an earlier 640x400 incarnation's one-shot, retired before the latch). Zoomed: native at (0,0) of the display, the rest black (`fs-2-zoomed.png`); no letterbox line is ever printed for that incarnation (aux's "zero letterbox output after Super+F").
4. The latch's own rationale does not cover this client: the crop exists because a ROTATING client's slot is stale outside its damage (the frame from `nslots` presents back), so scaling one slot would compose half-stale frames. thyla_tap presents slot 0 only -- the app's framebuffer, complete by construction -- so its partial damage is a hint, not a patchwork, and the doc's claim that "the SDL class never latches" was an assertion about a class the code never checked.
**Suggested fix**: key the latch on rotation -- latch only when partial damage arrives on a surface that has presented two or more distinct slots (`Surface.slots_presented`, a bitmask that survives reweaves); and since a letterboxed compose then serves partial presents, redraw only the damage's projection through the scale rather than the whole scaled rect per rect (a cursor blink at 70 Hz would otherwise rescale a display-sized rect per blink; DOSBox's four overscan rects would rescale it four times per present).

## Disposition

Fixed in [[chg-2026-09-05-fullscreen-zoom]], both halves. The latch = partial damage AND >=2 distinct slots presented (aurora latches at exactly the same present as before -- its second present is always a second slot; a single-slot client never does); the compositor says `surface N patchwork latched (...)` once when it trips. `ComposeOp.clip` carries the damage's projection (`libhalcyon::place::scaled_clip`, brute-force host-tested against `nearest_src`, the exact mapping `compose_cpu` samples by, so a clipped compose is pixel-identical to a whole one with no seam); the CPU path composes and pushes only the clip; the GPU path keeps its whole-op blit. `letterbox` moved to `libhalcyon::place` so the battery's sample points derive from the compositor's function. Regression: the `singleslot` leg of ls-gfx-panes -- a single-slot client (libtapestry `Surface::set_single_slot`, thyla_tap's discipline) zoomed: the compositor's own `letterbox 640x400 -> 1280x800 @(0,0)` line (the witness aux's log lacked) with the latch line a FAIL arm, the partial present's pixel through the 2x scale, the untouched frame's pixel at three-quarters (black under the bug), a post-zoom partial present's pixel; then the one-variable-away control (rotation on, the second partial present latches at `slot 1 of slots 0b11`). Not yet re-run on real DOSBox-X: the fixture lives on aux-3; owed to aux after the merge (aux's `dx-fullscreen-repro.exp`). FIT vs FILL was not a fork: aspect-fit is the compositor's existing letterbox policy (640x417 -> 1227x800, 26 px pillars).
