# UTOPIA-VISUAL — Bonfire

Thylacine's user-visible visual identity scripture. Binding for every Utopia
program that emits coloured output. Consumed by `libutopia::palette` and the
`ut` shell's prompt-emit path. Halcyon (Phase 8) renders the same identity
natively on Thylacine's own framebuffer; until then the host terminal renders it.

**STATUS**: COMMITTED — scripture commit, U-2 chunk.

**Supersedes**: UTOPIA-VISUAL U-1 (*Pale Fire*). The U-1 palette (`#0e1018` cold
near-black background, `#d8e4f4` moonlight foreground, `#8898b4` steel path) is
retired. The U-2 palette is a warm shift of the same design logic: a background
that reads as dark room lit by a distant fire rather than cold void. The `⊢`
ember glyph is unchanged and has always belonged here.

---

## 1. The palette — *Bonfire*

Named for the quality of the background: `#0e0c0c` is not red, not warm grey —
it is near-black with a barely-perceptible red cast, the way darkness looks when
a bonfire a few hundred metres away cannot illuminate anything but slightly shifts
the colour temperature of the air. The ember glyph now belongs to its background
rather than contrasting against it.

### 1.1 Foundation colours

| Role | Hex | Description |
|---|---|---|
| `bg` | `#0e0c0c` | Warm near-black. Red channel 14, green and blue 12. |
| `surface` | `#180f0e` | Lifted warm dark; selection background, popup surfaces. |
| `gutter` | `#2a1f1c` | UI borders, ruler column, pane dividers. |
| `border` | `#3a2a26` | Explicit border strokes where gutter is too subtle. |

### 1.2 Text scale

Four steps from readable to invisible. The hierarchy communicates weight by
recession rather than by brightness or colour change.

| Role | Hex | Description |
|---|---|---|
| `fg` | `#e4ddd8` | Primary text, all output — warm off-white. |
| `fg_dim` | `#c8bdb8` | Secondary text, inactive pane text, parameter names. |
| `fg_muted` | `#9a8f8a` | Prompt path, inactive UI chrome, mode indicators. |
| `fg_subtle` | `#5a4e48` | Indent guides, decorative brackets, gutter annotations. |

### 1.3 Accent

The ember glyph is unchanged from U-1. Two stops are now defined: the full
ember for the active cursor and `⊢` glyph, a dimmer variant for the inactive or
insert-mode cursor.

| Role | Hex | Description |
|---|---|---|
| `ember` | `#e07840` | Prompt `⊢`, cursor, active indicator. The fire. |
| `ember_dim` | `#b85f2a` | Insert-mode cursor, secondary ember uses. |

### 1.4 Syntax colours

These apply at the host-terminal / editor layer (Ghostty + Helix). Utopia's own
programs remain under the four-colour discipline of §7; the syntax layer exists
for third-party code rendered by the host editor.

Each colour is named for a natural material in the same world as the background —
not arbitrary hue names. All are deliberately desaturated: they read as tones
rather than colours, so extended sessions accumulate warmth rather than visual
noise.

| Name | Role | Hex | Semantic use |
|---|---|---|---|
| `slate` | keyword / info / blue ANSI | `#8a9ac8` | Keywords, control flow, storage modifiers |
| `sage` | type / teal ANSI | `#8ab8a8` | Types, structs, primitives, import statements |
| `sand` | member / warning / amber ANSI | `#c8a882` | Struct fields, object members, attributes |
| `moss` | constant / green ANSI | `#b8d098` | Constants, macros, enum variants |
| `ash` | function / identifier | `#b07060` | Function names, identifiers, git hashes |
| `dusk` | string / purple ANSI | `#a898c8` | String literals, interpolations |
| `smoke` | comment | `#7a8a7a` | Line and block comments |

### 1.5 Diagnostic colours

These are the only colours carrying urgency. They are deep rather than vivid —
designed to register as states, not alarms.

