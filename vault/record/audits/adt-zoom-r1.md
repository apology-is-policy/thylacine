---
id: adt-zoom-r1
type: adt
title: "The fullscreen-zoom hunt: the operator's Cmd+F report run to ground (self, static)"
date: 2026-09-05
scope: [sub-tapestryd]
reviewer: self
model-start: claude-fable-5-1
model-end: claude-fable-5-1
verdict: dirty
counts: {p0: 0, p1: 1, p2: 0, p3: 0}
findings: [fnd-zoom-r1-f1]
round-of: chg-2026-09-05-fullscreen-zoom
created: 2026-09-05
---
## Scope

Not a prosecutor round: the coordinator's own hunt (Fable 5.1, static) on the operator's report that Super+F (zoom) of the DOSBox-X pane showed the frame at native size in the display's top-left corner on black, reproduced and handed over by aux (yip 0048; frames `fs-1-tiled.png` / `fs-2-zoomed.png` + `fs-repro-run.log` in aux's scratchpad). Surfaces read: `usr/tapestryd/src/server.rs` (`placement_rect`, `compose_geometry`, `compose_cpu`, `reconcile`'s scanout choice, the present handler's #56 latch, `prefill_from_shown`, `ptr_hit`), `usr/tapestryd/src/pane.rs` (`recompute`'s zoom arm), `usr/ports/sdl2/thylacine/SDL_thylacineevents.c` (the CONFIGURE arm), `thyla_tap.{c,h}` (the single-slot discipline), `usr/lib/libtapestry/src/lib.rs` (the slot rotation + the age model), aux-3's `usr/ports/dosbox-x/patches/0005-thylacine-non-resizable-window.patch` and DOSBox-X's `sdlmain.cpp` present sites.

## Convergence

Two hypotheses were retired by reading before any instrument was built: (1) aux's "the zoomed leaf's content rect reaches `placement_rect` as ~640x417" -- `recompute` sets the zoomed leaf's content to the display and `surface_target` returns it; (2) the pickup's "the SDL backend reweaves the surface to the offered display size" -- the port pins DOSBox-X non-resizable, so the backend DECLINES every offer. The ground truth was in aux's own frame: in the TILE, DOSBox already sat at the pane's top-left (the crop arm), and the log's `letterbox 640x400 -> 632x395 @(0,188)` was an earlier incarnation's one-shot (DOSBox re-creates its window per mode change; three `retire surface 1` lines; the last incarnation is 640x417 with the menu bar and never logged a letterbox). The crop arm's second entrance is the #56 patchwork latch; DOSBox-X presents partial rects (`SDL_UpdateWindowSurfaceRects` for its menu bar and changed scanline bands), so it was latched an accumulator. The single finding is [[fnd-zoom-r1-f1]]; fixed in [[chg-2026-09-05-fullscreen-zoom]]. The class is new: [[haz-latch-keyed-on-proxy]].
