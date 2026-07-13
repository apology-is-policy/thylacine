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
| W1 S3 fully-cold **post-C-2-F2** (stratum 85d3b72: transition-into-buffer + the 3-round audit close) | 3551/3844/3614 ms (med **3614**; warm 246/244/255 -- S1 stays crossed) | (same) | +1166 ms | <= 2648 ms (**-27% remains**; the W-C transition tax is dead [enc 636->73, hw 43.9->18.4 us/op]; the residual = the op-count bands F1 [voted, scripture @f9c3cafe] removes + the wga cold band) | OPEN |
| W1 S3 fully-cold **post-F1** (94409d56 write-behind + 4c823d30 single-flight + e40f487f audit close; the FINAL-tree N=3, clean sentinels 193134Z [the pre-fix set 190526Z read 3297/3454/3245 med 3297 -- the fixes are hot-path-neutral, the delta is run variance]) | 3354/3142/3166 ms (med **3166**; warm 248/244/248 -- S1 stays crossed) | (same) | +518 ms | <= 2648 ms (**-16% remains**. The op-count lever DELIVERED mechanically: wire writes 19593 -> 2155 [-89%], write band 892 -> 283 ms, rpc_ms 1991 -> 1399 [device-1 pairs of the 164124Z/190526Z instrumented boots] -- roughly -450 of the -592 ms RPC cut reached the wall median. Post-F1 S3 anatomy: rpc ~1399 [wga 414 = the #1 band, read 367, write 283, misc ~335] + non-RPC ~1.8 s [~compile CPU + exec/page-in + the go-tool floor]. Remaining levers: the wga cold band [#372 + the base-Spoor memo seam, ~-150-250?], F1b anchor-from-cached-attr for opened-existing files [the 1312 residual sz4k through-writes, ~-100-150?], read tail [~-50-100?] -- optimistic sum ~-300-500 vs the +518 needed: the bar is at the edge of reach; see the Term-2 FINAL close below: post-F1 the guest FS-cache levers are EXHAUSTED [wga/F1b/read-sizing/prefetch all measured-dead] and #50 [the ~10%25 EIO] was root-caused to the #375 out_buf clobber + FIXED; the honest S3 disposition is GUEST-EXHAUSTED/SERVER-FIXABLE [Stratum #367], NOT foundational) | GUEST-EXHAUSTED |
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

## Term-2 MEASUREMENT (2026-07-12; fresh goroot pool, instrumented, smp=4): the post-F1 lever plan was built on a STALE decomposition

The post-F1 row above priced "misc ~335 = getattr" and voted the base-Spoor
memo (Senate (c)). A fresh instrumented S3 boot (DIAG23ns + a new WGAd
first-touch/attribution probe) CORRECTS it:

- **The getattr lever is DEAD.** `DIAG23ns gofmts3 gattr=9-12ms`; the Larder
  attr cache (L1c) already serves **98%** of base X-checks cold (la=14488 vs
  wire gattr ~290). The "1:1 Tgetattr:Twalkgetattr residual" + "misc 335 =
  getattr" were PRE-L1c readings carried forward. The base-Spoor memo would
  duplicate what L1c already does -> NO lever there.

- **True S3 RPC decomposition** (rpc_ms ~1256 of the ~3072-3166 wall; the rest
  ~1816 is non-RPC = compile CPU + exec/page-in + go-tool floor): **wga 329 /
  read 329 / write 252 / open 57 / walk 74 / rdir 44 / gattr 9 / clunk 10**.

- **wga is 83% RE-WALK, not first-touch** -- `WGAd wire=7188 distinct=1225`;
  attribution **dinval=2228 / devict=0 / dskip=878** = 100% invalidation-thrash,
  0% eviction. The whole-parent dentry invalidation (700 creat + 615 unl + 349
  mkdir = 1664 dir mutations, each dropping the parent's WHOLE dentry set when
  only the mutated name is stale) drives it. FIX BUILT
  (`larder_dentry_invalidate_name` -- name-specific drop, matches
  `fs_cache.tla OwnWrite(f)` per-token, 1103/1103, NoWrongRead intact) but
  **MODEST**: wire 7188->6902 (-4%), band 351->329 (~-22ms, within single-boot
  noise). The re-walks are dominated by mutated-file re-walks + the global
  gen-bump skip (dskip 707), NOT siblings -- so preserving siblings reclaims
  little. Only ~15% of the wga band (~50ms) is cache-survival-reclaimable; the
  ~85% residual is the build's inherent create->walk->create->walk churn (each
  walk legitimately follows a mutation that invalidated its binding) -- an H1/H6
  floor (metadata mutation rate x per-op 9P RTT).

- **read (329ms, pe=10344 evictions) is NOT a clean sizing win.** Already a
  128 MiB page cache (32768 slots); dominant miss is `pv=3834` (partial-page
  past-valid), not eviction -- growing the cache does not fix partial-page.

- **F1b MEASURED DEAD (the projection was WRONG).** The "~-50-118ms projected"
  above assumed the 1312 sz4k residual writes were opened-existing appends. A
  direct append-shape instrument (`WBd`, classifying every wire Twrite at
  `dev9p_write` by whether it is a sequential append to a `!wb_eligible`
  opened-existing file) proves otherwise on the fully-cold S3:
  `WBd gofmts3 staged=20687 wireelig=347 oeapp=26:204kb other=195`.
  - `oeapp=26 ops / 204 KB` = F1b's ENTIRE addressable ceiling (opened-existing
    SEQUENTIAL appends). At the ~148us/op S3 write-band wall rate that is **~4 ms**.
  - `other=195` = opened-existing NON-append = the Go toolchain's buildid
    `WriteAt` (positioned, mid-file) pwrites -- F1b-inert by construction (the
    wb gate already write-throughs a non-append; F1b stages only appends).
  - `staged=20687` = F1 already coalesced 20,687 append writes; the residual is
    inherent (each object written once, buildid-patched once).
  The WBd instrument UNDERCOUNTS oeapp: it flags an append via a per-priv
  running watermark (`dbg_thru_end`, born 0), so the FIRST append to each
  opened-existing file (watermark still 0, offset != 0) is miscounted into
  `other`. So F1b's true ceiling is bounded ABOVE by ALL opened-existing wire
  writes = oeapp + other = 221 ops (~33 ms if EVERY "other" were a first-append
  -- an overcount, since Go genuinely does buildid pwrites) and BELOW by the
  measured clean-sequential 26 ops (~4 ms). F1b's ceiling is **[4, 33] ms of a
  +650 ms gap** -- negligible regardless of the watermark undercount. Building an
  audit-bearing write-behind extension (a new anchor-staleness hazard) for it
  would be overfitting for nothing. NOT built; documented measured-insufficient.