| Name | Role | Hex | Use |
|---|---|---|---|
| `cinnabar` | error | `#c06050` | Compile errors, failing checks, dangerous operations |
| `sand` | warning | `#c8a882` | Warnings, caution — shared with the member role |
| `fen` | success / ok | `#6a9a6a` | Passing checks, successful operations |
| `slate` | info | `#8a9ac8` | Informational output — shared with the keyword role |

The sharing of `sand` between member and warning, and `slate` between keyword
and info, is intentional. The palette stays small; context disambiguates
semantic meaning. A warning in a build log and a struct field in source code do
not appear in the same visual frame at the same time.

---

## 2. The `⊢` glyph — U+22A2 RIGHT TACK

The prompt glyph. Unchanged from U-1. Unicode codepoint `U+22A2` RIGHT TACK
(the turnstile). The byte sequence in UTF-8 is `E2 8A A2`.

Rationale for the choice (preserved from U-1 for the record):

- **Formal meaning.** In sequent calculus and proof theory, `⊢` reads as
  "proves" or "yields." The shell yields commands to the kernel; every
  invocation is a derivation. The glyph names the act.
- **Visual reading.** A horizontal T — for Thylacine.
- **Uniqueness in terminal-land.** No widely-used shell uses this character as
  its prompt glyph. It is unoccupied, so it carries Thylacine's identity
  uniquely.
- **Renders at all sizes.** A simple two-stroke shape; clean in any monospace
  typeface, distinguishable at the smallest terminal sizes.

In U-2 the glyph has gained a background that earns its name. The `ember`
colour no longer contrasts against cold void — it belongs to the warm dark
around it, the way a coal belongs to the air of the room rather than sitting
exposed under a white light. The visual relationship between glyph and
background is now chromatic kinship rather than pure contrast.

---

## 3. Prompt format

```
~/src/thylacine ⊢ command argument
^─────────────^ ^ ^──────────────^
  fg_muted      │     fg
              ember
```

Three segments, identical in structure to U-1:

| Segment | Role | Content |
|---|---|---|
| Path | `fg_muted` (`#9a8f8a`) | Current working directory, `~`-abbreviated. |
| Glyph | `ember` (`#e07840`) | Single character `⊢` flanked by single spaces. |
| Command | `fg` (`#e4ddd8`) | The user's editable command line. |

The path recedes further in U-2 than in U-1: `#9a8f8a` against `#0e0c0c` is a
warmer and slightly lower-contrast recession than `#8898b4` against `#0e1018`
was. The command line reads as more prominent by comparison, which is correct —
the content is the content.

### 3.1 Path abbreviation

Same rules as U-1:

- Exactly `$home` → `~`.
- A path under `$home` → `~/<relative>`.
- A path under a bound namespace name → the bound name as the path prefix.
- An absolute path elsewhere → the full path, with `…` truncating the middle
  if the path exceeds a terminal-width-derived budget.

### 3.2 Multi-line continuation

