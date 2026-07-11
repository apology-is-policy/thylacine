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
| W1 S1 all-warm **post-C-3** | 448/439/396 ms (med **439**) | (same) | +373 ms | <= 266 ms (**-39% remains**) | OPEN |
| W1 S3 fully-cold | 5267/5283/5594 ms (med 5283) | 2246/2448/2473 ms (med 2448) | +2835 ms | <= 2648 ms (-50%) | OPEN |
| W1 S3 fully-cold **post-C-3** | 5093/5052/5085 ms (med **5085**) | (same) | +2637 ms | <= 2648 ms (**-48% remains**) | OPEN |
| W2 cold (diag) | — | — | — | not bar-bound | C-2 |
| W2 warm (diag) | — | — | — | not bar-bound | C-2 |

Post-C-3 rows: the readdir batch fix (stratum 750fab1) landed 2026-07-11;
N=3 pool-restored smp=4 boots, clean sentinels both ends. The warm mix is
now wga ~200 > read ~160 > open ~40 > rdir ~22 (the wga B1-loose-mode vote
+ the read page-cache band are each singly ~sufficient to reach the S1 bar).

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

## S1 MEASURED bands (C-1 boot 2, 2026-07-11; wall 668 ms, rpc_ms 859;
## bands SUM to 856 -- N=1, re-price at N=3 before any FOUNDATIONAL claim)

The warm window is ~100% RPC aggregate (4751 ops). Priced per type
(DIAG23ns; per-op = total/count):

| band | ms | ops | per-op | mechanism | fix candidate |
|---|---|---|---|---|---|
| readdir | 378 | 206 | 1835 us | **CONFIRMED server-side (C-3 instrument, 2026-07-11): 99.75% of h_readdir time is inside stm_fs_readdir** (whole-boot d26: rd=1633 ops/1353.5 ms, fs=13408 calls/1350.2 ms = 100.7 us/call, 8.2 calls/op). h_readdir pulls ONE entry per fs call (R92 P1-2) and each call pays the full stack: EBR pin + parent-inode btree lookup + engine get + WHOLE-DIR scan + qsort + malloc (the dirent key is LE-encoded -> byte order != probe order -> interior cursor bracket inexpressible -> scan-all-and-filter per call; dirent.c:1125 forward-note). The listed dirs are the per-package GOROOT/src ReadDir sweep (~103 dirs x 2 ops [data+EOF]; cmd/go memoizes ReadDir in-process so each dir lists ONCE per invocation -- NOT GOCACHE, which is 256-way fanout and warm-lookup-by-computed-path; the earlier "one flat ~2400-entry dir" note was WRONG) | **FIXED @ stratum 750fab1 (2026-07-11): h_readdir batches 32/call via a per-entry resume cursor** (stm_fs_dirent_entry.next_cursor, wire-identical cookies; UNSTABLE-tier ABI, no wire/disk change) + a collection-time cursor filter in the dirent scan. **Measured: readdir server time 1353.5 -> 39.6 ms/boot (-97%); fs calls 13408 -> 1826 (7.3x); identical wire op counts. Warm band 317 -> 22 ms; build2 274 -> 10; cold 297 -> 25; S3 339 -> 45. Wall: gofmt-warm 673 -> 448 ms, build2-warm 578 -> 317, S3 5505 -> 5093 (N=1; N=3 below).** ctest 73/73 stripped-tree; R92 truncation regression passes; new fs_p4_readdir_next_cursor_resume (4-property contract); dirent spec gate clean + buggy cfgs RED (pre-existing dirent_whiteout.cfg healthy-cfg failure surfaced + tracked). Guest-side Larder dirent cache DEAD for S1 (no repeat listings within one invocation). BE re-key = the deferred on-disk fix (dirent.c forward-note), not needed at this op mix |
| wga | 292 | 2356 | 124 us | cached-open B2 FORCED-WIRE revalidation (~1604 minted opens) + bind-form walks for wire opens (~535+) | the deferred B1 loose mode (serve the snapshot with NO wire revalidation) -- sound under single-writer-guest + own-write invalidation, but it weakens the I-38 close-to-open DEFAULT -> scripture + USER VOTE (FIXABLE-VOTED) |
| read | 148 | 1588 | 93 us | cached-open snapshot fills + reads that miss the Larder page cache (lp=1047 served vs 1588 wire). NOTE (corrected post-C-3): the cache is ALREADY 32768 slots / 128 MiB ceiling since #25 -- the earlier "512-slot" note was STALE. The misses are NOT a tiny-cache artifact; candidates: >DEV9P_CO_MAX_SIZE(128K) files (the ~4 MB gofmt binary copy-back), first-touch-in-window files, eviction churn (pe=8515 at C-0 = cold-build churn past 128 MiB). Needs a miss-class instrument BEFORE any sizing claim | instrument the wire-read distribution (size-class / qid / cached-open-fallback flag) first; then either the per-open snapshot budget/cap lift or a shared (qid,cvers) snapshot cache |
| open | 31 | 535 | 58 us | wire Tlopen (fid binds) | fidless-open coverage extension (funnel: hchain=126 hpart=77 htype=103 hsize=94 hcover=197 fails) |
| rest | ~7 | 66 | -- | walk/gattr/clunk/write | noise |

Reading: the four named bands carry ~849 of 856 ms. The bar needs
-64% (668 -> <=266): the readdir band alone (-370 if both levers land)
plus ANY second band gets within reach -- the chase is alive, and no
band has yet earned FOUNDATIONAL. Per-RPC turnaround (~90 us floor,
~1.8 ms readdir outlier) matters less than COUNT: at 10x op reduction
the residual boundary cost is ~40 ms.

## S1 candidate bands (pre-measurement shapes -- kept for the mechanism
## notes; superseded by the table above)

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
