---
id: chg-2026-09-07-aurora-doc-absorb
type: chg
title: "absorb docs/reference/140-aurora (the fbcon + /dev/cons drain/feed, G-4): clean redirect + sharpen the cfg-3 F1 attribution in sub-aurora"
date: 2026-09-07
arc: arc-vault
commits: ["660c90c4"]
touched: [sub-aurora]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-07
---
Aurora (the console renderer/fbcon + the /dev/cons drain/feed backend, G-4).
Audit-trigger surface. quaestor owner: usr/aurora (main/render/osd/config -- ALL 4
source files) -> sub-aurora (audit:hard, fresh 2026-09-05); kernel/cons.c ->
sub-kernel-cons (audit:hard, fresh). Verified atom-by-atom.

NOTE ON A PHANTOM: the doc names a "vt.rs" MODULE, but no usr/aurora/src/vt.rs file
exists -- aurora has exactly 4 source files and sub-aurora claims all 4; the VT/OSC
logic (osc_end, the Vt struct, the palette) lives in main.rs + render.rs. quaestor's
"4 of 4 files claimed" was the tell. So aurora is FULLY owned -- no ownership gap.
(Initial fold tried to claim the phantom vt.rs; the pre-commit lint hook caught the
non-existent path -- corrected before commit.)

ALREADY COVERED (verified): the three console roles (ATTACH/OWNER/RENDERER, renderer
confers no elevation/interrupt, single-holder NULL-only claim race close) + the
drain-drops-oldest + never-consults-the-role + is_break-false SAK guard + the tee ->
sub-kernel-cons; the devdev leaves (consdrain/consfeed gated-at-open + re-gated-per-op,
O_PATH/CWALKONLY, POLLNVAL) -> sub-kernel-devdev; the held-input #129 + bounded-wait
#135, the #35 lane-safe blend (edge-pixel-only corruption the near-grey gate missed),
the #31 dropped-frame-not-death, the cfg-2a strict-fsync persistence, the OSC-channel
must-not-gain-authority -> sub-aurora; the shared-VT reject -> sub-lib-vt.

THE ONLY CHANGE (attribution sharpen -> sub-aurora; updated 09-05 -> 09-07): the cfg-3
F1 OSC-newline-launder audit P1 was captured as a property but ATTRIBUTED to sub-lib-vt
-- aurora has its OWN OSC handler (osc_end, in main.rs) doing the receiving-end
control-byte reject (b<0x20), because config::parse re-splits an OSC value on .lines()
so an embedded newline laundered a second statement past the single-token allowlist;
the parser is the trust boundary (a documented tool splits into clean single-line OSCs,
which is why a tool-driven test missed it; closed by the osc-newline-attack fixture).
Sharpened to aurora's own osc_end; sub-lib-vt noted as the shared-library twin.

Redirect stub. Zero code change.
