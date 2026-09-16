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
  - usr/lib/libhalcyon/src/toml.rs
  - usr/lib/libhalcyon/src/carve.rs
  - usr/lib/libhalcyon/src/scale.rs
  - usr/lib/libhalcyon/src/motion.rs
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
updated: 2026-09-16
---
## Purpose

The Halcyon environment library (HALCYON.md 13): the shared pieces of the
graphical environment that must not fork between the compositor and its
clients. Eight modules, each a thing that must not fork between the compositor
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
- `instrument` (2026-09-14) is the SECOND theme schema, the two projections and
  the resolved `Bundle` -- so a display's profile, not a theme file, owns the
  geometry (see its section below).
- `toml` is the crate's own restricted TOML subset, the parser BOTH schemas'
  loaders consume -- the actual substrate of the format-fuzz surface.
- `carve` is the Instrument split and stack arithmetic -- the ONE snap rule, so
  a container's children partition it exactly minus the tracks.
- `scale` is the display scale: the percent derived from the display's physical
  size (or declared on the command line) that every painter follows.

It depends only on [[sub-lib-vt]] (for `vt::Palette`, which `theme` produces);
everything else is pure `no_std` + `alloc` -- the TOML subset is the crate's
OWN module, not a dependency.

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
  the H-4b tests, not by a shared type. It changed on 2026-09-16 (a split on a
  stacked tile splits the stack, under Instrument), and the model was kept in
  step by never PLANNING that split: `flatten_stack_members` removes every
  container member first (see the section below).
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

**`effects` -- section 10's literals, which are NOT theme fields (I-8).** The
module carries the seven effect colours the source defines (the divider drag
glow, the split flash, the status glow, the picker and help shadows, the
backdrop) with their alphas and blur radii. They are constants rather than
`InstrumentTheme` fields on purpose: section 10 says they "stay amber / green
literals on every theme (the CSS does not tokenise them)", and the difference
is MEASURABLE, not stylistic. Carbon's `amber` is `#C7B98B`, a pale sand; the
divider glow is `#D59A42`, a saturated orange. Carbon's `success` is
`#819B85`; the status glow is `#70A17C`. A painter that reaches for the token
paints the wrong colour under Carbon and a DIFFERENT wrong colour under every
other theme, because `success` is the source of the whole sage family.

That is not hypothetical: the status glow SHIPPED tokenised at `ebf8cab6` and
was corrected immediately after. It passed because the sage pair are close
enough to look right, and because the test asserted `s.inst.success` -- a
witness derived from the implementation, which can only ever confirm that the
code does what the code does. So the module's test pins each literal to its
value AND asserts it NOT EQUAL to the token it would be mistaken for; that
second half is the assertion whose absence allowed the defect.

Section 10 states TWO card shadows (the picker at .32 / dy 20 / blur 55, help
at .35 / dy 24 / blur 80) and the module carries ONE, `CARD_SHADOW`, at the
help card's heavier pair. That collapse is operator-answered (2026-09-16) and
recorded in section 10's amendment: the compositor cannot tell a picker from
a help card -- all four halcyond models ride one `Role::Menu` surface,
`MenuState` carries only `{n, gen, rect}`, `surf.title` is written and never
read, and `menu place` takes only coordinates -- and the radius cap had
already flattened blur 55 and blur 80 to the same value, leaving 8/256 of
alpha and four pixels of offset between them.

The alphas are `pct256` of the stated percentage -- the same rounding the
`Derived` opaques take -- so an effect and a derived opaque that both say
".25" agree to the byte rather than drifting by one. The seventh effect, the
swatch's white .12 inset border, is deliberately absent: its substrate is
known (`amber`), so 7.3 resolves it once as `Derived.swatch_ring` instead of
compositing it per frame.

## `toml` -- the restricted parser under BOTH schemas

`toml.rs` sits under BOTH loaders: `theme.rs`'s legacy 57-key schema and
`instrument.rs`'s Instrument one both consume its `Entry`/`Value`, and
`theme::load` parses ONCE before dispatching on `[meta] profile`. So a gallery
file of either schema crosses this parser and no other -- which makes this
module, not either loader, the actual substrate of the format-fuzz surface, and
the reason the crate takes no TOML dependency.

**It is a subset by REFUSAL, not a parser with gaps.** Entries are borrowed
(`Entry<'a>` / `Value<'a>` over the source, no copy). A table header must be one
plain name: nested `[a.b.c]`, empty `[]` and `[a b]` are `BadTable` at their
line. A key must be bare and followed by `=`. A value is a quoted string, an
integer, or an array of strings; floats, booleans, dates, inline tables and bare
words are each `BadValue`. Every refusal carries its LINE, which is what lets a
loader tell a user WHICH line of their theme was wrong.

