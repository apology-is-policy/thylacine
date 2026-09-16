---
id: sub-cartoon
type: sub
title: "cartoon -- the display list and its dumb, fully-clamped CPU executor"
parent: moc-userspace-runtime
code:
  - usr/lib/cartoon/src/lib.rs
  - usr/lib/cartoon/Cargo.toml
audit: light
guarded-by: []
validated-by: [prose]
locks: []
hazards: []
abis: []
design: ["docs/HALCYON.md section 13.2", "docs/TAPESTRY.md section 14"]
created: 2026-09-05
updated: 2026-09-16
---
## Purpose

A tapestry cartoon is the full-size design a weaver executes, and that is
exactly this crate's role in the Halcyon architecture (HALCYON.md 13.1--13.2):
halcyond -- the only place that *thinks* -- draws a display list, and a dumb
executor weaves it into pixels. The CPU executor here is the universal floor,
running wherever aurora runs; the vk executor (H-6, unbuilt) will execute the
*same* op set via the serialized wire form this data is already shaped for.

It exists to draw a hard line between deciding what to paint and painting it.
Everything that requires knowledge -- shaping text, measuring runs, computing
damage, choosing a theme -- happens in the author; the executor only fills
rects, blits pre-resolved glyph alphas, and composites blobs. That division is
what lets the same list run on a CPU today and a GPU later without the author
changing, and what keeps the executor small enough to trust.

## Contract

`Cartoon` is the list: `ops` in paint order plus a flat `runs` pool the glyph
ops index. `execute(cart, atlas, blobs, px, w, clip)` paints it into a pixel
buffer of stride `w`. `AtlasStore` + `AtlasPacker` manage the 8-bit alpha
pages and the shelf packing; `BlobStore` holds decoded rasters; `blend` is the
shared src-over. The author builds a `Cartoon` (`push_glyphs` appends a run and
its op together), hands it to `execute` with the store it was authored
against, and `reset`s it for the next frame keeping both allocations.

## Mechanism

**The executor is fully clamped -- no op can write outside the pixel buffer,
whatever the list says.** Coordinates are signed (scrolled content
legitimately starts above or left of the viewport), and every write is
intersected at execute time: the effective clip is the surface bounds
intersected with the caller's clip, `isect` uses `saturating_add` so a huge
`w`/`h` cannot overflow, and `fill`/`blit_alpha` early-out on an empty rect.
An empty buffer or a zero stride returns immediately. This is the memory-safety
property the whole "dumb executor" premise rests on: the author is trusted to
be correct, but the executor is written so that even a wrong list is only
wrong-looking, never out-of-bounds.

**A glow's radius is clamped in the EXECUTOR, not merely where the op is
built.** `Op::Glow` paints `color` at `alpha` under the rect's coverage run
through a separable box blur, and the paint reaches `radius` pixels past the
rect on every side -- so the radius is a *work* bound, not a cosmetic knob,
and `GLOW_RADIUS_MAX` (32) is applied on the READ side, where a list the
executor did not author arrives. It clamps DOWN rather than skipping, which
matches the executor's geometric discipline everywhere else: an oversize
`Rect` is clipped, not dropped. The 32 is section 10's 16-at-100 % doubled,
because the display scale tops out at 200 % (`libhalcyon::scale`); cartoon
carries zero dependencies, so that derivation is written out here instead of
imported, and the test asserts the 32 absolutely -- a bound asserted in terms
of its own constant moves when the constant does.

**The blurred coverage needs no mask buffer.** A rect's indicator function is
separable and so is a box blur, so the coverage at a pixel is exactly the
product of a horizontal and a vertical 1-D window overlap: two O(1) counts
per pixel, exact rather than approximate, no allocation. One consequence is
worth stating because it reads as a shortfall and is not: a rect smaller than
`2*radius+1` on an axis never reaches full coverage anywhere, since no pixel
ever sees a full window.