**Honest bar math (corrected)**: the two term-2 FS-cache levers are exhausted --
wga narrowing LANDED (`b317fc28`, ~-22 ms wire trim, its real value the
correctness-of-model + O(1) scan retirement) and F1b MEASURED DEAD (~4 ms
ceiling). Neither approaches the **+424 ms wall gap** (3072/3166 -> <=2648). The
read-band sizing lever is also inert (128 MiB cache; the miss is partial-page,
not eviction). The S3 residual decomposes as (but see the Opus audit correction
at the end -- the "FOUNDATIONAL" claim is NOT yet earned; two of these are
UNMEASURED levers):
(a) the compile-CPU floor (~1816 ms non-RPC, ~host-comparable under HVF -- H3;
    NOT the gap -- the compiler's CPU work is the same on host and device);
(b) the userspace-9P-FS first-touch RPC floor (the ~1256 ms RPC bands are
    dominated, after L1/D44/F1, by the ONCE-per-file traffic a cold build must
    pay -- read every input once, write every output once, walk every path once
    over 9P; the guest cache elides RE-touches, never first touches -- H1/H6).

## Term-2 CLOSE (2026-07-12): the FS-cache-thrash levers are exhausted + measured-insufficient; the FOUNDATIONAL determination is BLOCKED on two unmeasured levers (Opus audit F4/F5 -- see the correction at the end)

Per the Senate's accepted (a)-then-(b) recommendation, the two term-2 fixes were
processed to ground:

| lever | disposition | measured | magnitude |
|---|---|---|---|
| wga sibling re-walk (whole-parent dentry drop) | **FIXED** (`b317fc28`) | wire 7188 -> 6933 | ~-22 ms (correctness-of-model + O(1) scan retire the primary value) |
| F1b opened-existing append staging | **measured-insufficient** (NOT built) | `oeapp=26 ops/204 KB` | ~4 ms ceiling |
| read-band page-cache sizing | **measured-inert** (already done) | 128 MiB, `pv=3834` partial-page | 0 (miss is not eviction) |

**The residual (S3 ~3072/3166 wall, gap ~+424 to <=2648) decomposes as:**
- **H3 (compile CPU, ~1816 ms, NOT the gap):** the Go compiler's actual work,
  ~host-comparable under HVF. Removing it = a different compiler; out of scope.
- **H1/H6 (first-touch 9P RPC, the gap):** ~1256 ms of RPC bands, after L1
  (Larder attr/dentry/page), D44 (aligned reads + attr-EOF), F1 (write-behind
  coalescing 20533 -> 2155 wire writes), and this term's wga narrowing, is
  dominated by the ONCE-per-file cost a cold build cannot cache away (nothing to
  cache on first touch). This is the architectural cost of a **userspace 9P FS**
  (Stratum is a userspace 9P server by VISION/Plan-9-heritage design): every
  metadata op is a round-trip the host's page-cache-backed local FS elides.

**FOUNDATIONAL disposition + the one residual FIXABLE-VOTED candidate:**
- **Decision:** the FS is a userspace 9P server (Stratum), reached over a 9P
  transport, per the "filesystem is the OS" VISION + the Plan-9 heritage.
- **Mechanism:** one round-trip per metadata op; a cold build does O(files)
  first-touch metadata ops that no guest cache can pre-populate.
- **Magnitude:** ~1256 ms of RPC bands on the fully-cold S3 (of which the
  cache-reducible re-touch fraction is already captured by L1/D44/F1).
- **Removing redesign:** either (i) move the FS into the guest kernel (a local
  block FS -- contradicts the userspace-9P-server VISION), or (ii) **bulk-prefetch
  the build's working set** (readahead the GOROOT/GOCACHE the build will open, so
  first-touch RPCs batch). (ii) is the one un-tried lever that could trim the H1/H6
  floor; it is LARGE (predict the data-dependent file set) with UNCERTAIN payoff
  (a build's access pattern is data-dependent; the REVENANT 64-page read-ahead +
  D44 aligned reads already batch the WITHIN-file traffic). It is a **FIXABLE-VOTED**
  residual: the Senate's call whether to build it, weighed against S1 being
  crossed (warm 248-252 <= 266) and S3 being +424 short with the clean levers spent.