**A duplicate key is REFUSED, not last-one-wins.** The ordinary TOML-ish
behaviour lets a later line silently override an earlier one; for a file that
decides what colour a trusted surface paints, a silent override is the wrong
failure mode. Pinned by `a_duplicate_key_is_refused_rather_than_last_one_wins`.

**A `#` inside a string is a colour, not a comment** -- the one lexing subtlety
a theme parser must get right, since every colour literal begins with `#`.
Pinned by `a_hash_inside_a_string_is_a_colour_not_a_comment`.

**The bounds here and the bounds above it do DIFFERENT jobs, and both are
load-bearing.** This module caps what a parse can allocate: `MAX_ENTRIES` 512,
`MAX_ARRAY` 64, `MAX_ARRAY_LINES` 64, each `TooLarge` at its line. The callers'
caps (`theme::THEME_MAX` 64 KiB, `instrument::INSTRUMENT_MAX` 16 KiB) are NOT
about memory -- they refuse a file BEFORE parsing, because a caller's slurp stops
at its own limit and returns what it got, and **a truncated theme can be
perfectly valid TOML**. That is worse than malformed, because the whole-file
completeness check never fires and the user gets a silently half-applied theme.
Refusing anything that COULD have been cut is the only way to tell the two
apart, so dropping either layer leaves a real hole.

**Tests.** 11 host tests, and they are the robustness set rather than a feature
set: `arbitrary_input_never_panics` and `a_seeded_corpus_of_garbage_always_
returns` (in a no_std tool a panic is a silent `exit(1)` -- the failure these
parsers exist to avoid), `every_unsupported_construct_is_refused_with_its_line`,
the duplicate-key and hash-in-string rules above, the oversized and unclosed
array bounds, and a key before any header being rooted.

## `carve` -- the split and stack arithmetic, and the ONE snap rule

`carve.rs` is the Instrument profile's geometry arithmetic (HALCYON-INSTRUMENT
5.2 / 5.4). [[sub-tapestryd]]'s pane tree calls it with the scaled table and
stores the rectangles; nothing here knows a pane, a surface or a display, which
is why it is pure and host-tested while the authority stays in the compositor.

**The one snap rule is the whole point.** `split_spans` divides `extent` among
`n` children separated by `track`-wide tracks. Boundaries are accumulated as
exact rationals over a common denominator and snapped ONCE each (round half up);
each child is then the DIFFERENCE of two snapped boundaries -- never two
independently rounded widths. That is what makes the children partition the
parent exactly minus the tracks, with no drifting seam and no off-by-one column
between a divider and the pane beside it.

**Minima use a flex fixed point, not a single pass.** A child whose ideal share
`U * w_i / sum(w)` falls below its minimum is FROZEN at that minimum and the
remainder re-shared among the rest; the loop repeats until no unfrozen child is
under its minimum. Two degenerate cases fail in DELIBERATELY OPPOSITE
directions: when the minima alone exceed the usable extent every child is laid
at its minimum from the origin and the last ones OVERRUN, because the caller
clips and "a display that small keeps its data and scrolls" -- it never drops a
child; when every child is frozen the slack is taken by nobody, so the last
child ends SHORT of the extent and the caller sees a short span, never an
overrun. A zero weight counts as one (the verb refuses 0; this stays total), a
missing minimum is 0, and every boundary saturates into `u32`.

**It is checked against a browser, not only against itself.**
`the_reference_layout_snaps_where_chromium_did` pins the 1440 x 900 reference
against Chromium's own raster (JOURNAL run 46o): the browser lays out in 1/64 px
and snaps each box's edges to the nearest device pixel, which is this rule on
the same boundaries -- the root divider lands on columns 738..744 and the right
column's on rows 443..449 in both. An independent implementation agreeing to the
pixel is far stronger evidence than a self-consistent fixture.

**The drag band and the stack.** `drag_pair` follows the pointer under the
mockup's ratio clamp (`DRAG_RATIO_MIN_PCT` 22 .. `DRAG_RATIO_MAX_PCT` 78) kept
wherever it is TIGHTER than the minima, and `equalise_pair` is the double-click.
`stack_alloc` (5.4) lays a stack inside a frame's inner box: headers before the
open tile stack DOWN from the top, headers after it stack UP from the bottom,
the body is what remains, and the open tile's separator sits below its body
unless it is last. `stack_min_h` is the matching minimum -- the frame twice, `n`
headers, that separator only when `n > 1`, and a body minimum.

