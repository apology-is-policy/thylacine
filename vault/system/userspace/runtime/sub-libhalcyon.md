---
id: sub-libhalcyon
type: sub
title: "libhalcyon -- the Daylight tokens, the save format, and the restore planner"
parent: moc-userspace-runtime
code:
  - usr/lib/libhalcyon/src/lib.rs
  - usr/lib/libhalcyon/src/theme.rs
  - usr/lib/libhalcyon/src/layout.rs
  - usr/lib/libhalcyon/src/skeleton.rs
  - usr/lib/libhalcyon/src/place.rs
  - usr/lib/libhalcyon/Cargo.toml
audit: light
guarded-by: []
validated-by: [prose]
locks: []
hazards: []
abis: []
design: ["docs/HALCYON.md section 13", "docs/HALCYON-VISUAL.md"]
created: 2026-09-05
updated: 2026-09-05
---
## Purpose

The Halcyon environment library (HALCYON.md 13): the shared pieces of the
graphical environment that must not fork between the compositor and its
clients. Four modules, each a thing that must not fork between the compositor
and a client:

- `theme` is the Daylight visual scripture as code (HALCYON-VISUAL.md) --
  the SINGLE token source the ratified H-3 split names, so halcyond's
  transcript + chrome and [[sub-tapestryd]]'s pane bevel/hairline/cast-shadow
  read the same colours and metrics from here and nowhere else.
- `layout` is the `halcyon-layout v1` save format -- shared by halcyond (the
  device-tier restore) and the user-authority session tool (the session-tier
  save/restore, the H-4b "D decision"), so it lives in neither.
- `skeleton` is the pure restore planner -- a model of the compositor's split
  rule that turns a saved tree back into a sequence of compositor verbs.
- `place` (2026-09-05) is the placement + scale math -- `letterbox`,
  `scaled_clip`, `nearest_src` -- shared by [[sub-tapestryd]]'s scaled compose
  and the tapestry-battery's sample points, so a letterboxed present and its
  test's expected pixels derive from ONE function (the fullscreen-zoom fix; see
  its section below).

It depends only on [[sub-lib-vt]] (for `vt::Palette`, which `theme` produces);
everything else is pure `no_std` + `alloc`.

## Contract

`theme::DAYLIGHT` is the `Theme` (colours + syntax), `theme::METRICS` the
`Metrics`, `theme::hairline(&Theme)` the derived rule colour, and
`theme::daylight_palette()` the `vt::Palette` a per-tile kaua-term stamps its
cells in. `layout::serialize`/`parse` round-trip a `LayoutNode` tree to and
from the `halcyon-layout v1` text; `layout::prune_env` drops the env-marker
leaves; `layout::from_render_text` builds a tree from the compositor's own
dump (the D-decision read side). `skeleton::plan` turns a `LayoutNode` into a
`Plan` -- an op sequence (`Split`/`SetMode`) plus a focus path the executor
replays.

## Mechanism

**`theme` is the one place the palette lives, and that is the whole point.**
The H-3 split ratified that resolved RGB ships across the compositor seam, so
the palette must be applied at the producer, not re-mapped downstream; making
`theme` the sole source is what guarantees a session tile composites
coherently with halcyond's transcript. Colours are `Argb` (0xAARRGGBB,
opaque). The `Theme` struct is theme-agnostic -- Frutiger Aero (deferred) is a
second const of the same shape -- so nothing structural changes when a second
theme lands.

**`layout` parses UNTRUSTED input and is written to prove it can't be made to
fault.** A layout file lives in the user's `$home`, so `parse` is bounded on
every path (`MAX_DEPTH` 32, `MAX_NODES` 256, `MAX_TAG_LEN` 1024) and
fail-closed: a malformed or oversize file returns an `Err` the caller degrades
on (geometry-only, or no restore), NEVER a panic -- because a panic in a
no_std tool is a silent `exit(1)`, the worst possible failure for a restore.
Surface ids and geometry are never saved; a restored leaf gets a fresh surface
and rect from the respawned program, so only the tree's *shape* (container
modes, active child, per-leaf command line) crosses a save.

**`from_render_text` is the write side's inverse**, reading the compositor's
`pane::render_text` dump (the D-decision: the save tool reads `pane/<id>/tag`
and compares `pane/<id>/owner` to its own principal). A tag longer than
`MAX_TAG_LEN` is dropped to empty rather than rejected, so the result always
round-trips through serialize/parse -- and it is bounded and fail-closed
exactly like `parse`, so a garbled dump degrades to no-save, never a panic.

**`prune_env` implements the compositor's dissolve rule while dropping
env-marked leaves.** A container that loses all children becomes nothing; one
that reduces to a single child *becomes* that child (a one-child container
cannot exist in the compositor); otherwise it keeps its mode and re-points its
active index. This is what lets a restored layout omit the throwaway env
panes without leaving a degenerate tree.

**`skeleton::plan` is a MODEL of the compositor's split rule.** The compositor
offers two structural primitives -- `split <leaf> h|v` (which NESTS a new
container when the leaf's parent has a different mode, and FLATTENS into the
parent when the modes agree, always inserting the new empty leaf right after
the split one) and `mode <container> <m>` -- so the planner builds a tree
leaf-first: for each container, split its first leaf N-1 times (the first
nests, the rest flatten), fix a tabbed/stacked mode afterwards, then recurse.
Alternating modes (what the compositor itself produces) nest at every level by
construction; a hand-written same-mode nesting flattens into its parent, which
is exactly the shape a user splitting by hand would get. The planner names
leaves and containers by symbolic refs; the executor (the `halcyon` tool)
resolves them to real pane ids.

