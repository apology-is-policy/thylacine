# The B1 loose-mode vote (CHASE C-3, the wga band)

Status: **VOTED 2026-07-11 — the Senate chose B (per-attach opt-in;
I-38 default stays strict).** The sequence below is in motion:
scripture -> impl -> focused audit -> N=3 re-measure -> ledger. The readdir band is
fixed (stratum 750fab1; warm med 749 -> 439 ms). The wga band (~200 ms
warm, ~1414 ms S3) is the next largest, and its fix touches the I-38
DEFAULT -- a scripture decision, so it is the user's vote, not the
dictator's.

## What the band is

Post-C-3 warm window: wga ~200 ms over 2356 ops. Split: ~1604 are the
cached-open B2 FORCED-WIRE revalidation (fid-lifecycle arc: a plain
OREAD open of a fully-page-cached file mints a fidless snapshot, but
first issues ONE query-Twalkgetattr to revalidate cvers -- the I-38
close-to-open `Open` step); ~535+ are bind-form walks for real wire
opens (not fully cached); the rest are first-touch stats. B1 removes
the ~1604; the bind walks remain. Expected warm effect: -120..-150 ms
(with the read band, the S1 bar <=266 comes in reach from 439).

## What B1 is

Serve the cached-open snapshot with NO per-open wire revalidation when
the file is fully page-cached and cvers-known (v9fs cache=loose
semantics). Coherence keeps TWO legs: own-write invalidation (the
Larder drops attr+pages on every local mutation) and the cvers gate on
page serves (an EXTERNAL change is picked up whenever any fresh attr
arrives). What it drops: the GUARANTEE that every open observes the
server state as-of-open -- an external writer's change can be served
stale until some other op refreshes the attr.

## Why it is sound at v1.0 (the fit analysis)

1. **Single-writer-by-construction**: the pool's block device is owned
   exclusively by the guest's stratumd (virtio-blk claim, I-5). There
   is no host-side concurrent mounter and no second guest. The only
   writers are Procs of THIS guest, and every guest write path passes
   through the same kernel Larder -> own-write invalidation covers it.
2. **Dataset partition**: the two concurrent 9P sessions (the system
   mount + the A-5b per-user home proxy) reach DISJOINT datasets
   (--datasets-allowed + ds:<user> aname); no file is reachable through
   two Larder instances. (A future shared-dataset multi-session config
   breaks this premise -- named below as the re-strict trigger.)
3. **Precedent**: Linux v9fs cache=loose is exactly this mode and is
   the recommended single-client 9P configuration; Plan 9's cfs made
   the same bet. The strict mode exists for multi-client shares, which
   v1.0 does not have.
4. **The go build is self-coherent under it**: cmd/go reads what it
   wrote (GOCACHE, $WORK); own-write invalidation preserves exactness.

## The options

- **A. Loose as the v1.0 global default.** Scripture: I-38 text gains a
  single-writer-configuration exemption; strict returns when a second
  writer class appears (multi-guest, host-side mutation tooling,
  shared-dataset sessions). Simplest; biggest win; the exemption is
  argued from I-5 exclusivity, not hope.
- **B. Per-attach opt-in flag** (RECOMMENDED): a loose flag on the
  9P attach (the Larder is per-p9_client, so the flag is client-level;
  joey opts the SYSTEM mount in at boot; the per-user proxy mount can
  follow). I-38's default stays strict on paper; the exemption is
  confined to the mounts where the single-writer argument is checked.
  Cost: one attach-flag plumb (SYS_ATTACH_9P arg or a post-attach ctl)
  + the joey call site.
- **C. Cvers-lease/TTL hybrid** (serve loose within a short lease,
  revalidate after): more design surface for marginal v1.0 benefit
  under premise (1). Not recommended now; it is the natural v1.x shape
  if premise (2) ever weakens.

## What the dictator asks of the Senate

Vote A / B / C (or amend). On a vote for A or B the sequence is:
scripture commit (I-38 text + LARDER/FID-LIFECYCLE design notes) ->
impl -> focused audit -> N=3 re-measure -> ledger disposition
(FIXABLE-VOTED -> FIXED). The read band (~160 ms) needs NO vote and
proceeds in parallel -- but its first step is a miss-class instrument,
not sizing: the page cache is already 32768 slots / 128 MiB (#25); the
warm wire reads are likely >128K cached-open fallbacks (the ~4 MB
binary copy-back) + first-touch files, not capacity misses.
