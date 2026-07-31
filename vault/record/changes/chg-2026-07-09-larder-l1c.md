---
id: chg-2026-07-09-larder-l1c
type: chg
title: "Larder L1a–L1c: scripture + fs_cache.tla + the substrate/attr sub-cache"
date: 2026-07-09
arc: arc-go-build
commits: ["d95721ac", "fb736a3a", "4f8f2fa5"]
touched: [sub-kernel-larder, sub-kernel-ninep-dev9p]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
The scripture-first triple that opened the L1 arc (invariant I-38;
spec-first RE-ENABLED for the coherence race): the LARDER design doc,
`specs/fs_cache.tla` (L1b — Open/Read/OwnWrite/ExternalWrite/Evict on
content tokens, clean + external + liveness + 2 buggy cfgs), and the L1c
substrate — `struct larder` on `p9_client` (one leaf lock, 256-entry LRU
attr array, qid.path-keyed with an explicit valid bit for root's qid 0)
+ the dev9p serve/populate/invalidate hooks. Two impl subtleties the
build surfaced: the populate GEN guard (the atomic-Open realization —
capture pre-RPC, re-check at install, skip a raced fill) and
create-invalidates-the-CHILD (Stratum reuses freed inos; the create path
never runs walk_attrs, so an explicit child drop is required — caught
in-build by the stalk-2 delete+recreate E2E). The Stratum-side L1a
foundation (si_cvers carved + surfaced as qid.version, decoupled from
si_gen; no on-disk break) landed on the Stratum branch. Prerequisite
context for every later Larder chunk.
