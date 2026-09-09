# HALCYON-THEME — one file defines the whole coherent visual

**Status: DESIGN, for the operator's ratification.** Requested 2026-09-09,
verbatim: *"unify the pallete definitions into one and make it loadable from
a single TOML file somewhere. It feels like that TOML file could also host
the font rasterization and other parameters -- a single TOML file to define
the entire coherent visual of the theme. What I don't want to be a part of
the theme is the faces themselves, as they're core Thylacine visual
identity."*

**Scope**: every value that decides how Thylacine *looks* — colour,
rasterization weight, and the structural geometry those two are tuned
against — behind one type, loaded from one file, with nothing in the code
able to read a colour constant instead. **Not** the typefaces (§2), not the
layout algorithms, not the composition rules (`HALCYON-COMPOSITION`), not
the display scale (`HALCYON-SCALE`, which is derived from the panel, not
chosen).

**Companions**: `HALCYON-VISUAL.md` §1/§2/§4/§9 (the operator's document —
the Daylight values are *its* scripture; this document changes only where
they live and how they are read), `HALCYON-TYPE.md` §4.2 (the smoothing
stroke this file will carry), `HALCYON.md` §14.12 (the session/tile palette
coherence rule this design finally makes structural).

---

## 1. The problem, stated exactly

Three defects, all verified in the tree on 2026-09-09.

**1.1 — Two palettes, each naming the other as the source of truth.**

`usr/lib/libhalcyon/src/theme.rs` opens:

> *"This is the SINGLE token source the ratified H-3 split names: halcyond's
> transcript Sheet + chrome surface AND tapestryd's pane bevel/hairline/
> cast-shadow constants both derive from here and nowhere else"*

`usr/lib/vt/src/lib.rs`'s `DAYLIGHT` says:

> *"Single source of truth: libhalcyon::daylight_palette() returns this."*

and `libhalcyon::daylight_palette()` says:

> *"Single source of truth is `vt::DAYLIGHT`"*

It is a cycle. In practice `vt::DAYLIGHT` **hand-copies two hex literals**
out of the other crate:

```rust
pub const DAYLIGHT: Palette = Palette {
    bg: 0xFFF2_EBE0, // libhalcyon DAYLIGHT.surface
    fg: 0xFF1A_120A, // libhalcyon DAYLIGHT.fg
```

A test (`daylight_is_vt_daylight`) pins them equal, so they cannot drift
*silently* — but a second theme has to be written twice, in two crates, and
the test only ever checks Daylight.

**1.2 — The colours are catalogued in one place but not *read* from one
place.** `Theme` has 43 colour fields, each documented. But roughly twenty
**production** sites read the `DAYLIGHT` **constant** directly rather than a
theme handle — in `halcyond/src/{status,chrome,menu,layout,tile,main}.rs`
and `tapestryd/src/server.rs`. Define a second theme today and every one of
those keeps painting Daylight. This is precisely the operator's stated
worry: *"so that if a theme is made and all colors are changed to form a
dark theme, some hardcoded daylight color won't kick it in somewhere."*

**1.3 — There is no file.** A theme is a `const` in a Rust crate, so
changing one is a rebuild, and a user cannot have one at all.

## 2. What is a theme, and what is identity

**In the theme** — everything a coherent visual redefinition must move
together:

| Group | Values | Today |
|---|---|---|
| Chrome palette | grounds (5), text weights (4), bevel faces (4), ember trio (3), the two live keys (7 each), syntax roles (9), status bar (4) — **43** | `libhalcyon::theme::Theme` |
| Terminal palette | `bg`, `fg`, `ansi[16]` — **18** | `vt::Palette`, duplicated |
| Rasterization | the smoothing stroke (`smooth_mem`, thousandths of an em) | `Theme.smooth_mem` |
| Geometry | bevel, gap, hairline, header_h, status_h, tag_pad_x, tab_strip_h — **7** | `Theme` |

**NOT in the theme — Thylacine's visual identity, fixed:**

- **The typefaces.** IBM Plex Sans (Text 450 / Bold / Text Italic / Regular
  Italic) and Cornucopia. Operator's ruling, quoted above. A theme may not
  name a face, and the loader has no key for one.