When the command line wraps or continues (unbalanced brackets, trailing `\`),
subsequent input lines use a continuation glyph in place of the path:

```
~/src/thylacine ⊢ {
                ⋮ for (f in *.md) {
                ⋮     wc -l $f
                ⋮ }
                ⋮ }
```

Continuation glyph: `⋮` (U+22EE VERTICAL ELLIPSIS) at `fg_muted` (`#9a8f8a`).
Indented so the next character aligns under the original command start.

### 3.3 Prompt as a function

The prompt is generated by the `prompt` shell function, declared in
`~/.config/utopia/utopia.rc`. The shipped default:

```
fn prompt {
    palette path (pwd)
    palette glyph ' ⊢ '
}
```

Users override `prompt` to add segments; their additions are not bound by the
Utopia palette discipline.

---

## 4. The `palette` builtin and `libutopia::palette`

The `palette` shell builtin and the matching `libutopia::palette` Rust module
are the canonical entry points for emitting palette-coloured text. They handle:

- ANSI 24-bit colour escape emission (`ESC[38;2;R;G;Bm` foreground, `ESC[0m`
  reset).
- The length-doesn't-count markers that the line editor needs for cursor
  accounting.
- The colour names → RGB resolution table.
- The trailing reset after every coloured emission.

### 4.1 Colour names

Programs refer to colours by role, not hex. The role table is the single source
of truth. The Utopia discipline roles (`bg`, `fg`, `path`, `glyph`) are the
only roles that Utopia's own programs use; the extended palette roles
(§1.4–1.5) are exposed in `libutopia::palette` for third-party and host-editor
integration but are not introduced into the Utopia shell surface. The **one
exception** (added for `ut` at #115c) is the shell's command-line *validity*
coloring — a live typing affordance, distinct from error output — which renders
a resolvable command in `fen` and an unresolvable one in `cinnabar` (see §8).

| Role name | Constant in `libutopia::palette` | Hex |
|---|---|---|
| `bg` | `palette::BG` | `#0e0c0c` |
| `fg` | `palette::FG` | `#e4ddd8` |
| `path` | `palette::PATH` | `#9a8f8a` |
| `glyph` | `palette::GLYPH` | `#e07840` |
| `surface` | `palette::SURFACE` | `#180f0e` |
| `gutter` | `palette::GUTTER` | `#2a1f1c` |
| `fg_dim` | `palette::FG_DIM` | `#c8bdb8` |
| `fg_muted` | `palette::FG_MUTED` | `#9a8f8a` |
| `fg_subtle` | `palette::FG_SUBTLE` | `#5a4e48` |
| `ember_dim` | `palette::EMBER_DIM` | `#b85f2a` |
| `slate` | `palette::SLATE` | `#8a9ac8` |
| `sage` | `palette::SAGE` | `#8ab8a8` |
| `sand` | `palette::SAND` | `#c8a882` |
| `moss` | `palette::MOSS` | `#b8d098` |
| `ash` | `palette::ASH` | `#b07060` |
| `dusk` | `palette::DUSK` | `#a898c8` |
| `smoke` | `palette::SMOKE` | `#7a8a7a` |
| `cinnabar` | `palette::CINNABAR` | `#c06050` |
| `fen` | `palette::FEN` | `#6a9a6a` |

### 4.2 Shell-builtin shape

```
palette ROLE TEXT...
```

Emits `TEXT...` (concatenated with spaces) in the named role's colour, followed
by a reset. The `ROLE` argument accepts any name from the table in §4.1.

### 4.3 Rust API shape

```rust
// in libutopia::palette
pub fn emit_fg(role: Role, text: &str);    // foreground in role's colour + reset
pub fn emit_bg(role: Role, text: &str);    // background in role's colour + reset
pub fn fg_seq(role: Role) -> &'static str; // raw ANSI escape; caller emits its own reset
pub fn reset() -> &'static str;            // ESC[0m
```

`Role` is the enum covering all names in §4.1.

### 4.4 Capability detection

V1 does not attempt terminal-capability detection. The escape sequences are
emitted unconditionally. Users running Thylacine via UART or SSH are expected
to use a 24-bit-colour-capable terminal (Ghostty, iTerm2, Kitty, foot,
Alacritty, WezTerm).

Capability detection (downgrade to 8-colour, no-colour fallback) is a v1.x
consideration.

---

## 5. Host terminal and editor configuration

Thylacine ships terminal configuration files at `share/terminal-configs/` that
pin the Bonfire palette and configure the recommended typeface. The following
formats are shipped at U-2:

| File | Target |
|---|---|
| `share/terminal-configs/utopia.conf` | Ghostty |
| `share/terminal-configs/utopia.toml` | Helix |

### 5.1 ANSI 16-colour slots

Ghostty (and any terminal consuming the ANSI palette) maps the Bonfire colours
to the 16 ANSI slots as follows. The bright variants are light-shifted versions
of their normal counterparts, ensuring graceful degradation in programs that
use bold-as-bright rather than truecolor.

| Slot | Name | Hex | Bright slot | Hex |
|---|---|---|---|---|
| 0 (black) | gutter | `#2a1f1c` | 8 (bright black) | `#5a4e48` |
| 1 (red) | cinnabar | `#c06050` | 9 (bright red) | `#d07868` |
| 2 (green) | fen | `#6a9a6a` | 10 (bright green) | `#82b882` |
| 3 (yellow) | sand | `#c8a882` | 11 (bright yellow) | `#e4ddd8` |
| 4 (blue) | slate | `#8a9ac8` | 12 (bright blue) | `#a8b8e0` |
| 5 (magenta) | dusk | `#a898c8` | 13 (bright magenta) | `#c0b0e0` |
| 6 (cyan) | sage | `#8ab8a8` | 14 (bright cyan) | `#a8c8b8` |
| 7 (white) | fg_dim | `#c8bdb8` | 15 (bright white) | `#e4ddd8` |

---

## 6. Typeface

The recommended typeface for the Utopia experience is **PragmataPro Mono**.
Its narrow-condensed metrics, comprehensive Unicode coverage including U+22A2
and U+22EE, and ligature support match Bonfire's intent.

Distribution is the user's responsibility at v1.0. PragmataPro is commercial
software (FSD Type Foundry; see https://fsd.it/shop/fonts/pragmatapro/).
Thylacine does not bundle it. Users who hold a PragmataPro license configure
their host terminal to use it.

The bundled default for Halcyon (Phase 8) will be an **Iosevka Term** build
tuned toward PragmataPro's narrow, geometric aesthetic via a custom TOML build
plan. Iosevka is OFL-licensed, ships with full coverage of the required
codepoints including `U+22A2` and `U+22EE`, and its build system allows
close metric and weight matching to PragmataPro. PragmataPro remains a
documented opt-in for users who hold a license.

Until Halcyon ships, the visual identity expresses through ANSI bytes sent to
the host terminal; the host's font choice is up to the user.

---

## 7. Sixel and inline graphics

Unchanged from U-1. Sixel emission and the Kitty graphics protocol are deferred
from U-2 scripture and tracked as a Utopia v1.x extension. Halcyon (Phase 8)
implements the decoder side of both protocols.

---

## 8. Discipline summary

Bullet list for fast review:

- Utopia's own programs (`ut`, coreutils, libutopia-consumers) use four roles
  only: `bg`, `fg`, `path`, `glyph`. The discipline is unchanged from U-1.
- The extended palette (§1.1–1.5, all nineteen roles) is available via
  `libutopia::palette` for host editor integration and user programs; it does
  not enter the Utopia shell surface.
- The path segment is the only `path`/`fg_muted`-coloured text in disciplined
  programs; everything else is `fg` except the `⊢` glyph.
- The `⊢` glyph is the only `ember`-coloured text in disciplined programs.
- Errors are not colour-coded in disciplined programs. They are context-coded.
  The one exception is the shell's command-line *validity* coloring (#115c): a
  resolvable command token renders `fen`, an unresolvable one `cinnabar`. This
  is a live typing affordance (computed before the command runs), not error
  output — it is the sole disciplined-surface use of the diagnostic palette.
- Continuations use `⋮` in `fg_muted`.
- Programs reach the palette via `libutopia::palette` (Rust) or the `palette`
  shell builtin.
- No terminal capability detection at v1.
- No Sixel at U-2.
- No bundled font at v1; user configures host terminal.
- The U-1 hex values (`#0e1018`, `#d8e4f4`, `#8898b4`) are retired. Any
  hardcoded occurrence of these values in source is a U-1 residue and should
  be migrated to the role constants.

---

## 9. Open design questions

None at U-2.

---

## 10. References

- `docs/UTOPIA.md` — the top-level Utopia experience.
- `docs/UTOPIA-SHELL-DESIGN.md` — the shell design that consumes this scripture.
- `docs/ARCHITECTURE.md §23` — POSIX surfaces and the Utopia milestone.
- `docs/ROADMAP.md §8` — the Utopia execution phase.
- `share/terminal-configs/utopia.conf` — Ghostty configuration.
- `share/terminal-configs/utopia.toml` — Helix theme.
