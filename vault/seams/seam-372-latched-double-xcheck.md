---
id: seam-372-latched-double-xcheck
type: seam
title: "Latched-fallback pounce pays a double base X-check Tgetattr"
status: open
surface: [sub-kernel-stalk]
opened-by: fnd-pounce-p5-f1
tracker: "task #372"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

On a `wga_unsupported`-latched session (netd's `/net`), the pounce block
pays its base X-check `stat_native` BEFORE `walk_attrs` returns the
sentinel, then the `per_component` fall-through X-searches the same
parent again — 2 Tgetattr per component where the pure loop pays 1.
Idempotent (a racing chmod only tightens); no correctness impact.

## What closes it

Reorder the base X-check AFTER the sentinel arm (sound per
POUNCE-DESIGN §6 — walks are side-effect-free). Deferred at the P-5
close to avoid churning the just-audited deny/release ladder of the
audit's #1-target block right after the clean round + the 40/40 gate.

## Risk while open

Cost only: `/net`-class paths are shallow setup-only ops, so the wasted
RPC is marginal. A future latched HIGH-TRAFFIC server would make it
worth the reorder.