**`Op::Blur` is the one op that READS the surface it paints into**, and that
is precisely why it exists: `Glow` blurs a rect's coverage MASK -- which is
what a drop shadow is -- and cannot express a blur of whatever happens to lie
underneath, which is what section 10's modal backdrop needs. The alternative
was a second blur inside the compositor, the shape that produced three
HALCYON-WORKSPACES defects.

**The destination blur is ALLOCATION-FREE, and that is a soundness property
rather than a frugality.** A separable in-place blur normally wants a scratch
of the region's area -- megabytes for a full-display region -- but only the
`r + 1` values already OVERWRITTEN need keeping, since everything at or ahead
of the write cursor is still original in `px`. A fixed ring of
`GLOW_RADIUS_MAX + 1` entries therefore serves any permitted radius. cartoon
is `no_std`, where a failed allocation aborts, and an executor whose contract
is "always produces a validly-clamped frame" must not be able to fail. The
ring cannot be clobbered under its own reader: slot `k % (r + 1)` is rewritten
at step `k + r + 1`, strictly past every step that still needs it, because a
window at `i` reaches back only to `i - r`.

**The window RUNS (2026-09-16), so a pixel costs the same at any radius.**
`blur_line` keeps four channel sums over the current window: step `i` gains
the value entering at `i + r` -- still original, being ahead of the cursor --
and loses the one leaving at `i - 1 - r`, which is already overwritten and so
comes from the ring. That leaving value sits in slot `(i - 1 - r) % (r + 1)`,
which is `i % (r + 1)`: EXACTLY the slot step `i` is about to write. So the
subtraction reads it BEFORE the write, and that order is the whole of the
ring's correctness argument for the running form. The output is identical to
the per-tap sum it replaced -- same window, same clipped edges, same floor
division -- and that is pinned, not asserted: the old sum is kept VERBATIM in
the tests as `reference_blur_line`, and
`the_running_window_matches_the_per_tap_sum_everywhere` compares the two on
216 random fields covering every window class (`n` below, at and past `2r +
1`; radii past the cap; the column pass's stride; a non-zero base). Why it
changed: the modal backdrop became display-sized (section 10 as reversed at
`c065ec06`), and on a 2560x1664 field at the release profile the per-tap sum
took 111 / 144 / 182 ms at radius 3 / 6 / 8 against 41 / 32 / 29 ms running
-- the difference between a menu that opens and one that stalls.

**A blur of a PIECE of a field is exact inside the piece, given a clip grown
by the radius** -- the property the compositor's upload-time effects rest on
(HALCYON-INSTRUMENT section 10 as revised 2026-09-16: tapestryd lays a
dialog's backdrop over only the pixels one upload carries, never over a
stored scene). A pixel's window reaches at most `r` either way; the vertical
pass reads horizontal results no further than `r` above or below, each of
which read no further than `r` across; so every tap lies inside the grown clip,
and where that clip meets the field's (or the op rect's) own edge, both runs
clip the window identically. Grown by the CLAMPED radius, because that is how
far the executor actually reads. Witnessed, not argued:
`a_blur_clipped_to_the_grown_target_is_exact_inside_the_target` runs 400
random fields with op rects overhanging the field, targets at every edge and
radii past the cap, comparing more than 10 000 pixels against the unclipped
blur -- and a control that the UNGROWN clip is inexact at its edge, so the
equality is a property of the growth rather than of blurring at all. Growing
the clip one pixel short fails it (sabotage-measured).

**`blur_line` clamps the radius itself, and the clamp lives there and nowhere
else.** `keep` is sized from `GLOW_RADIUS_MAX`, so an over-large radius would
index past the ring -- the bound is MEMORY SAFETY here, not the work bound it
is for `Glow`. Clamping in the caller as well was rejected on a testability
argument, not a stylistic one: two redundant guards mask each other's
sabotage, so neither can be shown to be load-bearing.