- **The rasterization *pipeline*** — the outline path, the coverage union,
  the quarter-pixel phases (`HALCYON-TYPE` §4). The stroke's *amount* is a
  taste; how a glyph becomes pixels is not.
- **The scale** — derived from the panel's DPI (`HALCYON-SCALE`), or
  declared on the cmdline. Not a look, a fit.
- **Layout and composition** — what is proportional, what is a mono island,
  where a rule goes (`HALCYON-COMPOSITION`). A dark theme is the same
  document in different ink.

Geometry is in because the two are tuned against each other: a bevel is a
lit edge, and its width and its four face colours are one decision. A theme
that changes the light without the geometry reads wrong.

## 3. The shape

### 3.1 One type, one owner

`libhalcyon::theme::Theme` absorbs the terminal palette by value:

```rust
pub struct Theme {
    // ... the 43 chrome colours, unchanged ...
    pub terminal: vt::Palette,   // bg, fg, ansi[16]
    pub smooth_mem: u16,
    // ... the 7 geometry tokens, unchanged ...
}
```

`libhalcyon` already depends on `vt`, and `vt` depends on nothing, so this
is the direction that already exists — no new edge, no cycle. `vt` keeps
`Palette` as a **type** and keeps `THEMES` (the user-selectable
`set_theme` sets, which are a terminal feature, not a Halcyon theme). What
it loses is `vt::DAYLIGHT` — the hand-copied const — which becomes
`DAYLIGHT.terminal` and is derived, not transcribed. `vt`'s own comment
already names this as the intent: *"v1.x: the compositor plumbs the palette
to the kaua-term rather than the producer defaulting."*

### 3.2 Nothing reads a constant

The rule, and the thing that makes a second theme actually work:

> **No production code may name `DAYLIGHT`.** Every colour, stroke and
> geometry token is reached through a `&Theme` threaded from the one place
> that owns it. `DAYLIGHT` remains, as the built-in default and as the
> fixture the scripture tests pin — reachable from tests and from the
> loader's fallback, and nowhere else.

Mechanically enforced, not merely stated: a `#[cfg(not(test))]` visibility
split, so that a production reference does not compile. That is the only
form of this rule that cannot rot.

### 3.3 The file

TOML, because the operator asked for TOML and because the subset needed here
is small enough to parse in `no_std` without vendoring a crate (§5).

```toml
[meta]
name = "Nocturne"
base = "daylight"        # inherit every unset key; omit to require all (§4.2)

[palette]
surface   = "#1A1714"
header    = "#241F1B"
fg        = "#E8E0D4"
ember     = "#E07840"
# ... any subset of the 43 ...

[palette.sage]           # the live-key families, as their own tables
key = "#3F6B3F"
tint = "#20281F"
# ...

[palette.syntax]
slate = "#7FA8D0"
# ...

[terminal]
bg = "#1A1714"
fg = "#E8E0D4"
ansi = ["#3A332E", "#9C3A28", ...]   # exactly 16

[type]
smooth = 0               # thousandths of an em; Daylight is 12 (§HALCYON-TYPE 4.2)

[geometry]
bevel = 2
gap = 2
hairline = 1
header_h = 20
status_h = 20
tag_pad_x = 6
tab_strip_h = 5
```

Colours are `"#RRGGBB"` (alpha is always opaque — the pixel format's alpha
byte is not a theme decision) and scalars are plain integers.

### 3.4 Where it lives, and who wins

| Path | Owner | When |
|---|---|---|
| built-in `DAYLIGHT` | the binary | always; the floor nothing can remove |
| `/lib/halcyon/theme.toml` | the system | read at start by tapestryd and by halcyond |
| `$HOME/lib/halcyon/theme.toml` | the user | read by the user's session, overrides the system file |

`/lib/halcyon/` is already the established home (`/lib/halcyon/renderer`,
`/lib/halcyon/layouts`, `$HOME/lib/halcyon.rc`), so this adds a file, not a
convention.

**Display coherence.** tapestryd paints the chrome; halcyond paints the
content; they must agree or the bevel does not match the pane. tapestryd is
system-spawned and halcyond is the user's, so the user's file cannot reach
tapestryd by being read — it is pushed. The session's `declare` already
carries per-display authority (`H-4b`), and `scale <pct>` is the precedent
for a ctl verb that re-decides a display-wide visual: a declared session
pushes its resolved theme the same way. A display with no declared session
keeps the system file.