**Tests.** 13 host tests: the Chromium reference, the two-child flex identity,
children partitioning the extent minus the tracks, equal weights dividing with
the remainder spread by the snap, a minimum freezing a child while the rest
re-share, minima that overflow stacking from the origin, zero weights counting
as one, the drag clamped by the tighter of band and minima, the double-click
equalise, and the stack's allocation, lone-tile case and minimum.

## `scale` -- the display scale every painter follows

`scale.rs` (HALCYON-SCALE 3) is the percent the compositor derives and the
clients obey: [[sub-tapestryd]] derives, [[sub-halcyond]] consumes. The rule is
the operator's -- logical pixels at a 96 DPI reference, `scale = DPI / 96`
snapped to the nearest 0.25, round half up -- and the value carried is that
scale TIMES 100 (100 / 125 / 150 / 175 / 200), so the ctl line and the verb move
an INTEGER and no float ever crosses the seam. `round_half_up` exists because
`f32::round` is not in `core`.

**`scale_pct` takes the SMALLER axis on purpose:** each axis' DPI is snapped
independently and the lower result wins, so a monitor lying about one dimension
cannot blow the other up; any zero dimension yields 100.

**The clamp happens BEFORE the narrowing, and that ordering is a landed audit
fix.** A 1 mm axis under thousands of pixels makes `quarters * 25` exceed `u16`,
and a wrapped value need not even be a multiple of 25 -- an off-table percent
from a hostile or garbled EDID. Clamping after the narrowing would clamp the
ALREADY-WRAPPED value and admit it. See [[haz-latch-keyed-on-proxy]]'s sibling
lesson: a bound applied on the wrong side of a conversion is not a bound.

**Two untrusted inputs, both fail-safe.** `parse_edid_mm` reads device bytes
(VESA E-EDID 1.4) and returns `None` unless the block is 128 bytes with the
fixed header, a wrapping checksum of zero, and BOTH axes within
`1..=EDID_MM_MAX` (2 m -- larger is not a monitor); the first detailed timing
descriptor carries the millimetres when its pixel clock is non-zero, otherwise
the basic parameters' centimetres stand in. `declared_scale` reads the kernel
command line for the LAST whole-word `thylacine.scale=<pct>` token -- word
boundaries checked so `xthylacine.scale=` and `thylacine.scalex=` are not it,
the value terminated by space, NUL, newline or tab because the FDT property
carries its NUL. A declaration OUTRANKS the EDID, because it exists precisely
for the display whose EDID cannot say (QEMU's synthetic one) or lies; a token
that is not one of the five values is `Declared::Invalid`, said once and
ignored, and the EDID stands.

**Tests.** 6 host tests: the operator's snap rule, the five values and their
steps, the pixel helpers' rounding, the EDID parse's fail-safe arms, a short
millimetre axis never truncating off the table (the clamp-ordering regression),
and the declaration being the last whole-word token with a table value.

## `motion` -- section 10's movement, and the rule deciding whether it runs

`motion.rs` (HALCYON-INSTRUMENT 10 + 9.5 as amended at I-8) holds what section
10 MOVES, as `instrument::effects` holds what it PAINTS -- which is why
`SPLIT_FLASH_MS` stays in `effects` rather than migrating here. It is shared
for the reason `scale` is: [[sub-halcyond]] animates the tile, the hover, the
body and the caret, while the split flash is [[sub-tapestryd]]'s.

**A dead monotonic clock turns motion OFF rather than freezing it.**
`libthyla_rs::time::monotonic_ns` is documented fail-soft -- 0, forever, when
the clock is unreadable -- so every deadline built on it would sit permanently
in the future and NO animation would ever complete. A tile stuck mid-expansion
reads as a hung compositor rather than as a broken clock, so `admitted`
refuses on a zero sample and `phase` returns 1.0 rather than 0.0. The degraded
state is section 9.5's STATIC DEFAULT -- a mode that section already specifies
and supports, so this is a documented behaviour rather than a fallback
invented for the occasion.

**Motion is ON by default and `0` is the only opt-out** (9.5 as amended at
I-8, operator-answered): absent, empty, and any other word all mean on,
trimmed exactly as the scale lever trims.

**`stated` and `admitted` came apart at I-8c-3a, because their two conditions
belong to different parties.** The WORD is the user's and it TRAVELS: the
session reads it once and forwards it to [[sub-tapestryd]] as the `motion`
verb, because the compositor cannot read the user's `/env` at all. The CLOCK
is the reader's, and the two readers do not even share a substrate --
[[sub-halcyond]] animates against `monotonic_ns` deadlines while the
compositor animates against its own `Instant`-paced frame tick. Forwarding
`admitted`'s folded verdict would therefore hand the compositor one process's
clock fault dressed as the other process's user preference, turning
animations off on a machine whose compositor clock is fine. `admitted` is now
`now_ns != 0 && stated(word)`, so the word rule still has exactly one home.

**`fold_timeout` is one reducer where the two halcyond loops had two.** The
console's poll timeout is always positive and was reduced with `min`; the
session's carries the `-1`-means-infinite sentinel and needed a guarded
`match`. The session's form IS the general case and the console's `min` is its
positive-`current` specialisation, so both call sites expand to their
originals verbatim and the adoption changed no behaviour. The frame clock
(I-8c-2) is then a THIRD fold rather than a second hand-written reducer at
each site, in two different shapes.

**The easing is deterministic by construction.** `cubic_bezier` bisects over a
FIXED 20 iterations rather than running Newton to a residual: `core` carries
no `f32::abs`/`sqrt`/`powi` -- the same gap `round_half_up` works around above
-- and I-9 compares against goldens, where a convergence-dependent iteration
count would not reproduce. 20 halvings bound the parameter error at 2^-20.

**`ease_out` is named as the CSS KEYWORD, not spelled at its call site.**
`cubic-bezier(0, 0, .58, 1)` is the DEFINITION of `ease-out`, not a choice
anyone made, and four control points inline at a call site read as a tuning
knob. It is the split flash's curve (`animation: flash .25s ease-out forwards`
in the kit) and must not be confused with `ease_expand`: the witness pins
`ease_out(0.5) < ease_expand(0.5)`, which fails if either is substituted for
the other and which an endpoints-and-monotonicity check cannot see.

