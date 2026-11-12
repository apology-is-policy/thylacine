# Handoff 051 -- the trusted screen at its display's scale: what was changed in Lictor while you were away (to Astra)

**From**: aux, 2026-11-12. **To**: Astra, for when you are back.

Handoff 050 ended with two things the trusted screen did not do: follow a resize, and know a
display scale. This is the second, done under the operator's word of 2026-11-11 ("Astra is on
vacation until further notice. Feel free to change Lictor."). Your worktree and anything you had
not committed were not touched. Everything below is yours to change or take out; the reasons are
here so that you can judge them without me. The first, following a resize, is not done.

## Why

The layout was in device pixels. On a display the session runs at 200%, which is every Mac this
is being made to run on, the screen that asks for the imperium key had type half the session's
physical size.

## What changed in Lictor

- **`render.rs`: one layout, drawn at one or two device pixels to a layout pixel.** Your layout
  is untouched: every number in `draw` is the one you wrote, now read as layout pixels. `Canvas`
  gained `k`; `rect`, `outline`, `text` and `fasces` multiply, and `fill` is the one thing in
  device pixels. `render(pixels, width, height, k, backdrop, model, masked)` has the new
  argument. `fits_at(w, h, k)` is `fits` and the floor in layout pixels; `scale_at(w, h, pct)`
  is the nearest whole scale the screen fits at, two from 150 percent. The layout's own test
  draws the screen at two with the type as it is at one, doubled, and compares it with the
  screen at one exactly; its switch (`coarse`, on `Canvas` and on `draw`) is compiled for tests
  only, and no build of the service has it.
- **`typography.rs`, `type2.bin`, `tools/bake-lictor.py`: the type is baked at both scales.**
  The set at two has the box, the baseline, the bearing room and the advance of the set at one,
  times two, and only the outline is finer. So `width` and `wrap` did not change, the words are
  on the same lines, and your layout tests hold for both scales as they stand. `type.bin` is the
  same bytes as before (the unchanged baker reproduced it first). `glyph(font, k, ch)` has the
  new argument. `type_metrics.rs` carries each set's hash and a test recomputes it; `.gitignore`
  exempts both sets. The baker ran in a private Python environment with fontTools 4.68.0 from
  PyPI, outside the tree.
- **`render::geometry_for` replaces `Screen::size_for`.** It gives the screen's size (as 050
  described) and its scale together, and it is pure and host-tested: compiled for the backend
  and for tests, which made libhalcyon a dev-dependency of the crate as well as the backend's
  optional one. The scale's source is `libhalcyon::display::own_pct`: the declared
  `thylacine.scale` first, then the EDID's millimetres, carried to the screen's own pixels when
  the screen is not its display's size.
- **`backend/screen.rs`: `declared_scale()` reads `/hw/chosen/bootargs`**, as tapestryd's
  function of the same name does, and `Screen::new(gpu, declared)` takes it; `Screen` keeps the
  scale and `paint` hands it to `render`. The line it says is now `lictor: trusted screen WxH at
  Kx for a display of WxH`, when the size is not the display's or the scale is not one. Nothing
  in `tools/` matched the old wording.

## Three decisions that are yours and the operator's to change

They are recorded in `docs/GRAPHICAL-SAK-OWNERSHIP.md`, "The trusted screen's scale", as made by
me under the operator's standing authorization of 2026-09-01, not as voted: whole scales with
the type baked at each (no rasteriser and no resampling on the trusted path); the scale's source
is the platform's declaration and then the EDID, read by Lictor, and never anything a client
says; two from 150 percent.

## What it does not do

- The size and the scale are still the boot's. A window resized later, or moved to a display of
  another scale, leaves the trusted screen as it was made. Doing that means making the raster,
  the backing and the resource again outside an episode, and `Trusted raster memory` in the same
  document says why that is a decision and not a patch.
- A scale between one and two is drawn at the nearer whole one. There is no scale of three.
- `backend/screen.rs` is in no host build, so `declared_scale`, the four values `Screen::new`
  hands `geometry_for` and `paint` handing the scale to `render` are a guest's to show:
  `tools/interactive/ls-graphical-sak-scale.exp`.
- From a display's millimetres two begins at a true 137.5 percent, because the session's rule
  rounds to quarters before this one rounds to a whole scale. An EDID that has its aspect ratio
  where its size goes is read as saying nothing (`libhalcyon::scale::parse_edid_mm`, changed
  with this), so such a display is drawn at one unless the platform declares.

## Where the evidence is

The Lictor dossier's section "The trusted screen at its display's scale (2026-11-12)" has the
mechanism, the tests by name and what they do not show; `docs/JOURNAL.md` has the run, with the
two tests that failed first and what they taught (the set at one is sampled four times a pixel
each way, so the two sets agree in their faces' means and not pixel for pixel).

The change had an audit round (Opus 5.5 reviewing; no P0, P1 or P2, seven P3). What it changed
is in this note as it now reads: the second scale `draw` used to take became the test-only
switch, the full-screen test got a bound of its own for the type, the sets got their hashes, and
the guest scenario was written. One finding is about the session and not this screen, and is
open: tapestryd does not carry a declared scale through a mode it fitted to half its host, so at
such a size the session is at twice the scale this screen is drawn at, and this screen's reading
is the right one.
