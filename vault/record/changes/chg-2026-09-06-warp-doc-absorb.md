---
id: chg-2026-09-06-warp-doc-absorb
type: chg
title: "absorb docs/reference/149-warp (I-45 GPU seam): zero-fold, multi-redirect across hwcap + tapestryd"
date: 2026-09-06
arc: arc-vault
commits: ["d00bebbf"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---

# docs/reference/149-warp.md -> ABSORBED (I-45 audit-trigger surface)

Absorbed the 2261-line Warp reference doc (the biggest audit-trigger doc) into a
multi-redirect stub. The doc is a Warp-arc as-built log, most of it host-side
(Mesa/Venus/WSI -- the TRUSTED-not-enforced half of I-45 per GPU-DESIGN 9.2),
tooling (warp-prove/warp-host.sh/quarry, the gate + coherent-ring + health
provers), and measured performance (#215, C-4). The load-bearing I-45 halves are
owned across two today-current dossiers, verified atom-by-atom:

- kernel guest half -> sub-kernel-hwcap: the GPU-BO subtype-the-caller-cannot-
  get-wrong, the SYS_DMA_CREATE envelope (reject-at-the-door), and the containment
  arithmetic pinning a GPU-BO Burrow's physical base inside the discovered window
  (inv-i45 -- the base never escapes the BAR).
- compositor/GPU-host half -> sub-tapestryd (guarded-by inv-i45): the /dev/warp
  virgl-context/GPU-BO/3D-submit half that "holds inv-i45", the isolation contract
  F2 (a surface resolves only for its context; a never-reused host resource id
  bounded to one context's worth; wring_kick's cross-ctx rejection), the fenced
  lane, the Venus/WSI integration (V-0..V-3, W-3c/W-3d/W-3e), the present
  integration, and the performance decompositions.

Zero fold. The share substrate -> sub-kernel-burrow + sub-kernel-weft; the design
+ I-45 staging (guest enforced, host trusted, v3d the fork where it becomes ours)
-> GPU-DESIGN.md. I-45 has no spec module (proven by warp-prove on real V3D). The
staged rungs each have their own AUDIT-TRIGGERS.md row and land on tapestryd/mesa
surfaces sub-tapestryd already carries.

93 -> 94 absorbed of 157. Both big audit-trigger giants remaining is now down to
145-vivarium (3792L). lint 0-fail.