**`FRAME_MS` is OURS, and says so in its own doc comment.** Section 10 pins
durations and curves and states no frame rate, so the 16 ms cadence is a
compositor-independent choice; a later reader must not cite it as scripture.

**`caret_next_step_ms` exists because `FRAME_MS` is the wrong cadence for a
square wave** (I-8c-2). `steps(2, start)` changes exactly twice per period,
and 1100 ms holds 68.75 frames at 16 ms, so a frame-rate wake would fire
about 34 times per visible change and paint nothing on 33 of them. The
deadline is instead the distance to the next EDGE -- 605 / 495 ms
alternating. It is never zero, which is what lets a poll fold it without
spinning; and the truncation in `now_ns / 1_000_000` works in the caller's
favour, since the sub-millisecond remainder makes the wake land AT or AFTER
the edge, never one millisecond short of it. `FRAME_MS` keeps its consumer:
the CONTINUOUS tweens of I-8c-3. One motion, one cadence, chosen from what
the motion actually does.

**Prosecute**: the caret deadline's witness walks a whole period one
millisecond at a time, asserting `caret_visible` holds to the deadline and
has changed AT it -- an endpoint-only check would pass a deadline that
pointed anywhere inside the correct half. The two poll call sites are
bin-side and have NO host witness
-- a transposed or dropped fold still compiles and nothing fails, and
halcyond's lib count does not move across such an edit; the dead-clock rule is
SILENT, so a user reporting "no animations" may have an unreadable clock
rather than a preference; and any move from bisection to a convergence test
reopens the golden determinism I-9 depends on.

## `halcyon workspace <n>` -- the tool verb that needed no new channel (2026-09-15, W-2b)

`Cmd::Workspace { n }` and its `parse_cmd` arm. One-based, bounded 1..=9 in
the PARSER as well as in the compositor -- the tool should name what is wrong
rather than forward a number the tree refuses with an errno the user never
sees.

**The part worth recording is what did NOT have to be built.** The verb rides
the `layout` file, and the tool's own `/srv/tapestry` conn already resolves to
`Actor::Session(principal)` -- the same actor halcyond gets, and the route
HALCYON.md 13.7 ratified for `halcyon layout restore`. So no service, no
`SPAWN_PERM_MAY_POST_SERVICE`, no protocol, no new authority model. An earlier
claim of mine -- that the tool had to reach the switch "through halcyond, the
way the theme picker's word travels" -- was disproved: the picker is a menu
INSIDE halcyond and `write_user_pick` is halcyond WRITING the user's file, so
that precedent runs the opposite way, and halcyond posts no service at all.

