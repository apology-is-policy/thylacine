---
id: chg-2026-09-05-h4d2-family-fold
type: chg
title: "H-4d-2/2a/3 fold: the rich session tile -- beacon-tier plumbing (DC_PTS, stdout_is_terminal, env_beacon_tier) + the span-serial Beacon threading + the tile Normal mode / menu / selection"
date: 2026-09-05
arc: arc-vault
commits: ["83ce0206"]
touched:
  - sub-beacon
  - sub-kernel-syscall-abi
  - sub-kernel-syscall-dispatch
  - sub-utopia-interactive
  - sub-substrate-build
  - sub-lib-vt
  - sub-kaua-term
  - sub-halcyond
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-05
---
Folds the H-4d-2 family -- three main-track chgs that landed together in one merge
(the peer's `f626fe04`, merged here at `7ab2257d`) and form one feature, the rich
session tile: [[chg-2026-09-05-h4d2-tile-menu]] (the tile Normal mode + menu +
selection + the cell-span model), [[chg-2026-09-05-h4d2a-rich-tiles]] (the pts
render-tier plumbing), and [[chg-2026-09-05-h4d3-welcome]] (the welcome + a
populate step). Each carried `no-dossier-change` naming its delta with "the vault
peer folds these" -- the KT-1 inheritance pattern. One combined fold-chg rather
than three source-aligned ones because the family is coherent and the dossiers
overlap (halcyond in all three); every dossier is edited exactly once. Every named
symbol was verified in the landed code before folding, not taken from the chg text.

## The beacon-tier plumbing (h4d2a + h4d3)

A pts slave is a terminal too. The through-line: a session tile's `kaua-term`
declares its render tier, the kernel carries it, and the shell it hosts inherits
it -- so a program in a tile gets rich Beacon output exactly as one on the console.

- **[[sub-beacon]]**: the Auto emission gate now admits `DC_CONSOLE` (`'c'`) OR
  `DC_PTS` (`'t'`), not the console alone. Folded into the fail-closed gate prose.
- **[[sub-kernel-syscall-abi]]**: the "is-a-terminal is exactly `dc == 'c'`" line
  was stale -- it is `'c' || 't'` now, via the one libthyla-rs wrapper
  `stdout_is_terminal()`.
- **[[sub-kernel-syscall-dispatch]]**: `sys_fd_devclass_handler`'s inline
  class-pick became the named classifier `spoor_devclass` (the pts-slave `'t'` arm
  via `pts_resolve_spoor`; master stays `'9'`). +18 lines -> the file is 14749, not
  the 14731 [[chg-2026-09-05-syscall-dispatch-census]] measured (that census
  predated this merge). NOT a new split -- a classifier, not a `_handler`/`_for_proc`
  pair -- so the 50/45 metric is unchanged. The line count was corrected in both
  the opening claim and the Provenance history; the census figure itself is left as
  what that chg found.
- **[[sub-utopia-interactive]]**: `ut` reads `/env/BEACON` (the pts host's word,
  deep-copied at spawn) and arms transcript zones iff `rich` AND
  `stdout_is_terminal()`; decided once at startup, no per-prompt re-read. The
  dossier had no beacon-tier coverage; added under the `ut`-owns-descriptors seam.
- **[[sub-substrate-build]]**: the `corvus-mint` host tool builds from the repo
  ROOT (cargo config discovery is cwd-based; from `usr/` the vendor `aegis 0.9.8`
  cannot satisfy the tool's `0.9.12` lock -- a bake started there fails on the
  mint). Added as a trip-hazard. The `/lib/halcyon/layouts/default` populate step
  is below the target/ledger granularity -- noted, no target-set change.

## The span-serial Beacon threading (h4d2)

A session tile renders Beacon presentation over a pure cell grid without a second
parser (R5). The mechanism spans two dossiers:

- **[[sub-lib-vt]]**: a monotonic `span_serial` advanced only by an OSC 1936
  (Beacon) frame, copied into `Vt.span`, inherited by every `Cell.span`; other OSCs
  (titles) do not advance it. The raw body rides the new `Boundary::Osc { serial,
  body }`; the parser never reads a Beacon body. Added as a Mechanism subsection.
- **[[sub-halcyond]]**: the tile keeps a `SpanMap` (8192-entry ring, `serial ->
  SpanTag { block, obj, em, hdr }`) noted as it feeds the same frames in order, so
  a cell recovers its presentation from its serial however late it scrolls off.
  `push_scrolled_rows(rows, &spans)` + `local_obj` carry it into scrollback;
  `grid_runs`/`grid_run`/`grid_hit` + `select::flatten_with_grid` (`GRID_BLOCK`) are
  the readers. Folded into the tile-model section (which already named the
  Osc1936Raw feed from the KT-1 arc but not the SpanMap or the grid rendering).

## The tile Normal mode, selection, and menu (h4d2 + h4d2a)

- **[[sub-kaua-term]]**: the DOWN channel gained `Input::Text(Vec<u8>)` (a byte run
  written to the master verbatim, not re-encoded -- the tile menu's `^E^U<cmd>\n`);
  the UP `Control` gained `Osc1936Raw { serial, frame }`; the wire cell is 17 bytes
  now (`ch/fg/bg` u32, `attrs` u8, `span` u32); `--beacon <tier>` writes the tile's
  own `/env/BEACON` before the slave spawn.
- **[[sub-halcyond]]**: a new subsection for the session tile's modal interaction --
  `Insert`/`Normal` (Esc enters Normal only on the VT's normal screen), `normal_input`
  navigation + `Sel` selection (the band + ember underline via `render(.., &mut
  scroll_up, Option<Mark>)`), `click` -> `Tile::hit`/`grid_hit` via `Tile.frame`, the
  verb menu summoned at display coordinates through the H-3c-2 `MenuSet` on the
  session's ring (`menu::step_run_with`), and a `--beacon rich` spawn. The welcome
  (h4d3) is "none in code" for this dossier -- the compositor hosts the tag by
  H-4d-1's rules; usr/halcyon (unowned) + 150-halcyond.md carry it -- so no edit.

`updated:` -> 2026-09-05 on the two dossiers still older (utopia-interactive,
substrate-build); the other six were already dated today (this session's de-stales
plus the KT-1 arc) and gained the missing h4d2 content in place.
