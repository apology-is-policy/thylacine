---
id: seam-larder-lazy-array-robustness
type: seam
title: "Lazy first-install: order-9 contiguous allocs + the build-under-lock spike"
status: open
surface: [sub-kernel-larder]
opened-by: fnd-25-r1-f1
tracker: ""
created: 2026-07-31
updated: 2026-07-31
---
**Owed**: (a) a chunked / non-contiguous entry pool — the lazily-
allocated entry arrays are single contiguous kmallocs (the 32768-slot
page array ≈ 1.7 MiB → an order-9 buddy block; attr/dentry ≈ 0.4–0.5 MiB
each), which can fail under buddy fragmentation; (b) double-checked-
locking for the first install — today the array is allocated + zeroed +
hash-initialized UNDER the leaf lock (a bounded preempt-off spike of
~tens of µs, once per cacheable client, ~2–3 clients at v1.0).

**What closes it**: (a) a chunked pool or a reserved boot-time
allocation; (b) build-outside-the-lock, publish-under-it.

**Risk while open**: correctness-safe by construction — an alloc failure
skips the install and the client silently serves as a PURE MISS (I-38
holds; the cache is a best-effort accelerator), then RE-ATTEMPTS on the
next install, so it self-heals when fragmentation clears. The risk is a
silently-disabled cache on a long-fragmented system (a perf cliff that
looks like a regression), plus the once-per-client latency spike. Both
dispositioned P3 at the task-#25 round; promote if a large-cap
contiguous alloc is ever observed failing in practice.
