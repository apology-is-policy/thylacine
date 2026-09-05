---
id: sub-cornucopia
type: sub
title: "cornucopia — five baked atlases, and the box characters deliberately left out"
parent: moc-userspace-runtime
code:
  - usr/lib/cornucopia/src/lib.rs
  - usr/lib/cornucopia/Cargo.toml
audit: none
guarded-by: []
validated-by: [prose]
locks: []
hazards: []
abis: []
design: ["docs/AURORA.md"]
created: 2026-08-04
updated: 2026-08-04
---
## Purpose

The system typeface, already rasterized. One outline source is baked at
build time into fixed-cell alpha bitmaps at five sizes and compiled in, so
the renderer needs no outline rasterizer at runtime and no font file at
boot.

That is the whole trade, and it is the right one for a console: a
rasterizer is a large dependency with a large input surface, and a console
needs exactly one face at a handful of sizes. It also means the pre-login
screen has a font before any filesystem is mounted.

## Contract

`Atlas::for_advance(n)` returns a view over the blob baked at cell advance
`n`; an unknown advance falls back to the default face rather than
failing. `verify()` checks one blob's integrity, `verify_all()` checks
every baked size. `cell_w()`, `cell_h()` and `baseline()` give the
geometry; `glyph(ch)` returns that character's alpha coverage, or nothing
if it is not baked.

`Atlas` is `Copy` — a fat pointer — which is why the renderer can carry it
alongside the cell dimensions and swap both together on a size change.

**Box-drawing and block-element characters are deliberately absent**, and
the reason is precise: a font's box glyphs are fitted to *its* line box,
not to the renderer's cell, so they do not join across cell boundaries.
The renderer draws them procedurally instead, and gets pixel-exact joins.

## Mechanism

**Five sizes, largest first, from the same outline.** The advance list is
ordered so the settings overlay walks a clean progression and the renderer
can step *down* to the largest size that fits a small display — which is
what makes a persisted font size unable to strand the console.

**A binary search over a sorted codepoint table**, with the record table's
extent validated at startup rather than trusted per lookup. `verify()`
checks the magic, the version, that the record table actually fits the
blob, and that the cell geometry is sane — because a truncated blob with
an intact header would otherwise pass and then read past the end on the
first glyph.

**Verification covers every size, not just the one in use.** A re-bake
that half-wrote a sibling blob must surface at startup, not on the first
font-size change — which is a different failure at a much worse moment.

**The lookup still fails soft at the last step**: a record whose offset
plus length exceeds the blob returns nothing rather than slicing, so even
a blob that passed verification cannot produce an out-of-bounds read.

## Data structures

One struct: a static byte slice. Everything else is computed by reading
fixed offsets — a sixteen-byte header (magic, version, cell width, cell
height, baseline, glyph count) followed by eight-byte records of codepoint
and offset, then the alpha planes.

The blobs are `include_bytes!` statics, so they live in the binary's
read-only data and cost nothing at startup.

## Concurrency

None. Every function is pure over its arguments and the compiled-in
statics are immutable. An `Atlas` is `Copy` and shareable by value.

## Invariants enforced

None. This is a data-decoding library beneath every boundary — no
syscalls, no allocation, no state.

Its one safety-relevant property is that a corrupt or truncated blob
cannot produce an out-of-bounds read, and that is defended twice: the
startup verification rejects the blob, and the lookup bounds-checks
anyway.

## Error paths

Options and booleans, no error type. An unknown advance falls back, a
failed verification returns false, an unbaked character returns nothing.

The fallback direction is deliberately forward-compatible in both
directions: an old binary tolerates a configuration naming a future size,
and a new binary tolerates one naming a dropped size. Neither panics.

## Performance

A logarithmic lookup over roughly two hundred records per glyph, against a
compiled-in table. The renderer's cost is the alpha blend, not this.

## Prosecution

- **The parser and the bake tool must stay in lockstep.** The layout is
  fixed little-endian and unversioned beyond a single version word; this
  file and the tool are the two halves.
- **`verify()` must keep checking the record-table extent**, not just the
  header. A header-intact truncation is the case it exists for.
- **The legibility floor is real.** The procedural box characters need a
  minimum cell size, so a smaller bake would render them broken — the
  floor is asserted, not assumed.

## Seams

One typeface, one weight. No bold or italic bake — the renderer promotes
bold to the bright colour tier instead, which is the classic terminal
convention.

The bake covers roughly two hundred glyphs: Latin, the punctuation and the
symbols the tree's own output uses. Anything else renders as a hollow box.

The header anticipates a second consumer — the kernel's crash-time and
trusted-path renderers sharing the same bake through a C emission — which
does not exist yet.

## Caveats

- **Its tests run, and it is the only crate in this batch that can say
  so.** It gates the no-standard-library attribute on not-under-test, so
  `cargo test` builds it for the host. Its own header names the
  counter-example: "unlike aurora's no_std+aarch64 modules these run".
  That is the refactor [[sub-aurora]]'s three sibling modules keep naming
  and the fourth denies needing (task #153) — the pattern exists, in the
  crate next door, cited by name.

- **The two tests are chosen well for what a data blob can get wrong.**
  Every size verifies, the cell width equals its advance, the sizes shrink
  strictly and monotonically (so no blob half-wrote), the baseline sits
  inside the cell, a staple glyph is present at the right size in every
  atlas, and an unknown advance falls back. That is the whole failure
  surface of a baked table.

- **Nothing checks the atlases against the outline they came from.** The
  tests prove the blobs are well-formed and mutually consistent; that they
  are the *right* glyphs is a build-time property with no runtime witness.
  A re-bake from a wrong or corrupt source passes everything here and
  renders wrong text — visible instantly to a person, invisible to the
  suite.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