### 3.5 The existing export stays downstream

`/env/HALCYON_PALETTE` already publishes `role=RRGGBB` text so a hosted pts
program (`nora`) can match. That export becomes a **rendering of the
resolved theme** rather than a second hand-maintained list — one direction,
file → `Theme` → env, never back.

## 4. The failure modes, decided

**4.1 — Missing file: silent fallback to `DAYLIGHT`.** The default
installation has no file, and that is not an error.

**4.2 — Malformed file: LOUD fallback, and the whole file is refused.** A
half-applied theme is the operator's stated nightmare in its worst form —
some values new, some Daylight, no way to tell which. So a parse error
rejects the *file*, names the line, and paints Daylight. Never a partial
apply.

**4.3 — Incomplete file: the operator's actual worry, answered directly.**
`base = "daylight"` means unset keys inherit — convenient, and correct for a
theme that only retints a few roles. But for a **dark** theme that is the
exact hazard: forget `status_muted` and a bright Daylight grey lands in the
middle of a dark bar. So:

- **Omit `base`** and the file must set **every** key. A missing one is a
  parse error (4.2) that *names the missing keys*. This is the mode a
  serious theme uses, and it makes "some hardcoded daylight colour kicked
  in" impossible by construction.
- With `base`, a `halcyon theme lint` reports every inherited key, so the
  convenient mode is still auditable.

**4.4 — A theme cannot break the invariants it does not own.** The loader
clamps nothing and validates only *shape*: a theme may choose an unreadable
contrast, and that is the author's business. It may **not** change a
geometry token to a value the compositor's own bounds reject (a negative
hairline, a bar taller than the display); those are refused as 4.2.

## 5. The parser

A `no_std` TOML **subset**, in `libhalcyon`: comments, `[table]` and
`[table.sub]` headers, `key = "string"`, `key = <integer>`, and
`key = ["...", ...]` arrays of strings. No dates, no floats, no inline
tables, no multi-line strings. That is the whole grammar this file needs,
it is a few hundred lines, and it is fully host-testable — which a vendored
general-purpose crate would also be, at the cost of a vendoring round and a
much larger surface for a format we control both ends of.

A malformed input must never panic: the parser is total, returning an error
with a line number, and it is fuzzed against the untrusted-input bar the
project already applies to the Beacon wire — a theme file is a *user* file,
but the user is not always the author.

## 6. Chunks

- **TH-1** — Fold `vt::Palette` into `Theme` as `terminal`; delete
  `vt::DAYLIGHT`'s hand-copied hex; make `daylight_palette()` a derivation.
  The `daylight_is_vt_daylight` pin becomes a statement about one value
  rather than two, and the scripture pin test stays.
- **TH-2** — Thread `&Theme` through every production site that names
  `DAYLIGHT`, and make the constant unreachable from production by
  visibility (§3.2). This is the chunk that makes a second theme *work*;
  it is mechanical, wide, and its test is that the constant no longer
  compiles when referenced.
- **TH-3** — The TOML subset parser + `Theme::from_toml`, host-tested,
  including every failure mode in §4.
- **TH-4** — The load path: the two file locations, the session's push to
  tapestryd, `/env/HALCYON_PALETTE` as a derived export, and a
  `halcyon theme lint` reporting inherited/missing keys.
- **TH-5** — A second theme, shipped, as the proof the arc worked: a dark
  Nocturne written with **no** `base`, so it must set all 61 colours. A
  theme arc that ships only the theme it started with has proved nothing.
- **TH-6** — The audit. The surfaces: a new on-disk format parsed in
  `no_std` (format-fuzz class), a display-wide visual pushed over a ctl
  verb (the `scale` precedent's gate applies), and the §3.2 rule's
  enforcement.

## 7. What this does not decide

- **Frutiger Aero** (`HALCYON-VISUAL` names it as a later theme) becomes a
  TH-5 sibling, not a code change.
- **Live reload.** Re-reading on a file change is a natural TH-4+ addition;
  the design does not preclude it, and nothing here requires it. A theme
  change today costs a session restart, which is honest.
- **Per-tile themes.** The display has one theme. A hosted program may
  choose its own colours from `/env/HALCYON_PALETTE`; that is the program's
  business, not the theme's.
