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
updated: 2026-09-05
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

`Op` is the drawing op (Clear / Rect / Glyphs / Image / Embed) with
surface-local signed coordinates. `GlyphRef` is one glyph's atlas index +
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

Per-op, per-pixel within the clip. The blend short-circuits the opaque and
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
- **The executor must not grow knowledge.** Any text measurement, damage
  computation, or theme decision belongs in the author; adding it here breaks
  the CPU/vk equivalence the op set is shaped for.

## Seams

- `Embed` is a v0 no-op (flow-space reservation only); actual inline-surface
  placement is the compositor's (TAPESTRY 14).
- The vk executor (H-6) is unbuilt; the wire encoding (little-endian,
  length-prefixed, carrying `CARTOON_V0`) is designed for but not yet emitted.
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