**Recommendation (CORRECTED by the Opus audit -- see the end):** S1 is DONE
(crossed). The FS-cache-thrash levers are exhausted + measured-insufficient
(wga narrowing ~0 wall; F1b [4,33] ms; read-sizing inert). But **"S3
FOUNDATIONAL" is NOT yet earned** -- two levers under the H1/H6 + H3 residual are
UNMEASURED: (a) bulk working-set prefetch (~150-400 ms addressable, overlaps the
gap) and (b) the ~1816 ms non-RPC bucket (W2 -- the compile-CPU-vs-spawn-floor
split -- was NEVER run). The honest arc-exit path: **run W2 + measure the
prefetch ceiling** (both cheap; the protocols exist), THEN decide FOUNDATIONAL
vs FIXABLE -- OR **vote to attempt the prefetch lever** -- OR accept the honest
partial (S1-crossed, S3 FS-cache-exhausted, the residual FIXABLE-VOTED-pending).
The Senate's call.

### N=3 result (2026-07-12, instrumented keeper build `b317fc28`-behavior, smp=4, sentinels clean)

- **S1 (warm gofmt): {240, 253, 242} -> median 242 ms.** Bar <=266. **CROSSED.**
- **S3 (cold gofmt): {3370, 3271, 1890}.** The **1890 boot is VOID** -- its
  `go build` FAILED (`compile: writing output: write $WORK/b062/_pkg_.a:
  input/output error`), exiting nonzero after ~half the work (wga=3750 vs the
  valid boots' ~6908; staged=10595 vs ~20600). The 2 VALID boots are 3370/3271
  -> **median ~3320 ms, gap ~+670 to <=2648.**
- **The wga narrowing's S3 WALL effect is ~0 (within boot variance).** It trims
  255 wire re-walks (7188 -> 6933) but the ~3320 median is within noise of the
  pre-narrowing ~3072-3166; the narrowing's value is correctness-of-model +
  the O(1) scan retirement, NOT a measurable S3 win. This SHARPENS the
  FOUNDATIONAL case: the term-2 FS-cache levers moved S3 by ~0 measurable ms
  against a ~+670 gap.

### SURFACED DEFECT (task #50, enqueued): a rare intermittent Stratum/bdev write-EIO

The 1890 boot's failure is a **real cold-build reliability defect**, ground-truthed:
- **~10% frequency** (1 of ~10 S3 cold builds this session; every other boot 0 EIO,
  incl. the stripped-keeper N=3 which was 0/3).
- **NOT the wga narrowing** -- a write EIO to an already-open fid never consults
  the dentry cache; the walk-path change cannot cause it; the stripped keeper build
  hit it 0/3.
- **NOT pool-margin** -- goroot content 125M in a 2.5G pool (~2.3G free).
- **Stratum/bdev-side** -- the STMD26 instrument shows a read-error counter anomaly
  (`rd=...e10290`) on the failing boot; territory = the F1 write-behind flush path
  OR the C-2 F2 Stratum inline->extent transition (both freshly landed in the S3
  write path), OR a QEMU virtio-blk hiccup (NOT yet PROVEN QEMU -- per the no-flake
  discipline, an intermittent I/O error is a RACE to hunt in the guest/Stratum until
  QEMU is proven). Does NOT invalidate the wga narrowing (committed, sound) or this
  FOUNDATIONAL disposition (which uses the successful boots' timing) -- but it is a
  soundness item on the cold-build write path that should be hunted (the go build is
  the stress oracle). Enqueued as task #50.

## S3 candidate bands (fully-cold; host 2.43s; device number lands at C-0)

- **B-S3-1 compile CPU** (91 packages, arm64 codegen under HVF): the
  compute band. W2-cold at C-2 separates HVF-CPU from machinery.
- **B-S3-2 GOCACHE writes**: every action's output written to /gocold
  through 9P + AEAD + Merkle + commit cadence (H1/H2/H6). STMD26 write
  ticks + d26 re/dec counters. **DECOMPOSED at C-2 — see the C-2
  section below.**
- **B-S3-3 tool-invocation floor x N** (H3): ~dozens of
  compile/asm/link spawns; each pays spawn + ELF load + page-in.
  version-warm (~17 ms) x invocation count bounds it from below.
- **B-S3-4 source reads**: stdlib + gofmt sources through the Larder
  (warm-ish from S1's window; the S3 step runs after S1 in the same
  boot).
- **B-S3-5 sched/parallelism** (H4/H5): -p 8 fan-out efficiency on 8
  vCPUs vs the host's 8 cores.

## C-2 write-band decomposition (task #46; MEASURED 2026-07-11, smp=4
## instrumented boot 155304Z: wall S3=4093 ms, rpc=40480/3609 ms — the
## instrument boot reproduces the 4059 baseline; decomposition-grade)

**The S3 write band = 19593 wire Twrites / 1795 ms guest RTT (91.6 us
avg; 52 us depth-1) moving only 85 MB — 97.9% of Twrites are <= 4 KiB
(sz4k=19174, sz32k=376, sz128k=43).** The Go toolchain writes its
outputs through ~4 KiB buffered I/O (cmd/internal/bio = bufio 4096),
and every object lands TWICE ($WORK output + GOCACHE copy: the C2top
pairs — q=100001812 + q=100001820 both 11075 KB at ~2.7k writes each;
q=1000018ef + q=100001913 both 3114 KB; ...). Server-side (STMD26W
deltas over the S3 window, frq=24 MHz):

| sub-band | measured | mechanism |
|---|---|---|
| W-A transport+queueing | **~933 ms** (guest 91.6 − server 43.9 us × 19.6k ops; depth-1 transport ≈ 8 us — the rest is queueing: ge2=18257 = 45% of sends ride behind a peer) | per-op wire cost × op count; same-inode writes serialize on the server's exclusive per-inode pin + the single guest wire |
| W-B server per-op fixed | **~430 ms** of hw=862 ms: ins=218 ms (dirty-buffer insert memcpy+admission, 11.6 us/op), iset=71 ms (per-write mtime+size inode_set, 3.7 us), vfs+ilk=31 ms (the 2 EBR lookups — nearly free), parse/fid/reply ≈ 52 ms | 3 metadata ops + 1 buffer memcpy per 4 KiB dribble |
| W-C INLINE→EXTENT transition | **~400 ms** = 587 transitions × ~600 us: the transition arm SYNCHRONOUSLY writes the combined buffer as an extent — reserve + AEAD + **one ~508 us virtio-blk round-trip mid-RPC** (bw window delta: 635 writes / 323 ms / 47.4 MB ≈ 587 transitions + 47 drains) | every created file > 128 B pays a sync bdev write on its FIRST over-inline write, for bytes NOT yet durable anyway (commit=0 in-window) |
| W-D cap-pressure drains | **~146 ms** (47 drain cbs / 42.8 MB — the two 11 MB files breach the 8 MiB per-inode dirty-buffer cap mid-write; drains emit efficient big extents) | correct behavior; shrinks to noise if ops coalesce |
| AEAD encrypt | **35 ms TOTAL** (enc delta = 636) | armcrypto did its job — encryption is NOT the S3 story |
| commit barrier | **0 in-window** (cm delta=0; the only commit is at mount; bf=0) | no barrier cost; #369/#40 quiet; unlink correctly DISCARDS buffered ranges (drop_ino — no drain-of-dying-bytes) |

Cross-check: hw(862) + W-A(933) = guest 1795 ✓; fw(810) = ins(218) +
iset(71) + ilk(21) + drains(146) + transitions(~354) ✓.

**Verdict: the write band is OP-COUNT dominated (W-A+W-B ≈ 1.36 s scale
linearly with the 19.6k ops) plus the per-file sync-transition tax
(W-C ≈ 0.4 s).** Named levers (C-2 STEP 2):
- **F1 guest write-behind — SENATE VOTED YES (2026-07-11); scripture
  LANDED (LARDER-DESIGN.md §12 + the ARCH/CLAUDE.md Larder-row F1
  addendum)**: per-open-file contiguous append-run staging
  (`dev9p_priv.wb`, 256 KiB cap, I-32-charged, write-through fallback
  on any non-append/over-budget/latched-error); flush at close
  (before the async-clunk)/fsync/cap/non-append; same-priv reads
  overlay; fstat patches size; per-write invalidates become per-flush.
  Deferred errors latch on the priv + surface at fsync (the `Dev.close`
  slot is void at v1.0 — close-flush best-effort, documented). Strict
  mounts byte-identical. Spec-first: fs_cache.tla + StageWrite/
  FlushClose + 2 buggy cfgs BEFORE the impl. 19.6k ops → ~1.4k ⇒
  W-A+W-B ≈ −0.7..0.9 s of the post-F2 936 ms band + queueing ripple.
- **F2 server transition-into-buffer (Stratum) — LANDED; R1 audit
  CLOSED (Fable 5: 0 P0 / 2 P1 / 0 P2 / 4 P3, ALL FIXED; round-2 on
  the fixes in flight; `memory/audit_c2_f2_closed_list.md`)**: the
  INLINE→EXTENT transition INSERTS the block-aligned combined buffer
  into the dirty buffer (same #40 bounded admission; ENOSPC falls
  back to the pre-F2 sync write — fail-safe) instead of the sync
  extent write; the kind-flip iset is unchanged. **R1-F1 [P1]: the
  reclaim-on-ENOSPC double-commit persisted a transitioned inode's
  flip WITHOUT its extent (drain-free commit; crash/wedge → silent
  zeros incl. previously-committed inline content) → FIXED
  drain-first (raw drain, no reclaim re-entry; deep corner: pair →
  re-drain → closing commit); regression revert-probed (pre-fix
  reads zeros). R1-F2 [P1, PRE-EXISTING, widened]: the buffered
  read's 3 critical sections race a pin-free cross-inode flush_all →
  transient silent-zero reads → FIXED via the monotone resident-byte
  retry (termination-proof: same-ino inserts pin-excluded, the count
  only decreases). +F5 threshold gate (≥1 MiB transitions go
  direct), F6 ghost-range drop, F4 punch-refusal regression, F3
  comment.** The read path needed ZERO changes (the EXTENT arm
  zero-fills holes [never ENOENT] + overlays the buffer —
  writeback.tla::ReadHidesFlushOrder); every extent-structure mutator
  pre-flushes (fallocate/truncate/reflink/cfr verified); getattr
  blocks synthesize from size. TWO accompanying finds: (a) the #40
  admission counted plaintext blocks but the drain reserves plaintext
  + AEAD tag/header (~1 block per emitted extent) — a PRE-EXISTING
  invariant hole that passed on margin luck; FIXED by charging +1
  block per started 8-MiB piece per range in `blocks_spanned`
  (symmetric at every footprint site; conservative ≥ needed under
  drain coalescing); (b) the two fallocate punch/collapse tests
  encoded the OLD transition's extent seam as if contractual —
  rewritten to construct per-block seams via commit-between-writes
  (extent granularity is write-pattern-dependent; punch cannot split
  an AEAD-blob extent, the documented MVP posture; F2 shifts which
  patterns give which seams). stratum ctest 73/73; writeback.tla
  clean GREEN + both buggy cfgs RED. **Measured (smp=4 instrumented
  boot 163621Z, clean pre-sentinel): S3 4093 → 3657 ms (−436); warm
  243. Mechanism confirmed server-side: transition encrypts 636 → 73,
  fw 810 → 362 ms, hw 862 → 381 ms (43.9 → 18.4 us/op), commits
  still 0; the queueing ripple lifted every band (wga 821→397, read
  524→311; guest write band 1795 → 936 ms).** First N=3 attempt
  VOIDED by a dirty post-sentinel (mediaanalysisd 208% — host media
  indexing); the quiet-gated retry (clean sentinels BOTH ends):
  **3810 / 4076 / ~3507 → med 3810 vs the pre-F2 med 4059 (−249 ms
  at med; the clean single boot showed 3657).** Boot-3's TIMING line
  was uart-interleave-mangled by a concurrent STMD26W print (the
  known never-commit-instrument artifact; the phase ran healthy —
  full DIAG23 block, rpc=41039 consistent; the fragments reconstruct
  to ~3.5 s). The instrument print volume grew this term (STMD26W
  fires per 200 ms-gated change), so keeper-build numbers should sit
  slightly better.
- Projected remaining after F1: 3657 − (W-A+W-B op-count collapse
  ~0.7-0.9 s + ripple) ≈ **~2.6-2.8 s vs the 2648 bar** — tight; the
  wga cold band (397 ms post-F2) is the reserve lever.

## FOUNDATIONAL candidates (empty until measured)

(none yet — a band lands here only with decision + mechanism + measured
magnitude + the removing redesign, and survives a Fable prosecution.)

## Opus audit correction (2026-07-12; the Fable prosecutor ran out of credits mid-run -> Opus 4.8 max fallback; MODEL start==end)

The adversarial audit of the wga narrowing (Surface A) + the FOUNDATIONAL
disposition (Surface B). **A: 0 P0 / 0 P1 / 0 P2 / 3 P3. B: 0 P0 / 0 P1 / 2 P2 / 1 P3.**

**Surface A -- the wga narrowing is SOUND (verdict).** Name-specific invalidation
is sufficient for EVERY guest-reachable synchronous mutation (create / mkdir-via-
create / unlink / rmdir / rename); the retained global `gen++` closes the
concurrent-populate resurrection independent of whether the mutated name was
cached; O(1), hash-correct, under the leaf lock, no torn serve, no new lock-order
edge; the old whole-parent drop was NOT accidentally load-bearing for any
guaranteed invariant; the regression is non-vacuous + in the committed tree. Three
P3s, ALL pre-existing-or-hygiene, NONE attributable to `b317fc28`:
- **A-F1 [P3]: reused-directory-qid dangling negative dentry (PRE-EXISTING).** A
  cached negative `(Q,"x")` survives when dir-inode `Q` is rmdir'd + reused as a
  new dir, because neither the old whole-parent drop (keyed by the *container*
  parent, never by `Q`) nor the new name-specific drop touches anything keyed on
  `Q`-as-parent. Identical under both versions -- the narrowing neither introduces
  nor worsens it. It is the L1f ino-reuse class (attr+pages defended at create)
  with the **dentry-children axis** undefended. ENQUEUED as task #51. Narrow
  (needs dir-qid reuse + a pre-cached negative child + reuse-as-dir); the
  create-forward cold go build rarely bites it.
- **A-F2 [P3]: the narrowing removes an *incidental, non-guaranteed* heal** of the
  pre-existing Loom-bypass attr staleness (a Loom SETATTR on a file the old
  whole-parent drop happened to force-re-walk via a sibling mutation). Root cause =
  the tracked L1f-F2 Loom-bypass seam; ZERO effect on the go build (sync SYS_* ->
  dev9p, no Loom mutations). Folds into the Loom-bypass seam.
- **A-F3 [P3]: strip-hygiene.** The uncommitted DIAG23-instrument working tree
  re-adds the dead `invalidate_parent` + leaves a stale larder.h:157 comment.
  RESOLVED by stripping the instrument (revert to the clean committed HEAD).

**Surface B -- the FS-cache exhaustion is HONEST; the "FOUNDATIONAL" HEADLINE is
NOT yet earned (verdict).** Credited honest: the F1b-dead [4,33] ms bound (robust;
the watermark undercount cannot flip it), the wga ~0-wall honesty, the S1-crossed
242 ms, the `fault=0` Image-cache confirmation, and -- explicitly -- the void-boot
handling as "exemplary no-flake discipline" (excluded from the median, enqueued as
#50, attributed away from the wga change). Two P2s demote the headline:
- **B-F4 [P2]: bulk prefetch is headlined-away.** Its plausible payoff (~150-400 ms
  addressable = read 329 + open 57 + part of walk 74; it does NOT touch the wga
  re-walk band, correctly) OVERLAPS the ~424 ms gap. A lever whose payoff overlaps
  the gap while UNMEASURED cannot be called "foundational" (a claim of
  impossibility) -- it is "unmeasured." FIXED: the headline + recommendation
  demoted above.
- **B-F5 [P2]: the ~1816 ms non-RPC bucket is asserted "compile CPU, host-
  comparable" but W2 was NEVER RUN.** The bar table itself shows W2-cold = "--".
  `fault=0` honestly rules out exec page-IN, but the bucket still contains the
  go-tool SPAWN floor x ~91 invocations (exec-setup + Go runtime init per
  compile/asm/link), whose host-comparability is extrapolated from a SINGLE
  driver-invocation parity (version-warm ~17 vs host ~30 ms), never measured for
  the 91 child spawns. OWED: run W2 (the C-2 protocol exists) to split compile-CPU
  from the spawn floor.
- **B-F6 [P3]: the transport-RTT framing understates.** ~40-55% of each first-touch
  RPC is SERVER CPU (`read=41us`/`meta=37us` server-side; the #367 territory,
  server-side-fixable), and the round-trip COUNT is further reducible by fusion
  (the POUNCE Twalk+Tgetattr precedent). Precise floor: the first-touch *count* is
  foundational; the per-touch *cost* + the round-trip *count* are open levers.

**Corrected disposition:** S1 crossed. S3's FS-cache-thrash levers are exhausted +
measured-insufficient. The FOUNDATIONAL determination is BLOCKED on two owed
measurements -- **(1) W2** (compile-CPU vs the ~91x spawn floor, B-F5) and **(2)
the bulk-prefetch ceiling** (B-F4) -- plus two open non-prefetch levers (server
per-op CPU / #367 + round-trip fusion, B-F6). Until those are measured, the honest
verdict is **FIXABLE-VOTED-pending**, not FOUNDATIONAL. The Senate's arc-exit call:
run the two measurements, attempt the prefetch lever, or accept the honest partial.

## Term-2 FINAL (2026-07-13): the owed measurements + the #50 root-cause + the EARNED verdict

The Senate delegated the arc-exit forks to the dictator. Sequenced soundness-first
(a surfaced defect on the measured path preempts the perf verdict), then the two
Opus-owed measurements, then the earned close.

### #50 (the ~10% S3 write-EIO) was the #375 out_buf clobber -- NOT Stratum/bdev

The first attribution ("Stratum/bdev race") was OVERTURNED by reading the WHOLE
failure window: the failing boot (`20260712T105542Z/device-3`) was a CLUSTER --
4 spurious ENOENTs on existing paths across $WORK+GOCACHE + 1 write EIO, not the
lone EIO first triaged. Spurious ENOENT on an existing path = a poisoned NEGATIVE
dentry; the guest installs a negative ONLY on a clean wire ENOENT (`dev9p.c:765`),
so the WIRE carried the lie. Root: the pre-existing **#375** `out_buf` clobber
(task #28, enqueued-unhunted since #349) -- `client_send_flow`'s pump/park drops
`c->lock`, and the retry re-read the SHARED `out_buf`; a peer's equal-length build
(two F1 128 KiB flush Twrites -- F1 made the shape common) went out as a clean
DUPLICATE with the parked frame LOST; the duplicate's 2nd reply hit the
freed-then-reused tag as a WRONG `Rlerror(ENOENT)` (parses for ANY op -- 9P has no
per-tag wire generation) -> poisoned a live `Twalkgetattr` for an existing
component -> everything beneath served ENOENT RPC-free. FIXED `53778794`
(spill-on-first-EAGAIN). Proof: repro-first regression (pre-fix 1103/1104 FAIL ->
1104/1104), spec gate GREEN, **N=10 post-fix S3 loop 10/10 clean, 0 clusters**.
Fable audit `e6a2e49a`: SOUND+COMPLETE, #50 chain PROVEN by the code, 0 P0/P1
(2 pre-existing P2s -> tasks #52/#53). The wga narrowing was NOT the root -- it
only lengthened the poison lifetime; with the root fixed the latch is sound.

**Bar (post-#375, N=10 clean, smp=4):** S1 warm median **245 ms -- CROSSED**
(<=266); S3 cold median **~3302 ms**, gap **~+654** to <=2648, all builds
succeeding.

### B-F4 (bulk prefetch): MEASURED DEAD

The S3-rerun experiment (an identical `cmd/gofmt` build on a 2nd fresh GOCACHE,
with the GOROOT FS fully Larder-warm from run 1): rerun median 3405 >= cold 3158
-- warming the static inputs between runs saves **~0**. The sharp reason: by the
S3 phase the earlier build1/build2 phases have ALREADY warmed GOROOT source
(Larder-resident), and the GOCACHE bulk is THIS-build-generated (unprefetchable) --
so a prefetcher has nothing cold left to warm. This also bounds every
read-cache-sizing lever to ~0 (the same null).

### B-F5 (the ~1816 ms non-RPC bucket): host-comparable (triangulated)

W2-via-`cmd/compile` proved UN-measurable on the device: the trimmed GOOS=thylacine
GOROOT lacks `preprofile` (default-PGO) and cmd/compile's toolchain deps
(`no such tool "preprofile"`; `-pgo=off` fails 0 ms on the device fork) -- a
bench-FIXTURE gap (the GOROOT trim), honestly noted, not a guest bug. The
triangulation substitutes: device S3 ~3302 = RPC ~1256 + non-RPC ~2044; host S3
2448 (~all non-RPC, local FS ~free). Device non-RPC (~2044) is NOT larger than
host non-RPC (~2350) -- so the compile-CPU + go-tool-spawn work is host-comparable,
and the device's +654..850 excess over the host lives in the 9P RPC band, NOT in
the non-RPC bucket. (Suggestive-not-clean under -p overlap; a clean W2 split needs
a fatter GOROOT bake -- a recorded fixture seam, not arc-blocking.)

### The EARNED verdict

The guest-side FS-cache levers are **EXHAUSTED and measured-insufficient**:
wga narrowing (~0 wall), F1b ([4,33] ms), read-sizing (inert), bulk prefetch
(~0, this term). The S3 residual is:
- **H1/H6 -- 9P first-touch RPC** (~1256 ms): the once-per-file cost a cold build
  cannot cache away. The first-touch COUNT is FOUNDATIONAL to a userspace 9P FS
  (Stratum by VISION/Plan-9 heritage). BUT (Opus B-F6) the per-touch COST is
  ~40-55% server CPU (`read=41us`/`meta=37us` server-side -- the #367 territory,
  server-side-reducible) and the round-trip COUNT is fusion-reducible (the POUNCE
  Twalk+Tgetattr precedent). These are a **SERVER-SIDE / protocol** arc, distinct
  from the guest CHASE.
- **H3 -- compile CPU** (~1816-2044 ms non-RPC): host-comparable (B-F5). Not a
  device lever.

**So (CORRECTED -- the honest verdict, self-caught 2026-07-13):** S3 is
**GUEST-EXHAUSTED, NOT FOUNDATIONAL.** Calling it "FOUNDATIONAL for the guest"
would be a SCOPE-DODGE (the same over-claim the Opus round already caught once):
the CHASE bar is `gofmt <= host+200` regardless of WHERE the fix lives, and a
NON-guest lever demonstrably exists -- the RPC band's per-op COST is ~40-55%
server CPU (#367, ~550-690 ms of the ~1256 ms band), a real chunk of which is
reducible (batching/caching the Stratum per-op path) and would close a large
fraction of the +654 ms gap; plus round-trip fusion (POUNCE precedent). So S3 is
**FIXABLE server-side**, not impossible -- it earns NEITHER a cross NOR an
impossibility proof, but a THIRD honest disposition: the GUEST-side levers are
exhausted + measured-dead (wga / F1b / read-sizing / prefetch), and the residual
reachable lever is a **Stratum #367 server-perf continuation arc** the guest-scoped
CHASE did not pursue. S1 is CROSSED (245 <= 266) -- the guest-side caching (Larder
+ fid-lifecycle) did its job. This is a correct SCOPING + a clean handoff, not a
failure and not an impossibility. (A quantitative bound on #367's reducible
fraction -- the difference between "large chunk" and "closes the bar" -- is the
final-audit + the Stratum-continuation's job.)

**SMP gate on the #375 fix: PASS** -- default+UBSan x smp4/smp8, N=10 = 40/40,
0 corruption (`tools/ci-smp-gate.sh`, 2026-07-13). The shared-FS-client spill is
production-clean.

### FINAL-AUDIT CORRECTION (2026-07-13; Fable, MODEL start==end; 0 P0 / 2 P1 / 2 P2 / 1 P3)

The adversarial audit of the earned verdict broke it further -- the honest
disposition is the charter's own **FIXABLE-VOTED**, and even "all GUEST levers
exhausted" is NOT fully established:

- **[P1, F1] the disposition is FIXABLE-VOTED, not any flavor of FOUNDATIONAL.**
  CHASE.md section 1 requires the impossibility branch to show the fix is an
  ARCH-level redesign; "a floor that is actually fixable fails the proof." The
  residual is admitted-fixable (server #367 ~40-55%25 of the RPC band + POUNCE
  fusion, est. -400..-650 ms >= the +654 gap), and the guest/server scope line
  was NEVER honored DURING the arc (C-2 F2 + C-3 were in-arc Stratum fixes;
  POUNCE/B1/F1 were cross-tree; Stratum is in-scope by standing scripture). So
  invoking it only at exit is the scope-dodge. Disposition: **S3 = FIXABLE-VOTED
  (Stratum #367 per-op + protocol fusion + msize; the Senate votes the arc).**
- **[P1, F2] B-5 "non-RPC host-comparable" is RETRACTED as arithmetically
  unsound.** `rpc_ms` is a SUM over concurrently-in-flight ops (pre-fix boot
  102334Z: `rpc_ms=7617 > wall 5594`, 60%25 of ops at depth >=2), so
  `wall - rpc_ms` is NOT a non-RPC WALL footprint. The device non-RPC bucket is
  therefore UNMEASURED, and it hides a candidate **GUEST** lever -- the ~91x
  go-tool spawn/exec floor (H3: exec/REVENANT/runtime-init, guest-kernel
  territory) + the H5 8-vCPU-scaling band -- that W2-device was meant to split
  but could not (the trimmed-GOROOT fixture gap). So "all GUEST levers
  exhausted" is NOT established: one guest band (the spawn floor) is UNMEASURED.
  Per this ledger's own header an unattributed band means C-1 is not done -> the
  impossibility proof is INCOMPLETE regardless of the label.
- **[P2, F3] the prefetch-null CONCLUSION stands (prefetch <= ~300 ms, N=9
  rerun 3352 vs cold 3162 -- indistinguishable from 0 at the +654 scale) but its
  stated MECHANISM was wrong:** the rerun RE-WIRED the full ~92 MB read volume
  (`abs` 2492 >= 2007, `qids` 686 vs 241, `pe~10.4k`) -- the Larder does NOT
  retain the working set across a build; the true basis of the null is that the
  re-payable read band is OFF the critical path (overlaps compute). The
  fresh-GOCACHE-both-runs control design is VALID (a rerun's warmth ceiling
  dominates any real prefetcher's), so prefetch-dead holds -- on the corrected
  reasoning.
- **[P2, F4] completeness hole:** the msize lever (128 KiB -> 512 KiB, ~2-4x
  fewer flushes on multi-MB objects, ~-100..-150 ms) -- a guest+server co-design
  like POUNCE/F1 -- was never enumerated. Belongs priced inside the continuation.
- **[P3, F5] provenance:** the N=10 keeper boots print `rpc=0` (RPC instruments
  stripped), so the "RPC ~1256" decomposition above is INHERITED from the
  instrumented builds (~3072-3166 walls), not contemporaneous with the 3302
  median.

**Corrected close: S1 = PARITY (crossed, real guest win). S3 = FIXABLE-VOTED,
impossibility proof INCOMPLETE** -- owed before any close: (1) device W2 (fatter
GOROOT bake) OR a spawn-storm microbench to measure the ~91x spawn/exec guest
band + a post-F1 depth histogram to convert `rpc_ms` to a wall footprint; (2)
the server-#367 reducible-fraction bound (does it close the bar?); (3) price
msize. Only "no guest FS-CACHE lever reaches the bar" is earned (wga/F1b/
read-sizing/prefetch). The task-#54 continuation owns (1)-(3).
