---
id: fnd-pounce-p5-f1
type: fnd
title: "P-5 F1: latched-fallback resolution pays a double base X-check Tgetattr"
round: adt-pounce-p5
severity: P3
status: deferred
surface: [sub-kernel-stalk]
threatens: []
seam: seam-372-latched-double-xcheck
created: 2026-08-01
---
## Prosecution

On a `wga_unsupported`-latched session (netd's `/net`) the pounce block
pays its base X-check `stat_native` BEFORE `walk_attrs` returns the
sentinel; the `per_component` fall-through then X-searches the SAME
parent again — 2 Tgetattr per component where the pre-POUNCE loop paid
1. No correctness or safety impact (idempotent; a racing chmod only
tightens).

## Disposition

Deferred → [[seam-372-latched-double-xcheck]] (task #372): the reorder
(base X-check after the sentinel arm — sound, walks are
side-effect-free) was declined AT the close to avoid churning the
freshly-audited deny/release ladder of the round's #1-target block
right after the 40/40 gate. Re-verified still present in current code
at the vault sweep.
