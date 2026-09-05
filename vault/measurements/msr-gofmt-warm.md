---
id: msr-gofmt-warm
type: msr
title: "Warm gofmt build wall (the go-build FS-perf yardstick)"
metric: "warm cmd/gofmt build wall-clock, device"
unit: "ms"
created: 2026-07-31
updated: 2026-07-31
---
## Method

The on-device `go build cmd/gofmt` (91 packages) with a fully warm
GOCACHE — the go-build mission's standing warm-floor yardstick. TWO
harness eras (values are NOT directly comparable across the switch):
through L1f, the GOFMT374 harness (build ×2 in one boot, the second
build's wall); from B1 onward, the CHASE S1 scenario (median of N=3
boots, clean sentinels, smp=4). Both instrumented builds — the stripped
tree runs faster. Context floor: a trivial warm hello build is ~987 ms
in the L1f era — the L1f re-measure ground-truthed the warm build as
~86% FIXED go-tool overhead (exec/page-in/build-graph/link), so
FS-side levers below that era's ~1.1 s are chasing the ~14% band; the
B1/D44/G-era S1 values sit below the old floor because the fid-lifecycle
+ loose-mode arcs attacked the fixed overhead too (cached-open, aligned
reads, write-behind). Host reference: ~110 ms warm (the "12× damning
ratio" that opened the Larder arc).

## Series

| date | value | chg |
|---|---|---|
| 2026-07-09 | 1352 (pre-Larder baseline, GOFMT374) | [[chg-2026-07-09-larder-l1c]] |
| 2026-07-09 | 1147 (GOFMT374) | [[chg-2026-07-09-larder-l1f]] |
| 2026-07-11 | 367 (S1 med, N=3) | [[chg-2026-07-11-b1-loose]] |
| 2026-07-11 | 249 (S1 med, N=3 — the ≤266 S1 bar crossed) | [[chg-2026-07-11-d44-read-band]] |
| 2026-07-13 | 195 (S1, instrumented A/B window) | [[chg-2026-07-13-g1-write-populate]] |
