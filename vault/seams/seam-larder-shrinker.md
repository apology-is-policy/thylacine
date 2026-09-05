---
id: seam-larder-shrinker
type: seam
title: "The 128 MiB/client page-cache ceiling has no memory-pressure reclaim"
status: open
surface: [sub-kernel-larder]
opened-by: fnd-29-r1-f1
tracker: ""
created: 2026-07-31
updated: 2026-07-31
---
**Owed**: a memory-pressure-adaptive cap (a shrinker) for the Larder page
cache — or a smaller constant — before any constrained-RAM bring-up.
`LARDER_PAGE_ENTRIES` = 32768 (a 128 MiB lazy ceiling per cacheable
client) was sized to hold the go-build read working set (the ~27k-page
knee: a Go build scans archives sequentially, LRU-hostile, so the cache
helps only once it holds the WHOLE set). Buffers stay resident until
`larder_destroy` (mount teardown) — Thylacine has no reclaim framework
yet.

**What closes it**: a shrinker hook (reclaim page buffers under memory
pressure, cap-down under a low-RAM policy), designed with whatever
reclaim framework v1.x grows; or a boot-time cap keyed to RAM size.

**Risk while open**: on the 2 GiB dev target, ~2–3 cacheable clients ×
128 MiB ≈ 13–19% of RAM at peak — acceptable. On a small-RAM target
(RPi4 / Lazarus) the ceiling × client-count makes the absent shrinker
LOAD-BEARING — the task-#29 audit's stewardship flag: weigh this seam
before a constrained-RAM bring-up. Degradation is safe (alloc failures
serve as pure misses; competing allocs hit graceful per-Proc OOM, never
extinction) — the risk is performance collapse and memory squeeze, not
unsoundness. A real project larger than gofmt re-thrashes at ANY fixed
cap — the adaptive design is the durable fix.
