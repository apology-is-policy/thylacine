---
id: abi-halcyon-palette
type: abi
kind: contract
stability: internal
title: "The /env/HALCYON_PALETTE contract -- the session's role=RRGGBB palette for hosted programs"
pinned-by:
  - "usr/lib/libhalcyon/src/theme.rs (env_palette -- the role vocabulary + the RRGGBB format + the surface=header mapping)"
mirrors:
  - "usr/halcyond/src/session.rs (the writer -- HALCYON_PALETTE_ENV_PATH, daylight_env_palette)"
  - "usr/nora/src/theme.rs (a reader -- Palette::with_overrides, the role->field map)"
  - "usr/nora/src/main.rs (a reader -- adopt_session_palette)"
literals:
  - "HALCYON_PALETTE"
literal-scan:
  - "usr"
created: 2026-09-06
updated: 2026-09-06
---
## The contract

A Halcyon session publishes its resolved theme palette to the per-Proc
environment device at `/env/HALCYON_PALETTE`, so a program hosted in a session
tile follows the session's theme instead of painting a hardcoded palette on the
session's ground. It is a **program-agnostic** contract: the session names
semantic ROLES and their colours; a hosted program maps those role names onto
its own fields. (The write side is the v1.x seam `vt`'s palette comment named --
the compositor plumbs its resolved palette to the programs it hosts.)

**Format.** Newline-separated `role=RRGGBB`, one role per line: the role name,
`=`, then exactly six lowercase hex digits (the opaque alpha byte of the source
`Argb` is dropped). A reader ignores unknown role names, malformed hex, comment
lines (`#...`) and blanks -- a hostile or partial source degrades to the roles it
could parse, never a failure. The same `role=RRGGBB` grammar is reused by a
program's own palette dotfile (e.g. nora's `$HOME/.config/nora/palette`).

**The 11 roles** (the `env_palette` set, `libhalcyon::theme`):

| role | source (`DAYLIGHT` field) | meaning |
|---|---|---|
| `bg` | `surface` | the page ground |
| `fg` | `fg` | primary ink |
| `dim` | `fg_muted` | de-emphasised ink |
| `accent` | `ember` | the highlight / selection accent |
| `surface` | **`header`** | a lifted PANEL a program paints its own dark ink on |
| `border` | `border` | rule / divider |
| `moss` `dusk` `sand` `slate` `cinnabar` | `syntax.*` | the five syntax classes |

**The one mapping worth pinning: `surface` resolves from `Theme.header`, NOT
`status_bg`.** The `surface` role is a lifted panel a hosted program paints its
own dark ink on (nora's status bar, popups, current-line). `status_bg` is
Halcyon's OWN dark bottom strip, worn with the light `status_fg`; a program that
painted its `fg` on it would render dark-on-dark. `header` is the light lift that
keeps the contrast. A reader that re-derives `surface` from `status_bg` breaks
this and produces unreadable panels.

## Who pins it, who mirrors it

`libhalcyon::theme::env_palette(theme)` is the format authority: it defines the
role vocabulary and emits the text; `daylight_env_palette()` is
`env_palette(&DAYLIGHT)`. It is host-tested against the `DAYLIGHT` scripture
([[sub-libhalcyon]]).

- **Writer:** [[sub-halcyond]] writes `daylight_env_palette()` to
  `/env/HALCYON_PALETTE` at session start, beside `/env/HALCYON_SESSION`,
  best-effort (an unset value just leaves a hosted program on its own default).
- **Readers:** [[sub-nora-view]] (`theme::Palette::with_overrides` maps each role
  name to a nora field) and [[sub-nora-host]] (`adopt_session_palette` reads
  `/env/HALCYON_PALETTE`, then the dotfile, over `BONFIRE`).

## Stability

`internal` -- a v1.0 cross-program contract inside the Thylacine userspace, not a
frozen or foreign ABI. The role set may grow (a new role is additive: an old
reader ignores it, degrading to its default for that field), and more hosted
programs may become readers. What must not change silently is the `role=RRGGBB`
grammar and the meaning of an existing role name -- the `surface=header` mapping
in particular. A second theme (Frutiger Aero, deferred) publishes the same role
names with different colours; the contract is the vocabulary, not the values.

## Change discipline (R6)

The literal `HALCYON_PALETTE` is scanned across `usr/`: every file that names it
is either the `pinned-by` authority or a declared `mirror`. A new reader or
writer that touches the string must join the `mirrors` set in the same change,
and the change note must carry `mirrors-checked`. A new role is added in
`env_palette` (the authority) first; a reader picks it up by adding a
`with_overrides` arm.
