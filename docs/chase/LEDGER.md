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
| W1 S1 all-warm **post-B1** | 347/401/367 ms (med **367**) | (same) | +301 ms | <= 266 ms (**-28% remains; ~101 ms**) | OPEN |
| W1 S3 fully-cold | 5267/5283/5594 ms (med 5283) | 2246/2448/2473 ms (med 2448) | +2835 ms | <= 2648 ms (-50%) | OPEN |
| W1 S3 fully-cold **post-C-3** | 5093/5052/5085 ms (med **5085**) | (same) | +2637 ms | <= 2648 ms (**-48% remains**) | OPEN |
| W1 S3 fully-cold **post-B1** | 4877/5252/5222 ms (med **5222**) | (same) | +2774 ms | <= 2648 ms (S3 needs C-2) | OPEN |
| W1 S3 fully-cold **post-D44+#45** | 4059/3973/4066 ms (med **4059**) | (same) | +1611 ms | <= 2648 ms (**-35% remains**; the gap now lives in the write band -- rpc_ms 3.3-3.5 s of the 4.0 s window, write=~20k ops) | OPEN |
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
3. **S3 shape** (pre-D44: rpc=49890, write=20005, read=12977, wga=9818; post-D44+#45: rpc=41219, write=20533, read=6331, wga=7280, rpc_ms=3395 of the 4059 ms window -- the write band is now the S3 gap),
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
| wga | 292 | 2356 | 124 us | cached-open B2 FORCED-WIRE revalidation (~1604 minted opens) + bind-form walks for wire opens (~535+) | **FIXED @ thyla bea3afc9+f56fce66, audit CLOSED @da391435 (Fable 5: 0 P0/0 P1/1 P2/2 P3, NOT dirty -- the P2 = a PRE-EXISTING keeper-commit snapshot-atomicity hole [the third-actor stale-fid repopulate], closed by the pages-snapshot gen witness on BOTH strict+loose paths; memory/audit_b1_loose_closed_list.md): B1 per-attach loose mode, Senate-voted option B** -- SYS_ATTACH_9P_LOOSE on the system mount; a full-hint-hit cached-open skips the wire revalidation (snapshot at the cached cvers; I-38 default stays strict; premise + re-strict triggers in the ARCH I-38 row). **Measured (N=3, clean sentinels): warm wga 2356 -> 751 ops / 201 -> 51 ms; total rpc 4751 -> 3132, rpc_ms -> 255; wall med 439 -> 367 ms (arc cumulative 749 -> 367, -51%).** S3 marginal (cold wga is first-touch -- expected). NEW: wall 367 vs rpc 255 = ~110 ms non-RPC residual now visible (go-tool CPU + cache serves + sched) -- decompose after the read band |
| read | 148 | 1588 | 93 us | **MEASURED (D44 miss-class + per-qid top-8 + host ino->path map): the band was the GO TOOLCHAIN'S OWN BINARIES, re-read in full per exec.** (1) REVENANT demand-pages only TEXT; every other PT_LOAD (a Go binary's multi-MB rodata + data) is EAGERLY dev->read at exec (`map_eager_from_file`) -- /goroot/bin/go = 6.1 MB/exec (even `go version`), compile = 8.7 MB/exec (x91 execs in S3 = 792 MB!). fault=0 proved these are DATA reads, not page-ins. (2) The Larder could not serve the repeats: the msize payload (131049) is not a page multiple, so every sequential chunk ends in a PARTIAL page; populate cannot fill a partial-FRONT page, so the hole persisted and every re-exec pv-missed at page 31 and re-wired the whole tail (pv=1288/1568 = 82% of warm misses; stale=0; loose=ALL -- GOROOT+GOCACHE both ride the system mount) | **FIXED (thyla, uncommitted pending audit): (a) aligned wire reads** -- a big (>4 KiB) unaligned read on a cacheable client wires at the containing page's ALIGNED start (a legal short read; each chunk fully rewrites its predecessor's partial tail -> holes heal; <=3% duplicate bytes; zero-alloc, the shift is an in-buffer forward copy) + **(b) attr-served EOF** -- a FRESH cached attr (cvers == fid qid.vers; plain files only) answers the final read-returns-0 probe RPC-free. Regressions: `dev9p.read_align_heals_partial` (fails pre-fix) + `dev9p.read_eof_attr_served`; the loopback fixture grew a big-file pattern mode + msize-sized buffers (were under-msize 4096). **Boot 1 effect: warm 358->261 ms (read band 157->64 ms; wire reads 1568->935; KB 65884->20280; szbig 166->0; lp 1037->12632); S3 5486->3867 (-1.6 s); gofmt-cold 1540->1123; version-warm 18->7 ms.** N=3 DONE (warm med 249: 249/288/247; clean sentinels) -- **the S1 bar (<=266) is CROSSED**; committed @cd4c1e9b + audit close @(next commit): **Fable 5 focused audit 0 P0 / 1 P1 / 0 P2 / 2 P3** -- F1 [P1] the `got <= lead -> return 0` arm manufactured a FALSE MID-FILE EOF on a legal short Rread (SA-F1 class; REVENANT cluster -> zero-filled resident text pages) -> FIXED (true-EOF/short split + unshifted retry; regression `dev9p.read_align_short_not_eof`); F3 [P3, pre-existing] OTRUNC open invalidated nothing -> FIXED (own-write-through); F2 [P3] attr-EOF close-to-open extension -> documented within the I-38 premise. REMAINING seam CLOSED by #45 (scripture @23cca1fc, impl next commit): the PT_LOAD dispatch generalizes PF_X -> NOT-PF_W, so R-only rodata rides the REVENANT file-backed Image-cached path (R-only/XN PTEs; IMAGE_CACHE_MAX 64->128; the fault arm + Image needed ZERO changes -- already prot-general/per-segment). **Measured honestly (N=3, clean sentinels): warm med 249 UNCHANGED (247/249/255 -- the bar stays crossed); S3 med 4059 vs 4088 = FLAT (-29 ms).** The 792-MB monster had ALREADY been eaten by D44 (re-reads became guest-local Larder serves); #45 converts those serves into resident-hit fault installs (~a wash at wall-clock scale) -- its standing wins are structural: first-exec laziness (only touched pages load), ONE shared resident rodata across Procs instead of 91 private ~8-MB copies (RAM under parallel builds), and the Plan 9 Image model completed. Ground truth the mechanism engaged: S3 wire KB=101299 (~99 MB total -- the 810 MB of eager re-reads is structurally impossible in it); versionwarm wire = 14 KB (was 6.1 MB eager pre-D44). The S3 gap moves to the WRITE band: gofmts3 rpc=41219/rpc_ms=3395 with write=20533, wga=7280, read=6331 (writes unchanged since the pre-D44 shape; reads halved) -> C-2 decomposes it. **Audit CLOSED (Fable-5-max prosecutor + concurrent self-audit CONVERGED on F1; 0 P0 / 1 P1 / 0 P2 / 2 P3, NOT dirty): F1 [P1] the Image key omitted prot -> a crafted ELF aliases one FILE Burrow across R+X/R-only -> a rodata-first fill (no I-cache sync) then a text resident-hit executes stale I-cache lines (#317 hazard, self-inflicted) -> FIXED by adding an `exec` bit to the key (image.exec_discriminates_key + exec.from_spoor_aliased_window_distinct regressions); F2 [P3] the gate now requires PF_R (a no-access flags==0 PT_LOAD stays eager); F3 [P3] the rodata-prot test probes a non-zero pattern byte. memory/audit_45_rodata_closed_list.md** |
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