**The divisor is the tap count actually taken.** A window hanging off the
rect's edge averages fewer taps rather than averaging in black, so a constant
field is preserved EXACTLY, edges and corners included -- the property that
keeps a backdrop from ringing darker around its own border. Section 10 says
"downsampled"; this is a direct box blur, because downsampling is a
large-radius GPU optimisation and at the backdrop's 3 px (6 at the 200 %
scale ceiling) a direct blur is both cheaper and exact. The specified
appearance is the blur, not the means.

**The atlas generation makes a stale page reference impossible by
construction** (the 13.2 stale rule). A `Glyphs` op carries the `atlas_gen` it
was authored against; the executor paints it only when that equals the store's
current `gen`, and skips the whole run otherwise -- the author redraws next
frame against the new generation. So a glyph op can never index a page that has
been repacked out from under it; the mismatch is caught before a single blit.

**The executor never measures text.** Glyph runs arrive resolved: each
`GlyphRef` is an atlas index plus the pen advance to the next glyph, with
kerning already applied by the author's shaper. The executor blits left to
right from the baseline, advancing the pen after each blit, and -- crucially --
advances the pen even when a glyph or page lookup misses, so a bad index
degrades to a gap rather than a desynced run.

**The executor does not diff.** Damage is the author's job (`present_rects`);
the `clip` here is an execution bound, not a diff hint. `Embed` paints nothing
in v0 -- it reserves flow space for a compositor-placed inline surface
(TAPESTRY 14 inline-live), and the author paints any placeholder ground
beneath it with `Rect` first.

**The blend is the shift form, and that is load-bearing.** `blend(bg, fg, a)`
packs the red/blue lanes into one word and uses `na = 256 - a` with a `>> 8`,
which distributes correctly across packed lanes; the fully-opaque and
fully-transparent cases short-circuit. This is the exact shape [[sub-aurora]]
records as a scar -- an earlier divide-based blend of a packed word gave
antialiased edge pixels a garbage-blue correlated with red, and the bug lived
precisely where the short-circuits did not reach.

## Data structures

`Op` is the drawing op (Clear / Rect / RectAlpha / Glyphs / Image / Embed /
Glow / Blur) with surface-local signed coordinates. `GlyphRef` is one glyph's atlas index +
advance. `Cartoon` is `ops` + the flat `runs` pool -- flat because it keeps
the in-process form allocation-light and is already the shape the H-6 wire
form serializes. `AtlasPage` is a w-tight 8-bit alpha page; `GlyphEntry` is a
glyph's blit rect on its page plus its FreeType-convention bearing
(`left`/`top`, the blit origin being `(pen + left, baseline - top)`).
`AtlasStore` bundles the pages + glyph table + the `gen`; `AtlasPacker`/`Shelf`
is the shelf packer. `BlobStore` holds decoded images; `ClipRect` is a
half-open pixel bound.

## Concurrency

None. A pure `no_std` + `alloc` library with zero dependencies; the author
drives it single-threaded and owns every structure.

## Invariants enforced

None of the numbered system invariants -- no syscall, no capability, no
handle. Its own load-bearing rules:

- **No op writes outside the pixel buffer**, whatever the list contains -- the
  clamp is the executor's whole safety story.
- **A glyph op paints only against the atlas generation it was authored for**,
  so a repacked page can never be misread.
- **A glow's blur radius is bounded at the executor** (`GLOW_RADIUS_MAX`),
  because the spread past the rect is work that the list -- not the executor
  -- chooses. The SAME constant bounds `Op::Blur`, where it is load-bearing
  for a second reason: the allocation-free ring is sized from it.
- **The executor cannot fail.** No op allocates, so no op can abort a
  `no_std` compositor mid-frame.
- **The executor stays knowledge-free** (no shaping, no measuring, no diff);
  the moment it needs to *decide* something, the division of knowledge has
  been violated and the vk executor could not mirror it.

## Error paths

