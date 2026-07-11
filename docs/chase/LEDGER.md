# CHASE floor ledger (docs/CHASE.md section 6)

The gap decomposed into named, measured, non-overlapping bands that SUM to
the observed delta per (workload, state). Every band carries: mechanism,
evidence (boot log + instrument lines), disposition (FIXED /
FIXABLE-VOTED / FOUNDATIONAL). An unattributed band means C-1 is not done.

Status: **OPENING POSITIONS SET (C-0 complete, 2026-07-11)** — bands below
are CANDIDATES with first qualitative evidence from the C-0 instrument
windows; magnitudes land at C-1.

## The bar table

| pair | device (smp=4) | host | delta (medians) | bar (host+200ms) | status |
|---|---|---|---|---|---|
| W1 S1 all-warm | 675/749/819 ms (med 749) | 64/66/143 ms (med 66) | +683 ms | <= 266 ms (-64%) | OPEN |
| W1 S3 fully-cold | 5267/5283/5594 ms (med 5283) | 2246/2448/2473 ms (med 2448) | +2835 ms | <= 2648 ms (-50%) | OPEN |
| W2 cold (diag) | — | — | — | not bar-bound | C-2 |
| W2 warm (diag) | — | — | — | not bar-bound | C-2 |

Protocol per docs/CHASE.md section 3 (C-0 as-built block): device = joey
go4c `gofmt-warm` / `gofmt-s3cold` lines at smp=4 (the measured best; the
smp=8 rule was overturned by a clean A/B — see the charter), pool-restored,
clean sentinels, N=3; host = `tools/chase-bench.sh host 3` same-session
(host rep-1's S1=143 ms is its own first-touch warmup; steady-state 64/66).
Evidence logs: build/chase/20260711T10{2128,2200,2334,2458}Z/.

## C-0 qualitative findings (pre-decomposition)

1. **S1 is ~100% RPC-bound at smp=4**: DIAG23 gofmtwarm rpc=4734,
   rpc_ms=683 vs wall 675 ms — count x turnaround IS the window. Wire
   mix: wga=2356 (the POUNCE per-stalk base X-check residual + first-
   touch stats), read=1579, open=527; meanwhile the Larder served
   la=4166 / ld=16929 / lp=1057 hits without wire. fault=0 (REVENANT
   Image cache fully serves warm page-ins — H3 is NOT an S1 band).
2. **The smp=8 inversion is queueing, not work**: identical op counts,
   but depth ge5 explodes (196 -> 1567 warm; 2128 -> 15816 S3) and
   per-op svc inflates 50-130%. 8 vCPUs (4 on E-cores) into a 4-worker
   server. H5 sharpened; parked as a ledger band (the bar runs at the
   device's best config, so this band is diagnostic until 8-vCPU
   scaling is itself chased).
3. **S3 shape**: rpc=49890 (write=20005, read=12977, wga=9818),
   rpc_ms=7331 aggregate at smp=4 wall 5267 ms; pe=8515 (the Larder
   page cache thrashes under the cold build's churn — the #25 sizing
   was fitted to the WARM working set). Compile CPU under HVF is
   plausibly near host-parity (host S3 = 2448 ms is mostly compile) —
   W2 at C-2 separates it.

## S1 candidate bands (warm gap; device ~669-711ms @smp4 vs host 40-50ms)

Priors from the register: the L1f-era 987ms trivial-hello floor was ~86%
go-tool overhead, not FS redundancy; the fid-lifecycle arc (async-clunk +
cached-open) took 67% of that floor down; version-warm ~17ms is the
measured single-invocation floor with every tool Image-cached.

- **B-S1-1 go-driver invocation floor** (H3): exec + REVENANT page-in +
  runtime init + env/GOROOT discovery for the ONE `go` invocation a warm
  build needs. Prior: version-warm ~= 17 ms (quiet host, smp=4) vs host
  `go version` ~= 30 ms — a single warm invocation is already AT parity,
  so H3 is NOT the S1 driver; the warm gap lives INSIDE the one driver
  invocation (B-S1-2..6). H3's per-invocation x N shape belongs to S3,
  where N is ~dozens. (The REVENANT Image cache is doing its job on the
  warm path; the C-1 question moves to the driver's in-window RPC count
  x turnaround.)
- **B-S1-2 build-graph load + per-package freshness sweep** (H1/H6): the
  91-package dep graph's stat/readdir/importcfg sweep. Larder-served in
  the hit case; the residual is (RPC count x per-RPC turnaround). DIAG23:
  walk/wga/gattr/open/clunk counts + la/ld/lp hits for the gofmtwarm
  window.
- **B-S1-3 GOCACHE interaction**: action-id probes + content reads. Same
  evidence channel as B-S1-2 (read/clunk counts + lp hits).
- **B-S1-4 output materialization**: whether the fork's cmd/go re-links
  or cache-copies the ~4 MB gofmt binary; either way the device pays a
  multi-MB write through 9P + AEAD + commit that the host pays through
  APFS + unified page cache. DIAG23svc write avg x count + STMD26 service
  ticks attribute it. NOTE: a host S1 of 40-50 ms is too fast for a real
  link — check for a link-vs-copy asymmetry between the sides before
  reading the band.
- **B-S1-5 child-process floor x N** (H3): compile/asm/link/buildid
  invocations that still run in the warm window (expected ~0 actions on
  full cache hit — verify with -v/DIAG23 rather than assume).
- **B-S1-6 sched/wake residuals** (H4): futex/torpor wake chains + HVF
  vCPU wake latency inside the window. Evidence: the residual after
  B-S1-1..5 sum against wall-clock; TI-4e priors (deep-park resume) say
  this is real under HVF.

## S3 candidate bands (fully-cold; host 2.43s; device number lands at C-0)

- **B-S3-1 compile CPU** (91 packages, arm64 codegen under HVF): the
  compute band. W2-cold at C-2 separates HVF-CPU from machinery.
- **B-S3-2 GOCACHE writes**: every action's output written to /gocold
  through 9P + AEAD + Merkle + commit cadence (H1/H2/H6). STMD26 write
  ticks + d26 re/dec counters.
- **B-S3-3 tool-invocation floor x N** (H3): ~dozens of
  compile/asm/link spawns; each pays spawn + ELF load + page-in.
  version-warm (~17 ms) x invocation count bounds it from below.
- **B-S3-4 source reads**: stdlib + gofmt sources through the Larder
  (warm-ish from S1's window; the S3 step runs after S1 in the same
  boot).
- **B-S3-5 sched/parallelism** (H4/H5): -p 8 fan-out efficiency on 8
  vCPUs vs the host's 8 cores.

## FOUNDATIONAL candidates (empty until measured)

(none yet — a band lands here only with decision + mechanism + measured
magnitude + the removing redesign, and survives a Fable prosecution.)
