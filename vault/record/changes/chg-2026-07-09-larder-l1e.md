---
id: chg-2026-07-09-larder-l1e
type: chg
title: "Larder L1e: the page sub-cache + the load-bearing cacheability gate"
date: 2026-07-09
arc: arc-go-build
commits: ["c7cd73ad"]
touched: [sub-kernel-larder, sub-kernel-ninep-dev9p]
established: []
closed: []
opened: [seam-larder-cacheable-proxy]
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
The page sub-cache: `(qid.path, page_index) → {bytes[0..valid_len),
cvers}` (born a 512-slot linear table; heap per-slot 4 KiB buffers,
lazily kmalloc'd, reused across evictions, freed at `larder_destroy`),
hooked at dev9p read (serve the one page containing the offset —
cvers-gated against the reading fid's open-time qid.vers, the
close-to-open Open gate; populate every covered page from its ALIGNED
start so there is never a hole; a partial page serves within valid_len
and misses beyond — no EOF determination from pages) and write
(own-write whole-file invalidate). PLUS the arc's load-bearing gate: the
per-client `cacheable` flag latched by a successful Twalkgetattr — the
whole Larder engages only for a content-versioned FS, so netd's
consuming stream reads are never cached (also closing the latent L1c
netd-attr gap: attr caching previously had no server gate). No spec
extension (a page is a content token like an attr).
