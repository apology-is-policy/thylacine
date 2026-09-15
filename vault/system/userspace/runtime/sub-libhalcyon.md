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
  - usr/lib/libhalcyon/src/tag.rs
  - usr/lib/libhalcyon/src/instrument.rs
  - usr/lib/libhalcyon/Cargo.toml
  - usr/halcyon/src/lib.rs
  - usr/halcyon/src/main.rs
  - usr/halcyon/Cargo.toml
audit: light
guarded-by: []
validated-by: [prose]
locks: []
hazards: []
abis: [abi-halcyon-palette]
design: ["docs/HALCYON.md section 13", "docs/HALCYON-VISUAL.md", "docs/HALCYON-INSTRUMENT.md"]
created: 2026-09-05
updated: 2026-09-15
---
## Purpose

The Halcyon environment library (HALCYON.md 13): the shared pieces of the
graphical environment that must not fork between the compositor and its
clients. Five modules, each a thing that must not fork between the compositor
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
cells in, and `theme::env_palette(theme)` (with `daylight_env_palette()` its
`DAYLIGHT` specialization) the `role=RRGGBB` text a Halcyon session publishes to
`/env/HALCYON_PALETTE` ([[abi-halcyon-palette]]). `layout::serialize`/`parse`
round-trip a `LayoutNode` tree to and
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

**`env_palette` is the WRITE side of the palette seam.** `theme::env_palette(theme)`
renders the `Theme` as the 11-role `role=RRGGBB` text the session publishes to
`/env/HALCYON_PALETTE` ([[abi-halcyon-palette]]) -- the program-agnostic roles
(`bg fg dim accent surface border` + the five syntax roles), each emitted with
the opaque alpha byte dropped, which a hosted pts program (nora) adopts by name.
One role mapping is a deliberate judgement worth keeping: the **`surface` role
resolves from `Theme.header`, NOT `status_bg`.** `surface` is a lifted PANEL a
program paints its own dark ink on (nora's status bar, popups, current-line);
`status_bg` is Halcyon's own dark bottom strip worn with the light `status_fg`,
so a program painting its `fg` on it would render dark-on-dark. `header` is the
light lift that keeps the contrast. This is the concrete write side of the `vt`
palette-seam comment named for v1.x; the roles are host-tested against the
`DAYLIGHT` scripture (`daylight_matches_the_scripture`).

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

## The `halcyon` tool -- the executor that drives the crate

`usr/halcyon` is the native (libthyla-rs) session tool that runs **as the user**
and turns the pure crate into acts: `layout save|restore|list|delete` and
`welcome`. It is the crate's one driver, so it lives here; the authority it
exercises is still adjudicated in [[sub-tapestryd]], not conferred by anything it
holds (it takes no capability, no `SPAWN_PERM`, and adds no server verb -- the
authority is the user's own principal).

- **`name_is_valid` closes traversal by construction.** A layout name is one path
  component, `[A-Za-z0-9._-]`, no leading `-` (so a name never reads as an option),
  no leading dot, and never the save's `.tmp` suffix (the one constant the save's
  temp file and the list filter share). The session path is
  `<home>/lib/halcyon/layouts/<name>` -- with the leaf constrained this way, no
  `..` or absolute name can escape it.
- **Save is the aurora durability discipline verbatim.** Read `/dev/tapestry/layout`
  + each `pane/<id>/tag`, fold through `from_render_text` then `serialize`, and write
  the SESSION tier with write-tmp, content fsync, atomic rename, then a STRICT
  metadata fsync on the same OWRITE fd (the [[sub-aurora]] `config::save` pattern).
  The device tier (`/lib/halcyon/layouts/`) is halcyond's / the bake's, never the
  tool's.
- **Restore verifies the plan against the live tree and aborts rather than
  misplace.** The tool runs on its OWN `/srv/tapestry` session -- a
  `Session(principal)` peer whose splits/tags/claims are judged as the user's (the
  shared `/dev/tapestry` mount, whose peer is the mounter joey, is used only for
  reads). It `prune_env`s the tree, drives the build with `skeleton::plan`, binds
  each symbolic ref to a real pane id by diffing the live `layout` dump, and
  **verifies each split's predicted nest/flatten against what the compositor
  actually did** -- a divergence aborts rather than placing a program into the wrong
  tile. Each tagged leaf is claimed (`pane/<id>/claim`), named, seeded with its
  one-shot `TAPESTRY_CLAIM` token into the tool's `/env`, and spawned as the user
  (`resolve_prog` mirrors the shell's `/bin` search, since the kernel resolves a
  spawn name against CWD, not `$path`); the child's libtapestry auto-consumes the
  token on its first `open`. Under a session compositor (H-4d-1) the tool instead
  tags each leaf for the compositor to host and replays focus, anchoring the built
  part before a pre-existing environment tile (`anchor_last` / `active_is_env`).
- **The H-4b audit F1 build-then-fill window** (a co-resident `Session(other)` or
  `Client` could `close`/`split` the in-flight skeleton during the ~10 s fill,
  because `actor_owns_subtree` is vacuously true on an all-empty subtree) is
  harmless under v1.0's single-session model and is a DoS/misplacement of an
  in-flight restore only -- no escalation, no crash. The fix (blocking a subtree
  with a foreign-owned empty leaf) lands with the multi-seat hardening. Prosecuted
  where the rule lives: [[sub-tapestryd]].

