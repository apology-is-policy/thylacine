---
id: chg-2026-09-06-s7a-palette-destale
type: chg
title: "s7a nora session-palette de-stale (peer-flagged, yip 0067): 4 dossiers follow the palette-as-runtime-value after main's s7a arc merged, + a new abi-halcyon-palette contract note for the /env/HALCYON_PALETTE cross-program surface"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-libhalcyon
  - sub-halcyond
  - sub-nora-view
  - sub-nora-host
established:
  - abi-halcyon-palette
closed: []
opened: []
mirrors-checked:
  - "usr/lib/libhalcyon/src/theme.rs (pinned-by: env_palette, the format authority)"
  - "usr/halcyond/src/session.rs (mirror: HALCYON_PALETTE_ENV_PATH writer)"
  - "usr/nora/src/theme.rs (mirror: Palette::with_overrides reader)"
  - "usr/nora/src/main.rs (mirror: adopt_session_palette reader)"
depth: skeletal
created: 2026-09-06
---
A vault-owned de-stale, not an absorption. Main's nora s7a session-palette arc
(nora follows the Halcyon session theme instead of a hardcoded Bonfire -- the
operator's residual s7 P0) landed and merged into this worktree; main FLAGGED
the vault fold on yip 0067 (quaestor owner -> exit 0 -> ring the vault), and the
staleness census named exactly the three dossiers whose code changed. Every atom
verified against the merged code first.

**New: abi-halcyon-palette** (the /env/HALCYON_PALETTE cross-program contract).
A genuinely new cross-program surface -- halcyond WRITES it, nora (and future
pts programs) READ it -- with a defined role vocabulary (11 program-agnostic
roles) and format (`role=RRGGBB`, one per line, opaque alpha dropped, unknown /
malformed / comment / blank ignored). main delegated the call ("may deserve its
own abi note -- your call"); it is the right model for a format contract with a
writer and readers, so authored. R6: literal `HALCYON_PALETTE` scanned across
`usr/` (literal-scan), all four occurrences declared (one pinned-by + three
mirrors) -- mirrors-checked above. `stability: internal` (a v1.0 userspace
contract, role set may grow additively; the `surface=header` mapping must not
change silently).

**sub-libhalcyon** (pinned-by / format authority): `env_palette(theme)` +
`daylight_env_palette()` folded into Contract + Mechanism, with the load-bearing
JUDGEMENT recorded -- the `surface` role resolves from `Theme.header`, NOT
`status_bg`: `surface` is a lifted panel a program paints its own dark ink on;
`status_bg` is Halcyon's own dark strip, so a program's fg on it would be
dark-on-dark. `abis: [abi-halcyon-palette]`.

**sub-halcyond** (writer): the session publishes `daylight_env_palette()` to
`/env/HALCYON_PALETTE` beside `/env/HALCYON_SESSION`, before the first tile
spawn, best-effort (s7a-3). Folded into the session-compositor section.
`abis: [abi-halcyon-palette]`.

**sub-nora-view** (reader) -- a BLAST-RADIUS correction, not just an addition.
s7a made theme.rs a runtime `Palette` behind a `static ACTIVE`
(`UnsafeCell<Palette>`, `unsafe impl Sync`), which VOIDED two standing claims:
Data structures said theme was "colour constants ... no state" and Concurrency
said "no shared mutable state, no interior mutability." Both were rewritten to
the truth: one set-once global cell, sound by a set-once-before-render
discipline (single-threaded; a debug `AtomicBool` asserts at-most-once, compiled
out in release). Added the runtime-palette Mechanism subsection (`active()` /
`set_palette` / `with_overrides` role map / the `debug_bg` re-derivation).
`abis: [abi-halcyon-palette]`.

**sub-nora-host** (reader): `adopt_session_palette` -- start BONFIRE, apply
`/env/HALCYON_PALETTE`, then the `$HOME/.config/nora/palette` dotfile; precedence
**dotfile > /env > BONFIRE** (last-applied wins), set once before the first
render. `abis: [abi-halcyon-palette]`.

No code touched (the code was main's, already merged + audited s7a+F2 clean).
Render clean; lint 0-fail.