Five parser tests, one of them the control: `workspace` must be its OWN
subcommand and not fall through to the unknown-command arm, which is the
failure the other four would all still pass under.

## The tool's help outlived the rule it described (2026-09-15, round 2, F7)

S4 retired "only the next free number may be made" -- a property of the old
DENSE representation, not of the design -- and `ea53ac57` touched 21 files
without reaching `usr/halcyon/`. The tool's VERB was already correct: it
writes `workspace <n>` to the `layout` file and the compositor decides, so
nothing misbehaved. What diverged was the user-facing help text and two doc
comments, which went on telling the operator a rule the system no longer
enforces. Corrected to "creating it if it does not exist".

Recorded because it is the cheap half of a recurring shape: a behaviour change
lands in the MECHANISM and leaves its DESCRIPTION behind, where no compiler
checks it and no test reads it. Round 2 found it only because the prosecutor
looked outside the fourteen files it was handed.


## `MAX_WORKSPACES` -- one definition for two crates (2026-09-15, round 3, F4)

The workspace bound existed as two independent `const MAX_WORKSPACES: usize =
9` -- one in the compositor, one in the renderer's header reader -- with a
comment in the reader explaining that halcyond cannot import the compositor's.
That comment documented the hazard without preventing it: the standing "a guard
pinned to a NAME is re-pointed by hand" class.

It lives here now for the reason `layout` itself lives here: the compositor
ENFORCES the bound, the renderer must not accept a header that exceeds it, and
both link libhalcyon while neither links the other. `pane.rs` re-exports it so
`pane::MAX_WORKSPACES` still names it.

**The drift direction is what made it worth fixing.** Had the compositor's
bound risen alone, the reader would have rejected legal headers, so
`parse_workspaces` would return `None` and the bar would silently keep its
default -- fail-closed, but invisible at the only place a user can see it.

Proven LINKED rather than merely compiling: setting this constant to 8 fires a
test in BOTH crates. The tapestryd side needed a new absolute assertion to
manage it, because every bound assertion there had been written relative to the
constant and so could not see it move.

## A saved stack's container members restore FLAT (2026-09-16, HALCYON-INSTRUMENT 6.1)

The compositor stopped building containers inside stacks under the Instrument
profile ([[sub-tapestryd]], "a stack's members are tiles"): a split on a
stacked tile now splits BESIDE the whole stack. That broke an assumption
`skeleton::plan` depends on. For a stacked container the planner emits its
splits, then `SetMode`, then grows each child IN PLACE -- so a child that is
itself a container issues `split <stacked leaf> <dir>` and expects a nest.
The compositor now answers by splitting the stack instead, the executor's
per-op verification sees the wrong shape, and the restore diverges and
returns 1. A layout saved before the rule (the operator's `tyr-quake` shape)
or edited by hand could therefore never restore.

**`layout::flatten_stack_members`** lays every `Stacked` / `Tabbed`
container's CONTAINER members flat before planning: each is replaced, in
place, by its own leaves in order, recursively, so every tile survives -- the
kit's rule that an already-loaded layout "must retain data ... not delete
tiles". Only the nested arrangement is lost, and it was never renderable. A
member that was its stack's ACTIVE child hands the stack its own active-path
leaf (`active_leaf_offset`), so the tile that was open stays open; a laid-flat
leaf takes the default weight (a stack divides nothing); split containers, and
stacks whose members are already leaves, come back EQUAL, which is what makes
the pass safe to run on every restore. Witnesses: the member laid flat with the
open tile kept, flattening at every depth with splits outside stacks untouched
(weights too), and the unchanged-tree control. Sabotage-measured separately:
dropping the active offset, and keeping a laid-flat leaf's split weight, each
fail one.

**The tool applies it only under Instrument**, because under legacy a
container inside a tabbed/stacked one is an ordinary i3 shape the compositor
still builds. `halcyon layout restore` resolves the seat's profile with
`seat_profile(home)` -- the user's `lib/halcyon/profile` word, then the
system's, the two tiers the session reads at start -- through
**`instrument::resolve_profile`**, which is `resolve_bundle`'s first step
extracted unchanged: the user's word wins, a word that is not a profile is
skipped and its tier's label returned (so `resolve_bundle` still writes the
same REFUSED notes), and nothing at all is legacy from the built-in tier. A
profile tier that exists but cannot be read fails the restore (`read_theme`
says why) rather than guessing a profile and building the wrong tree.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
