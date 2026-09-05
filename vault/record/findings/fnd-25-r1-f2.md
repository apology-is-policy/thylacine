---
id: fnd-25-r1-f2
type: fnd
title: "The first install builds + zeroes the entry array UNDER the leaf lock (a preempt-off spike)"
round: adt-25-r1
severity: P3
status: deferred
surface: [sub-kernel-larder]
threatens: []
seam: seam-larder-lazy-array-robustness
created: 2026-07-31
---
## Prosecution

The one-time first-install allocates, zeroes, and hash-initializes the
entry array under `l->lock` with preemption off — a bounded spike (~tens
of µs for the ~1.7 MiB page array), once per cacheable client (~2–3 at
v1.0).

## Disposition

Deferred to the seam: the v1.x cleanup is double-checked locking (build
outside the lock, publish under it). Correctness-safe as-is — the spike
is bounded and once-per-client; no consumer holds a latency budget
across it.
