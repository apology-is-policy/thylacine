---
id: fnd-29-r1-f1
type: fnd
title: "The 128 MiB/client page ceiling has no reclaim — load-bearing on small-RAM targets"
round: adt-29-r1
severity: P3
status: deferred
surface: [sub-kernel-larder]
threatens: []
seam: seam-larder-shrinker
created: 2026-07-31
---
## Prosecution

The 32768-slot cap is a 64× lift of the prior 2 MiB inline footprint,
with buffers resident until `larder_destroy` (mount teardown) — no
reclaim framework exists. On the 2 GiB dev target ~2–3 cacheable clients
peak at ~13–19% of RAM; on RPi4/Lazarus-class targets the ceiling ×
client-count squeezes real workloads.

## Disposition

Deferred to the seam with the STEWARDSHIP flag added to the constant's
doc: weigh the absent shrinker before any constrained-RAM bring-up.
Degradation is safe (alloc failures serve as pure misses; competing
allocs hit graceful per-Proc OOM, never extinction) — the exposure is
performance and memory pressure, not unsoundness.