## `instrument` -- the second schema, the projections, the bundle (HALCYON-INSTRUMENT 4)

`instrument.rs` is the fifth module: the Instrument profile's theme schema, the
projections between it and the legacy one, the profile word, and the `Bundle` a
session resolves. `theme::load` DISPATCHES on `[meta] profile` -- absent is the
57-key legacy schema `theme.rs` owns; `instrument-v1` is this one, the 35 colour
roles of the Astra kit's `resolved-tokens.json` exactly, plus the authored
ANSI-16 and the smoothing stroke.

**A theme file carries NO geometry, and that is the identity rule.** Geometry
belongs to the PROFILE -- a compiled table (`INSTRUMENT_BASE` / `metrics_base`),
never a file a theme could name. HALCYON-THEME 2's rule extended by one axis: a
theme may restyle the display, never re-lay it.

**The loader is whole-file accept-or-refuse.** `from_entries` bounds truncation
at `INSTRUMENT_MAX` (16 KiB) as `theme::THEME_MAX` does; the meta is strict
(`schema = 1`, `profile` exactly the word, `id` a gallery id, `name`
presentable, `color_scheme` dark|light) with `group`/`tagline`/`rank` the
optional three; an unknown key, an unknown table, or a bare key outside a table
is refused by name and line; and every colour key is REQUIRED, so a partial file
returns `Incomplete { missing }` NAMING each absent key rather than silently
inheriting a floor. It starts from `builtin()` anyway -- not because anything of
it survives (nothing does: every key is required) but so the type stays total
without an all-`Option` mirror of itself.

**The projections are approximations BY CONSTRUCTION and say so.**
`project_legacy` / `project_instrument` are pure and total, so ANY theme renders
under EITHER profile -- a file in the other schema is projected, never refused.
Exactness holds only in a theme's native profile; the kit sidecars are pinned to
project to their stock twins exactly, which is what keeps "approximate" from
drifting into "wrong".

**`Bundle` -> `Visual` is the "nothing else reads a constant" rule.** The
`Bundle` (profile + both themes, one native and one projected) is the scale-free
thing that crosses the wire; `at(pct)` turns it into the `Visual` a painter is
handed -- profile, both themes, `Metrics`, and the derived opaques. A painter
reads the `Visual` and nothing else, which is the I-1 restatement of the H-3
single-token-source contract this dossier already enforces for `theme`.

**`Derived` resolves the 7.3 opaques once, through a DELIBERATE copy of the
executor's lerp.** The mockup states several colours as an alpha over a KNOWN
substrate (`open_header`, `selection`, the two focus insets, the swatch ring);
`Derived::of` resolves them per theme so every painter fills them flat and none
blends where the substrate is known. `over()` is `cartoon::blend`'s exact
arithmetic (a/256, per lane, truncating), repeated here rather than depended on
so this crate stays free of the painter's crate -- and
`carbon_derives_the_kits_opaques` pins the two against the kit's own figure
(Carbon's `#151819`), which is what stops the duplication becoming a fork.

**`resolve_bundle` falls ONE step per refusal, loudly.** Profile: the user's
word, then the system's, then the `legacy` floor. Theme: the picker's gallery
pick, then the user file, then the system file, then the profile's floor. Each
refused tier emits a note (a MISSING file is silent; a REFUSED one is not). Two
judgements worth keeping: a pick word that is not a gallery id is refused **even
when the caller supplied a file for it** -- the word is user-authored and the
path it would form is not -- and a valid id with NO file behind it is said too,
because a stale pick after a gallery change would otherwise read as "the picker
did nothing". `Sources` injects the file contents so the policy is pure and
host-tested, with the I/O left at the caller exactly as `theme::resolve` does.

**Why this module is `audit: light` though it parses authored bytes.** The same
reason the rest of the crate is: it holds no authority, makes no syscall, and
takes its input as an injected `&str`. The untrusted READ -- walking
`/lib/halcyon/themes` and deciding what may become a path -- happens in
[[sub-halcyond]], which is `audit: hard` and where the format-fuzz prosecution
belongs. The split is the same one this dossier already draws for
`layout`/`skeleton` vs the H-4b gate in [[sub-tapestryd]].

**Coverage note.** `carve.rs`, `scale.rs` and `toml.rs` are NOT yet carried.
`toml.rs` is the one to fold next: it is the parser this loader consumes, so it
is the actual substrate of the format-fuzz surface, and it already carries the
tests that say so (`arbitrary_input_never_panics`,
`a_seeded_corpus_of_garbage_always_returns`).

**Tests.** 16 of the crate's 119 host tests are this module's (measured green
2026-09-15, `cargo test -p libhalcyon --lib --target aarch64-apple-darwin` from
`usr/` -- the workspace pins the guest target, so the override is required).
They pin the strict meta, the partial file naming every missing key, the ANSI
slot rule, the registry covering every field, the dispatcher routing on the
profile word, the sidecar-to-stock projection equality, the tier ladder falling
one step per refusal, and a gallery id being a path ONLY when it is an id.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
