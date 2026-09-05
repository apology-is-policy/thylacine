---
id: chg-2026-09-05-halcyon-session-crates
type: chg
title: "The KT-1 session-compositor crates dossiered: sub-halcyond + sub-kaua-term (from the 150/152 reference prose)"
date: 2026-09-05
arc: arc-vault
commits: []
touched: []
established:
  - sub-halcyond
  - sub-kaua-term
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-05
---
The two UNOWNED KT-1 surfaces main flagged at the audit-close handoff (yip
0052, 0037-0041) get dossiers. Their source is the reference prose main wrote
for the unowned crates -- `docs/reference/150-halcyond.md` (the whole crate,
esp. the session-compositor section) + `docs/reference/152-kaua-term.md` (new)
-- with every load-bearing figure re-verified against the code.

## sub-halcyond (audit: hard, guarded-by [], moc-userspace-shell-tui)

The Halcyon environment client, in both roles: the joey-spawned console
transcript renderer (H-2, the CPU floor) and the login-spawned per-user
session compositor (`--session`, KT-1.5d). 19 code entries (18 modules +
Cargo.toml). It is the format-fuzz frontier for the display -- every rendered
byte is untrusted app output -- so audit:hard with the security posture
prosecuted in prose (guarded-by [] because it is a userspace client that
UPHOLDS, not enforces, §28 invariants; the audit anchors are the H-2 / H-3b /
H-3c / H-3d / KT-1 trigger rows + adt-kt1-r{1,2,3}). Covers: the two roles +
the lib/bin split; the console data flow; the session loop (the declared seat +
the takeover rule + the undeclared fallback [seam-login-halcyond-fallback]; the
tile model + the untrusted record stream + the grid OOB-drop containment; death
containment); chrome/menu/status; the event set (one SQPOLL EventRing); the
load-bearing invariants (streaming property, robustness, span hygiene, the
budgets); the windowed render (haz-budget-stored-not-derived). Carries
`haz-budget-stored-not-derived` in hazards.

## sub-kaua-term (audit: hard, guarded-by [], moc-userspace-shell-tui)

The crash-isolated per-tile terminal: one process per tile, holding the pts,
running the vt parser over the app's output, shipping halcyond a pre-digested
RECORD stream. audit:hard -- it is the format-fuzz untrusted-parse ISOLATOR
(the authority lives in its own files: the codec bounds, the per-class record
bounds, the crash-isolation), all three files on the `KT-1: the kaua-term seam`
trigger row. Consistent with [[sub-lib-vt]] (the parser itself) staying light:
the parser is a pure lib, the isolator + bounds are the security mechanism.
Covers the record stream + boundary order, the per-CLASS bounds (`scroll_cap`,
`feed_into`'s sink -- the amplifier defects), the alt-screen one-grid diffs, the
resize ordering, the master-write lock, the wire codec (`MAX_FRAME`/`MAX_TITLE`/
`TooLarge`).

## Figures re-verified, not copied (the "re-verify" discipline)

- sub-kaua-term: **30 host tests** (lib 23 + wire 7), not the reference's 28 --
  rounds 2-3 grew it.
- sub-halcyond: **99 `#[test]` across the twelve lib modules**, not the
  reference's 55 -- the KT-1 rounds grew tile.rs (13), transcript (23), tiles
  (8), grid (6), downq (4). The constants confirmed in code:
  `DOWN_PENDING_MAX`=4096, `SESSION_SCROLLBACK_BUDGET`=32 MiB, `POLL_MAX_NFDS`=64,
  `DECLARE_TRIES`=40, `OPEN_BLOCK_MAX_COST`=512 KiB; kaua-term's
  `SCROLL_ACC_BYTES`=256 KiB, `MAX_FRAME`=4 MiB, `MAX_TITLE`=256,
  `ThylaAllocN<32 MiB>`.

## Owed, not done here

The battery stays `seam-tapestry-battery-unowned` (its legs are described in
the sub-tapestryd Tests paragraph, not its own dossier -- main's direction).
The existing `fnd-kt1-*` notes already cover the audit findings; they were
authored while the surfaces were unowned and are left as-is (Record plane,
append-only) -- the new dossiers reference the adt/fnd records in prose rather
than re-homing them.