Everything degrades to a gap or a skip; nothing faults. A zero stride or empty
buffer returns. A degenerate clip returns. An out-of-range glyph or page index
skips that glyph (advancing the pen). An `atlas_gen` mismatch skips the run. A
run slice past the pool end is clamped. There is no fallible return -- the
executor's contract is that it always produces a validly-clamped frame.

## Performance

Per-op, per-pixel within the clip. `Op::Blur` is O(1) per pixel per pass at
any radius since the running window (measured above: a full 2560x1664 field
in 29-41 ms on the host, flat in the radius). The blend short-circuits the opaque and
transparent cases (the common ones for fills and glyph interiors), so only
antialiased edges pay the packed-lane arithmetic. The flat run pool avoids a
per-run allocation. Damage-bounding is the author's job via `clip`; the
executor honours it but does not compute it.

## Prosecution

- **Every write must stay clamped.** `isect`'s `saturating_add`, the
  effective-clip intersection, and the empty-rect early-outs are the guard; a
  refactor that lets an op rect reach `px` without passing `isect` is an
  out-of-bounds write from an author bug.
- **The `atlas_gen` equality check must gate every glyph blit.** Removing it
  lets a `Glyphs` op index a repacked page -- a stale-reference read the rule
  exists to make impossible.
- **The pen must advance on a missed glyph/page lookup.** Skipping the advance
  desyncs the rest of the run's positions.
- **The blend must stay the shift form.** A divide over the packed word
  reintroduces the [[sub-aurora]] edge-colour corruption.
- **The glow radius must be clamped where the list is READ.** Bounding it only
  at the author leaves the executor honouring whatever the next author -- or
  the H-6 wire -- hands it. A bound that does not hold on the read side is not
  a bound.
- **`blur_line`'s clamp must not be duplicated into its caller.** It is the
  ring's bounds check; a second copy upstream would mask its sabotage and
  leave the real guard unwitnessed.
- **`blur_line` must read the leaving value before it writes the ring.** Both
  live in slot `i % (r + 1)`; writing first subtracts the pixel ENTERING the
  window instead of the one leaving it. Sabotage-measured 2026-09-16: that
  swap fails the oracle comparison and `a_blur_averages_its_neighbourhood`.
  (A subtraction guard moved from `i > r` to `i >= r` does NOT fail, and must
  not be mistaken for a missing witness: at `i == r` the slot it reads has not
  been written in this pass and is zero, so the extra subtraction is a no-op.)
- **`Op::Blur` must keep normalising by the taps actually taken.** Dividing by
  the full `2r+1` window instead darkens every edge toward black, which shows
  as a ring around the backdrop's own border.
- **The executor must not grow knowledge.** Any text measurement, damage
  computation, or theme decision belongs in the author; adding it here breaks
  the CPU/vk equivalence the op set is shaped for.

## Seams

- `Embed` is a v0 no-op (flow-space reservation only); actual inline-surface
  placement is the compositor's (TAPESTRY 14).
- The vk executor (H-6) is unbuilt; the wire encoding (little-endian,
  length-prefixed, carrying `CARTOON_V0`) is designed for but not yet emitted
  -- which is why `CARTOON_V0` stays 0 while the op set grows. A version
  discriminates serialized streams, and there are none to tell apart: no v0
  stream can exist that predates a variant. The first encoder to ship freezes
  the number; growth after that bumps it.
- Sub-pixel positioning is not modelled -- glyph advances and blit origins are
  integer pixels.

## Caveats

- **Host-tested** (`cargo test -p cartoon --target aarch64-apple-darwin`): the
  clamping, the atlas-generation gate, the packer, and the blend are exercised
  on the host, which is the point of the pure-crate shape (the vt/beacon
  pattern).
- **The author's correctness is assumed, not checked.** The executor is
  memory-safe against any list, but a list that paints the wrong thing --
  wrong colour, wrong baseline, a glyph run that does not match what was shaped
  -- is halcyond's bug, invisible here. cartoon guarantees safety, not
  fidelity; fidelity is proven where the author is.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
