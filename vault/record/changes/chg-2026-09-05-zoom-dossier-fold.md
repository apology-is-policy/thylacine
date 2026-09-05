---
id: chg-2026-09-05-zoom-dossier-fold
type: chg
title: "The fullscreen-zoom fix folded into the dossiers: the #56-latch discriminator (sub-tapestryd), set_single_slot (sub-libtapestry), place.rs (sub-libhalcyon)"
date: 2026-09-05
arc: arc-vault
commits: ["7ce2bb26"]
touched:
  - sub-tapestryd
  - sub-libtapestry
  - sub-libhalcyon
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-05
---
main's fullscreen-zoom fix (`f25781ad`, [[chg-2026-09-05-fullscreen-zoom]])
touched three vault-owned crates; its chg carried `no-dossier-change` deferring
the dossier edits to the vault peer (yip 0052 turn 9). This is that fold, from
main's records ([[fnd-zoom-r1-f1]] + [[haz-latch-keyed-on-proxy]]), verified
against the landed code -- the KT-1 inheritance pattern.

## The three folds

- **[[sub-tapestryd]]** -- the #56-latch discriminator section (the paragraph
  promised on yip 0052). The latch keyed on damage COVERAGE as a PROXY for slot
  ROTATION; a single-slot SDL client (DOSBox-X) presenting partial rects
  satisfied the proxy but not the property and was cropped at the content origin
  instead of letterboxed. Re-keyed on `Surface.slots_presented` (partial damage
  AND >= 2 distinct slots), with the letterboxed compose clipping to the
  damage's projection (`ComposeOp.clip` from `libhalcyon::place::scaled_clip`;
  `compose_cpu` pushes `op.clip.intersect(op.dst)`, the GPU path unchanged).
  Verified in server.rs: `slots_presented: u32` (932), the re-key doc (912-923),
  `ComposeOp.clip` (4327). Carries the class [[haz-latch-keyed-on-proxy]] and
  the `singleslot` regression; the double-the-distance deferral to AUDIT-TRIGGERS
  row 42 recorded.
- **[[sub-libtapestry]]** -- `Surface::set_single_slot` (the client-side
  single-slot DECLARATION that answers the latch honestly; `single_slot` field,
  the present path stops rotating). Verified: lib.rs 251/533/820.
- **[[sub-libhalcyon]]** -- `place.rs` added to the code list + the module list +
  a section: `letterbox` / `nearest_src` / `scaled_clip`, the shared placement +
  scale math (moved from tapestryd so the battery's sample points derive from
  the compositor's own function; `scaled_clip` built on the `nearest_src` the
  compose samples by, so a clipped compose is pixel-identical, host-tested).
  Verified: place.rs 16/41/52 + the host tests.

## Not a re-derivation

The fix's own narrative, class, and regression are main's ([[fnd-zoom-r1-f1]],
[[haz-latch-keyed-on-proxy]], [[chg-2026-09-05-fullscreen-zoom]]); this chg only
brings the three dossiers current with the landed code. The real-DOSBox re-run
(aux's `dx-fullscreen-repro.exp`, fixture on aux-3) is owed to aux, per main's
record -- not a vault item.