## Data structures

`theme`: `Argb`, `LiveKey`, `Syntax`, `Theme`, `Metrics`, plus the `DAYLIGHT`
and `METRICS` consts. `layout`: `LayoutMode`, `LayoutNode` (Leaf carrying a
tag + env marker / Container carrying mode + active + children), `ParseError`,
and the format bounds. `skeleton`: `LeafRef`/`ContRef` (symbolic), `SplitDir`,
`Op` (Split/SetMode), `PlannedLeaf`, `Plan` (ops + leaves + focus path +
counts).

## Concurrency

None. Pure `no_std` + `alloc` (plus the vt palette type); every consumer drives
it single-threaded.

## Invariants enforced

None of the numbered system invariants -- no syscall, no capability, no
handle. Its own rules:

- **`theme` is the single token source** -- a colour or metric that halcyond or
  tapestryd computes independently rather than reading here breaks the H-3
  split's "nowhere else" contract, and the two surfaces drift.
- **`layout`/`from_render_text` never panic and always round-trip** -- untrusted
  `$home` input is bounded and fail-closed, and an over-long tag degrades to
  empty rather than producing a tree that cannot re-serialize.
- **`skeleton::plan` must model the compositor's split rule faithfully** -- the
  plan is replayed against the real compositor, so a divergence between the
  model and the compositor's nest/flatten behaviour produces a wrong tree on
  restore.

**Audit-trigger participation:** `layout.rs` + `skeleton.rs` are named in the
H-4b "Session(principal) pane-authority" audit-trigger surface
(docs/AUDIT-TRIGGERS.md), whose HARD gate -- the Session actor, the one-shot
placement claim, `PFK_OWNER`, the owner-gated claim mint -- lives in
[[sub-tapestryd]]. libhalcyon's half is the pure, authority-free planner + the
save format; it is classified `light` and the gate is prosecuted where the
authority is.

## Error paths

`parse` returns `ParseError` (BadHeader / TooMany / the row-parse errors) and
never panics; the caller degrades to geometry-only or no restore.
`from_render_text` is the same shape. `daylight_palette` and the theme consts
are total. `plan` is total over a well-formed `LayoutNode` (the parser
guarantees one).

## Performance

Irrelevant -- these run once at save and once at restore, over a tree bounded
at 256 nodes. No hot path.

## Prosecution

- **`parse` and `from_render_text` must stay bounded and no-panic.** The input
  is a user file; a panic is a silent `exit(1)` and a lost restore. Every
  path must return an `Err`, and the node/depth/tag caps must hold.
- **An over-long tag must degrade to empty, not reject.** The round-trip
  guarantee (render_text -> serialize -> parse) depends on it.
- **`prune_env`'s dissolve must match the compositor's** (0 children ->
  nothing, 1 child -> that child). A one-child container the compositor cannot
  represent would desync the restore.
- **`skeleton::plan` must track the compositor's split rule.** If the
  compositor changes when a split nests vs flattens, the model here must change
  with it, or restore builds the wrong tree -- this is a model kept in sync by
  the H-4b tests, not by a shared type.
- **`theme` must remain the sole palette source.** A second definition of any
  Daylight token anywhere else is the drift the H-3 split exists to prevent.

## Seams

- Frutiger Aero (the second theme) is deferred -- a `Theme` const of the same
  shape, no structural change.
- Surface ids and geometry are never saved; a restored leaf gets a fresh
  surface + rect from the respawned program.
- The planner is a *model* of the compositor, not a shared implementation;
  keeping the two in step is a discipline the H-4b tests enforce, not the type
  system.

## Caveats

- **Host-tested** (the vt/cartoon crate pattern): the serializer/parser
  round-trip, the bounds and fail-closed paths, the prune-env dissolve, and
  the planner's nest/flatten cases run on the host, which is why the untrusted
  parser can be trusted.
- **The crate header still says "no deps".** It gained one -- `vt`, for
  `daylight_palette()`'s `vt::Palette` return -- when the palette source
  consolidated here; the manifest is the ground truth (a minor doc-vs-code
  drift, noted for a future touch).

## `place` -- the shared placement + scale math (2026-09-05, the fullscreen-zoom fix)

`place.rs` is the fourth module: the placement geometry the compositor and the
tapestry-battery must agree on to the pixel. `letterbox(sw, sh, cw, ch)`
aspect-fits a source into a container (the existing letterbox policy, moved
here from tapestryd so the battery's expected sample points derive from the
compositor's own function, not a re-derivation). `nearest_src(d, s, dw)` is the
exact source coordinate a nearest-neighbour scaled compose samples for a given
destination coordinate; `scaled_clip(...)` is the destination-rect PROJECTION
of a damage rect through that scale -- the clip [[sub-tapestryd]]'s `compose_cpu`
uses so a partial present of a letterboxed surface redraws only its damage, not
the whole scaled rect. Because `scaled_clip` is built on the same `nearest_src`
the compose samples by, a clipped compose is pixel-identical to a whole one with
no seam -- the host tests prove exactly that (`scaled_clip_covers_every_pixel_
the_damage_reaches`, the letterbox identity/pillarbox/never-empty cases).
Pure math, host-tested; the drift it exists to prevent is a compositor that
scales one way and a test that expects another. See [[sub-tapestryd]]'s
fullscreen-zoom section and [[haz-latch-keyed-on-proxy]].

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
