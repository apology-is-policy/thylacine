---
id: seam-tapestry-battery-unowned
type: seam
title: "the compositor acceptance battery (tapestry-battery + ls-gfx-panes.exp) is UNOWNED -- no dossier"
status: open
surface: [sub-tapestryd]
opened-by: chg-2026-09-02-h4b1-claim
tracker: "H-4b-1 placement-claim doc pass, 2026-09-02"
created: 2026-09-02
updated: 2026-09-02
---
## Owed

`usr/tapestry-battery/src/main.rs` (the G-6 compositor acceptance battery:
one process hosting the synthetic clients + the layout driver, ~1300 lines
of in-guest asserts) and `tools/interactive/ls-gfx-panes.exp` (its host
half: the QMP-typed input + the pixel dumps + the expect sync points) are
the gate for the pane tree, and `quaestor owner` reports both UNOWNED: no
`sub` dossier names them in `code:`, so the doc-update cutover routes their
prose to `docs/reference/139-tapestryd.md` (the gate paragraph under the
G-6a section) rather than the vault. The H-4b-1 placement-claim leg landed
there. This seam records that routing so a future coverage sweep ratifies
it rather than re-discovering it at each doc pass -- the same class as
[[seam-warp-prove-unowned]] (the Warp prover), and the same decision.

## What closes it

A sweep decision, one of two:

1. **Accept test-only** (the likely call, matching the prover). The
   battery is a test harness whose discrimination logic documents itself
   where it runs; its as-built legs belong in the reference doc beside the
   surface they exercise. Close WONTFIX with that rationale.
2. **Grant a dossier.** If the battery's own guarantees (each leg's
   fails-without-the-fix discrimination) become a surface worth pinning
   independently, mint `sub-tapestry-battery` and add
   `usr/tapestry-battery/src/**` + `tools/interactive/ls-gfx-panes.exp` to
   its `code:`.

## Risk while open

Low. The reference coverage exists (139-tapestryd.md names every leg);
nothing is undocumented. The standing cost is only that `quaestor owner`
keeps routing edits to the reference doc -- the correct answer under
option 1. One instrument caveat surfaced by this pass: `quaestor stale`
measures against the checkout it runs in, and the vault worktree lags
main (its `server.rs` predates H-3d), so a dossier reads as current while
main's code has moved under it -- the H-3 chrome/menu/status mechanisms
are documented in 139-tapestryd.md and NOT in `sub-tapestryd`, and
`stale` did not flag it. A sweep should re-run `stale` from a checkout
that carries main.
