---
id: chg-2026-07-09-larder-l1d
type: chg
title: "Larder L1d: the dentry sub-cache (incl. negative) — own-write invalidation, not a cvers gate"
date: 2026-07-09
arc: arc-go-build
commits: ["1bb7bf77", "08a3c0cc"]
touched: [sub-kernel-larder, sub-kernel-ninep-dev9p]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
The dentry sub-cache: `(parent-qid.path, name) → child | ENOENT`
(negative entries first-class — the failed-lookup-storm win),
`larder_walk_serve` chaining a whole resolver run under one lock hold
with per-hop attrs from the attr sub-cache, hooked at dev9p walk_attrs
(serve before the RPC; populate after) + create/rename/unlink. The
scripture-correction commit PRECEDED the impl: the sketched parent-cvers
gate was UNSOUND on ground truth — Stratum's parent `si_cvers` does not
bump on a child create/unlink (only rename stamps it; verified in the
Stratum tree), so a cvers compare would falsely match a stale NEGATIVE
after a create. Coherence is own-write invalidation alone (the
fs_cache.tla Read+OwnWrite single-writer subset — no Open gate for
dentries). Populate only from walk_attrs/getattr replies, never a
readdir qid (the L1a-2 Stratum-round rule: Rreaddir's version is a
link-time si_gen snapshot).
