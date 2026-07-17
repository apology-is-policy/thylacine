# FID-LIFECYCLE — eliminating the per-file 9P round-trip floor

Status: **DESIGN (scripture-before-code)** — user-voted 2026-07-09 (the two forks
below resolved: cached-open coherence = **B2 revalidate-at-open**, preserving
I-38; land this scripture commit, then build async-clunk first). Binding;
implementation lands against it. The next lever after the Larder (L1) on the
on-device `go build` mission ([[project-go-build-clean-perf]]).

---

## 1. The measured problem (task #23)

The Larder (L1) killed the go-build metadata **stat/walk storm** (getattr=0 RTs,
~11,600 attr+dentry hits per warm build). But a per-9P-type instrument (DIAG23,
2 boots) decomposed the residual warm floor — the trivial-hello warm build
(`build2-warm`, ~810-986 ms) — to ground and REFUTED the L1f "fixed go-tool
overhead" theory:

| fact | measured | verdict |
|---|---|---|
| exec page-in (`fault`) | **0** | tools stay Image-cached — not the floor |
| go-tool startup (`version-warm`) | 37 ms, 114 RTs | not the floor |
| **the floor** | **~8250 serial 9P round-trips** (~1100 ms cumulative ≈ the whole wall) | **9P per-file overhead** |

Per-9P-type split of the ~8250 RTs:

| op | RTs | % | the Larder verdict |
|---|---|---|---|
| **read** | 2590 | 31% | page cache thrashes (lp=63 = 2.4% hit) — the sequential-scan-larger-than-cache LRU pathology |
| **bind-walk** (Twalkgetattr) | 2181 | 26% | the dentry cache serves the *metadata*, but a bind-form walk still RPCs to bind a server fid |
| **clunk** (Tclunk) | 1703 | 21% | fid release — not cacheable |
| **open** (Tlopen) | 1646 | 20% | fid open on the walk-bound fid — not cacheable |

**The reframe:** the warm floor is the **per-file fid lifecycle**
(bind-walk + open + clunk = **67%**) plus **content read-misses** (31%). Every
small go-cache file the build touches costs ~4 mandatory round-trips even though
its metadata is Larder-cached. Host does the same warm build in 130 ms; the ~7×
gap is entirely this per-file 9P round-trip overhead. Full data:
[[project-warm-floor-decomposition]].

---

## 2. Prior art (why this is a novel synthesis, not catch-up)

- **Linux v9fs `cache=loose`** (the closest SOTA; the register's LWN-1060656
  reference where `cache=loose` 9p BEAT virtiofs on a compile, 100% from
  metadata/negative-lookup caching) caches dentries/inodes/pages across opens —
  but it **always binds a server fid to open** (Twalk-clone + Tlopen + Tclunk per
  open). It eliminates the *reads* (page cache) and *path-lookup walks* (dcache),
  NOT the per-open fid lifecycle. A re-opened cached file still pays
  walk+open+clunk on the wire.
- **NFSv4 delegations** are the only SOTA that serves opens/reads/closes fully
  locally — via a server→client **callback** to recall the delegation on a
  conflicting access. 9P has no callback channel (server never initiates).
- **Plan 9 `cfs`** caches file DATA (blocks, `qid.version`-validated,
  close-to-open) but the client still opens fids to the (local) cfs.

**Thylacine's angle — the *fidless close-to-open open*:** serve a read-only
open+read+close from the Larder with a single close-to-open **revalidation**
getattr — no fid bind, no lopen, no clunk. It goes beyond v9fs (which always
binds) WITHOUT needing NFS-style callbacks, because the Larder's existing
close-to-open discipline (I-38) supplies the coherence. Candidate NOVEL.md entry.

---

## 3. The arc — three mechanisms

Projected: warm floor ~8250 RTs → **~2000 RTs (~4×)**, the residual being the
close-to-open revalidation (1 RT/open — the *correct* minimum for close-to-open).

### 3.1 async-clunk (−21%; composes I-10 / I-11; the tractable quick win — sub-chunk 1)

**Today** (`kernel/9p_client.c::p9_client_clunk`, 1104): build Tclunk via
`p9_session_send_clunk`, then `client_run` **BLOCKS** on Rclunk, then return. The
submitter's thread is parked for the clunk RTT on every file close.

**Ground truth that makes this nearly hazard-free** (the fid-lifecycle map,
2026-07-10):
- The fid **number** allocator (`p9_client_alloc_fid`, 9p_client.c:1585) is a
  monotonic counter that **NEVER reuses a fid number** (v1.0; the 32-bit space is
  the budget). So the classic async-clunk hazard — "don't reuse a clunk-pending
  fid" — is satisfied **unconditionally**. No fid-number reuse guard is needed.
- The fid **binding table** (`bound_fids[]` on `struct p9_session`, I-11) unbinds
  at **send** time inside `p9_session_send_clunk` (9p_session.c:375), already
  reply-independent. `any_outstanding_on_fid` (9p_session.c:153) already refuses a
  clunk while a live op targets the fid.

**The ONE residual hazard = the tag (the `outstanding[]` slot, I-10).** `alloc_tag`
returns the lowest free of `P9_SESSION_MAX_OUTSTANDING = 64` slots; the slot is
freed only when its reply is dispatched. A fire-and-forget Tclunk whose Rclunk is
never reaped permanently burns a slot → tag-pool exhaustion → the client stalls.

**The design:** send the Tclunk **without blocking** — `mark_outstanding` the tag
(no submitter thread waits), and let the elected reader **drain the Rclunk
ownerless** (dispatch it, `clear_outstanding` frees the tag). This is EXACTLY the
**#845 Tflush ownerless-dispatch** machinery already in the tree (an ownerless
Rflush frees its oldtag; extend the same dispatch arm to an ownerless Rclunk).
`dev9p_close`'s `p9_client_clunk` becomes `p9_client_clunk_async` (fire-and-forget)
on the normal close path; the error-path clunks (dev9p_walk_attrs/walk/create) may
stay synchronous (rare, off the hot path) or also go async — an implementation
choice, prosecuted in the audit.

**Why the tag pool does not exhaust:** a build closes files roughly serially
(close file N, open file N+1). At most a few clunks are in flight concurrently
(the fired clunk pipelines with the next op's walk); the elected reader drains
Rclunks continuously as they arrive, so a tag is held only for the clunk RTT, not
forever. 64 slots is ample. The WIN is that the submitter no longer *blocks* on
the RTT — the clunk overlaps subsequent work.

**Invariants:** composes **I-10** (the tag stays reserved until its Rclunk drains,
then freed — the #845 discipline, now with no submitter to wake) + **I-11** (fid
unbound at send; number never reused). **No new invariant.** **Spec-first:**
extend `specs/9p_client.tla` with the async-clunk model + a buggy cfg
(`async_clunk_tag_leak`: a fire-and-forget that never drains → the `outstanding[]`
slot is never freed → tag-pool exhaustion). Session-death: clunk-pending tags are
abandoned with the session (client-side already unbound; no leak on our side).

### 3.2 page-cache sizing (−31%; the prerequisite — sub-chunk 2 = task #25)

The page cache is at **2.4% hit** — the sequential-scan-larger-than-cache LRU
pathology: build1 reads ~2590 pages sequentially into 512 slots; build2's
sequential re-read evicts the tail before reaching it (~0% hit). A content cache
helps ONLY if it holds the WHOLE working set (>2590 slots; the #343 dcache lesson:
cross the working-set threshold or get nothing). **A 4096-slot INLINE
`larder_page_ent` array balloons `struct larder` ~200 KiB inside a kmalloc'd
`p9_client` (a risky order-7 alloc)** — so the fix is a small refactor: make the
page slot-metadata array HEAP-allocated (a pointer + one kmalloc in `larder_init`,
freed in `larder_destroy`), THEN size it to the working set (measure per
workload). This is the **hard prerequisite for cached-open** (§3.3 serves reads
from the page cache — with nothing cached, cached-open never engages). Audit-
bearing (the L1 dev9p surface); tracked as task #25.

### 3.3 cached-open — the fidless revalidate-at-open (the 46%; refines I-38; sub-chunk 3)

**Today:** resolving `/go-cache/xx/file` binds a server fid at the **walk**
(`dev9p_walk_attrs` bind-form, Twalkgetattr — 1 RT), `dev9p_open` does a **Tlopen**
on that bound fid (1 RT), reads (page-hit or RT), and `dev9p_close` **Tclunks**
(1 RT). Three fid RTs + reads per file.

**The mechanism (B2, revalidate-at-open — the user-voted coherence, preserves
I-38):** on a **read-only** open of a file whose content is fully page-cached, do
ONE **query-form** Twalkgetattr to the server (binds no fid, 1 RT) to obtain the
file's **fresh `cvers`** — this IS the I-38 close-to-open `Open` revalidation.
Then:
- If the returned `cvers` matches the page cache's cached content for that
  `qid.path` AND all covered pages are resident → **snapshot the file's content
  `[0, size)` into a private per-open buffer** (one Larder lock hold — atomic vs
  a concurrent invalidate) and mint a **fidless "cached-open" Spoor**
  (`dev9p_priv.fid = P9_NOFID`): its `dev9p_read` serves from the snapshot, and
  its `dev9p_close` frees the snapshot — a **no-op on the wire** (no Tlopen was
  done, no fid to Tclunk). **Zero further server RTs.**
- On any miss (chain not dentry-cached, a covered page not resident, `cvers`
  mismatch, size over the cap, budget exhausted, snapshot OOM, or a non-plain
  open mode) → **fall back** to the current path (bind-walk + Tlopen + reads
  that populate the caches). Cached-open is a *fast path*, never a requirement.

**Why a private snapshot, not live serves from the page cache (the 2026-07-10
impl-design refinement — scripture-first):** a fidless Spoor has **no recovery
channel for a read-time miss**. If reads served from `larder_page_serve` directly
and a covered page were evicted (LRU pressure) or invalidated (a concurrent
own-write to the same file) mid-open, the fidless read could not fall back — 9P
has no "fid from qid" op, so a fidless Spoor cannot late-bind, retained path
re-walks are rename-unsound (POSIX: an open fd survives rename/unlink), and
returning 0 at a non-EOF offset is a **false EOF = silent data corruption**.
Pinning the pages in-cache for the open's lifetime was weighed and rejected: it
couples into the L1f-audited eviction + own-write-invalidate paths (a pinned
page surviving an own-write must become snapshot-only, or the writer's own
read-after-write would serve pre-write bytes at its matching open-time cvers — a
NoWrongRead violation), a materially larger audit surface on the most-audited
path. The private copy keeps the Larder **byte-identical to its audited state**
(the only additions are pure readers), and gives exact close-to-open semantics:
every read of the open serves the open-time content (what NFS close-to-open and
a snapshot-at-open both mandate; a concurrent writer's changes surface at the
NEXT open's revalidation). Cost is trivial for the target workload (~1.6 pages
per file measured; a memcpy of ~6.5 KiB per open).

**Bounds (the CF-3 bounce-budget class — user-drivable kernel heap):** the
snapshot is kernel memory a hostile EL0 program could otherwise inflate, so (a)
a **per-file size cap** (`DEV9P_CO_MAX_SIZE`, 128 KiB — beyond it the fid RTs
amortize against the read volume anyway) and (b) a **global outstanding-bytes
budget** (`DEV9P_CO_BUDGET`, 8 MiB, atomic charge at mint / uncharge at close).
Global, NOT per-Proc: a cached-open fd outlives its syscall and crosses Proc
boundaries (rfork inheritance, handle transfer), so a per-Proc charge would
unbalance at close-by-inheritor. Exhaustion degrades the fast path only
(fallback is the normal open — fail-safe, no correctness or DoS exposure).

**The Dev slot + the resolver contract (who checks what):** a single
NULL-permitted vtable slot `Dev.open_cached(c, names, name_lens, nname, sts)`
called by stalk on the **final run** of a plain `STALK_OPEN`/`OREAD` resolution
(exact `omode == 0`; OTRUNC / write / OEXEC / O_PATH never take it). The Dev
does internally: (1) an RPC-free **hint** (dentry-chain the run + leaf attr +
full page coverage under the Larder lock — a non-eligible open costs nothing);
(2) the **forced-wire query** Twalkgetattr (`newfid = P9_NOFID`), issued
directly at the client, deliberately **bypassing the L1d dentry serve** — the
revalidation MUST be server-fresh or B2 silently degrades to B1 (the
prosecution item); (3) fresh-leaf checks (plain regular file, size cap) +
budget + snapshot at the fresh `cvers`; (4) mint the opened fidless Spoor.
It returns the Spoor **plus the fresh per-component records in `sts`**, and the
resolver — never the Dev — then runs the **mandatory fail-ordering post-scan**:
per-component X-search on the fresh records + mount-membership scan (ANY mount
hit, including the leaf, discards the cached open and falls back to the normal
path, whose split/cross machinery handles the crossing) + the final-hop R/W
`perm_check` on the fresh leaf record. Permission stays in the resolver
(the I-28/I-22 chokepoint is not fragmented into Devs); a denial destroys the
minted Spoor (a wire-free close) and fails with the identical error the normal
path would produce. On a NULL return nothing was bound or revealed — the
caller's observable outcome comes solely from the fallback path.

**Fidless-fd semantics (the v1.0 seam, documented + tested):** `fstat` serves
the open-time snapshot stat (the same close-to-open discipline as the attr
cache); `fsync` is a no-op success (read-only fd — POSIX-legal); `read`/`pread`
serve the snapshot; `write` is rejected (defense-in-depth — the omode-derived
handle rights already deny it). **`SYS_WSTAT` (fchmod/fchown *on the fd*)
returns -1**: Tsetattr is fid-addressed and a fidless Spoor cannot late-bind
(see above). No v1.0 consumer fchmod/fchowns an O_RDONLY dev9p fd (verified:
cmd/go's cache mtime updates are path-based `Chtimes`; the #47 kind-gate fix was
conformance-driven, not consumer-driven); path-based chmod/chown are untouched.
The divergence fails LOUD (-1), is regression-tested, and the v1.x fix — if a
real consumer appears — is the retain-the-walk-fid-unopened variant (wstat and
getattr work on an unopened fid; costs the async clunk back). Every other wire
op on a fidless Spoor (walk, walk_attrs, create, readdir, rename, unlink,
re-open) is `P9_NOFID`-guarded to fail cleanly rather than emit a NOFID wire op.

**RT accounting for a cached small file:** 3 fid RTs (bind-walk + lopen + clunk)
+ reads → **1 RT** (the query-getattr revalidation) + local reads = a 3-4× cut,
and the residual 1 RT is the correct close-to-open minimum.

**Why B2, not B1 (the resolved fork):** B1 (fully-loose — trust the cached attr,
serve the query-walk locally, 0 RT/open) would **weaken I-38** — it drops the
close-to-open revalidation, so an external writer's change is invisible until an
own-write or LRU eviction. Sound for a single-writer read-only tree
(goroot/go-cache), but I-38 is an enumerated invariant governing ALL dev9p, and
the marginal gain (1 RT/open) is not worth weakening it globally. B2 preserves
I-38 and still delivers ~4×. **B1 is deferred as a possible future opt-in
per-mount `cache=loose` mode layered on B2** — not v1.0.

**B1 as-voted (2026-07-11; the CHASE wga band; `docs/chase/B1-VOTE.md`):** the
Senate chose **option B — the per-ATTACH opt-in**, keeping I-38's default
strict. Mechanism, layered exactly on B2:

- A new `SYS_ATTACH_9P_SRV` flags argument (x4; the #112 every-caller-sets-it
  ABI discipline) with `SYS_ATTACH_9P_LOOSE = 0x1`; unknown bits reject. The
  flag flows `srvconn_attach_dev9p_root(..., loose)` → `p9_client.loose`, a
  plain bool set ONCE before the root Spoor publishes (ordered by the handle
  publication; no runtime flip, no atomics needed).
- In `dev9p_open_cached`: on a **full** step-1 hint hit (positive dentry chain
  + leaf attr + full page coverage) a LOOSE client skips the forced-wire query
  and proceeds to the fresh-leaf gates + budget + snapshot **at the cached
  cvers**, with the hint's cached records as the resolver post-scan input (the
  same attrs the L1c base X-check already serves — the permission axis is not
  weakened beyond L1c's accepted discipline). Any hint MISS falls through to
  the unchanged B2 forced-wire query — first touch always wires. A STRICT
  client's path is byte-unchanged.
- Soundness rests on the checked single-writer premise (the I-38 row carries
  the full argument + the re-strict triggers): I-5 exclusive block-device
  ownership, dataset-partitioned sessions, own-write invalidation, and no
  cross-session byte reader of the one foreign-written subtree
  (`/var/lib/corvus`). joey opts the SYSTEM mount in; the per-user home proxy
  may follow (its dataset is strictly session-private — a cleaner premise).
- The concurrency window is benign by construction: the snapshot re-verifies
  coverage at the cached cvers under one Larder-lock hold, so a concurrent
  own-write invalidate either precedes (coverage fails → fallback) or follows
  the whole copy — the same atomicity B2 relies on.

**Invariants: refines I-38, no new invariant.** The fidless open is close-to-open
because the query-getattr at open IS the `Open` revalidation (fs_cache.tla); the
snapshot is taken at the fresh `cvers` atomically under the Larder lock (a
concurrent own-write invalidate either precedes it — failing the coverage — or
follows the whole copy); the fallback is byte-identical to today. The served
open+read+close is exactly what a bind-walk + lopen + reads + clunk would return,
minus the fid RTs. **Spec-first:** the cached-open path maps onto the EXISTING
`fs_cache.tla` `Open`+`Read`+`OwnWrite` actions (the query-getattr = `Open`; the
snapshot = `Open`-time `Read` of every covered page, served unchanged for the
open's lifetime — close-to-open allows serving open-time values; no new action) —
the model already proves it; a prose SPEC-TO-CODE mapping suffices, or a focused
`cached_open` cfg if the audit surfaces subtlety. Prosecute in the focused audit:
the fidless-Spoor lifetime (no server fid to leak; close is wire-free; the
snapshot buffer + budget balance on EVERY path — mint, denial-destroy, close,
handle-transfer); the "fully page-cached" detection soundness (a partial-cache
open MUST fall back, not serve a hole; the msize-clamped mid-file partial page —
`valid_len` short of the boundary — fails coverage); the `cvers` revalidation
completeness (every cached-open engagement gated on the FRESH wire cvers — the
forced-wire query must bypass the dentry serve, else B2 silently degrades to
B1); the read-only gate (`omode == 0` exactly — write / OTRUNC / OEXEC / O_PATH
never take the fidless path); the resolver post-scan completeness (X-search +
mount scan + leaf R/W on the fresh records — the fail-ordering invariant, I-28);
the `P9_NOFID` guards on every wire op reachable from a fidless Spoor; the
`SYS_WSTAT` seam (loud -1, tested); the fallback byte-identity.

---

## 4. Sub-chunk plan + sequencing (user-voted: scripture first, then async-clunk)

1. **This scripture commit** (no code): this doc + ARCH I-38 refinement +
   §25.4 audit-trigger rows + CLAUDE.md rows + NOVEL.md candidate + memory index.
2. **async-clunk** (sub-chunk 1) — the tractable I-10/I-11 win (21%); extend
   `9p_client.tla` (async-clunk cfg) + the focused audit (the #845 ownerless-drain
   surface) + the DIAG23 re-measure (clunk RTs → off the critical path).
3. **page-cache sizing** (task #25) — heap page array + size to the working set;
   the read-miss win (31%) AND the cached-open prerequisite; focused audit + the
   DIAG23 re-measure (lp hit-rate ↑, read RTs ↓).
4. **cached-open** (sub-chunk 3) — the fidless revalidate-at-open (B2); refines
   I-38; the focused audit + the DIAG23 re-measure + the SMP gate + the gofmt
   re-measure (the arc payoff proof, ~4×).

Each sub-chunk lands independently with its own audit + SMP gate + a DIAG23
re-measure; the whole arc closes on a final gofmt cold/warm re-measure vs the ~4×
projection.

---

## 5. Alternatives considered + rejected

- **B1 fully-loose cached-open** — rejected as the v1.0 default (weakens the
  enumerated I-38 globally for a 1-RT/open marginal gain); deferred as a possible
  future opt-in per-mount mode.
- **async-clunk only (defer cached-open)** — rejected as the endpoint (leaves the
  46% open/bind-walk on the table), but IS the sequencing (async-clunk ships
  first).
- **Loom-pipeline the fid lifecycle** — hides the RTT of the mandatory RTs but
  does not eliminate them; ranks below elimination (eliminate an op > hide its
  latency). A residual lever after this arc if the revalidation RTs still bind.
- **A server-side fused open** (a 9P `Topenfd`-style walk+open+read in one RT) —
  would help the *cold* path (uncached) but not the warm re-open redundancy this
  arc targets; a separate future lever.

---

## 6. As-built (2026-07-10)

**async-clunk** landed as designed (`p9_client_clunk_async`, the ownerless
Rclunk drain via the #845 dispatch arm, `dev9p_close`'s normal path). The
`async_clunk_tag_leak` buggy cfg extends `specs/9p_client.tla` (the clean model
needed NO new action — SendClunk already holds the tag until ReceiveOp drains
it, and the spec never modeled a blocking submitter; the buggy action injects
the reply-consumed-but-tag-never-freed leak, caught by TagAndOpAccounting).

**cached-open** landed per §3.3 (the snapshot refinement): `Dev.open_cached` +
`dev9p_open_cached` (hint → forced-wire query → fresh gates → budget →
snapshot → fidless mint) + the stalk pounce-block arm (post-scan + mount
discard + leaf R/W on the FRESH records) + `larder_pages_cover`/`_snapshot`
(pure readers). The wstat-on-fidless seam is DOUBLE-enforced: the dev9p
`cached_open` guard + the session layer's I-11 `fid_bound` send gate
independently refuse the NOFID Tsetattr.

**The metadata-cache re-size (the engagement fix, measured via the DIAG23
funnel):** the first instrumented boot showed 86% of cached-open attempts dying
on the hint's metadata chain (`hchain=1525/1782`) — the attr + dentry caches
were 256 slots (the L1c base-X-check sizing) against the ~800+ distinct leaves
a warm build opens; leaf entries evicted before re-open (the #343/#25
working-set-threshold lesson). Fix: both caches heap-allocated + O(1)
chained-hash + CLOCK (the task-#25 page-array pattern replicated) at 4096
slots. Post-fix funnel (build2-warm): `hchain=10`, `MINTED=396`; open RTs
1634→1317, total RPCs 6888→5781; **build2-warm 645→613 ms and gofmt-warm
785→727 ms — both best-ever**. The residual funnel kill is `hcover` (~1185):
files opened but only PARTIALLY read (cmd/go buildid Preads, importer section
reads) never reach full page coverage — the fallback is the structurally
CORRECT disposition for them (snapshotting a 100 KiB archive to serve a
100-byte Pread would be waste), so this residual is not a defect. `wire=0
fresh=0 budget=0` across every boot: the hint's precision means the forced
query is never wasted and the 8 MiB budget is never touched at this workload.

---

## 7. Cross-links

[[project-warm-floor-decomposition]] (the measurement) ·
[[project-go-build-clean-perf]] (the mission register) ·
[[project-fs-cache-design]] + `docs/LARDER-DESIGN.md` (the Larder this extends) ·
`docs/POUNCE-DESIGN.md` (the Twalkgetattr the revalidation reuses) ·
ARCH §28 I-38 / I-10 / I-11 · `specs/fs_cache.tla` + `specs/9p_client.tla`.

## 8. The dir-fid cache (G2, term-4; 2026-07-13)

The T4-G A/B (docs/chase/LEDGER.md) measured ~3.7k of the S3 window's 6836
wire Twalkgetattr as BIND-FORM walks over FULLY-CACHED chains -- the RPC's
only remaining purpose is minting the server fid -- plus 901 leaf-perm_only
bails that are mostly bind-walks TO the just-mutated dir itself. The dir-fid
cache (the v9fs dentry-fid / Plan 9 mount-driver idiom, adapted to the
Larder's coherence machinery) makes the repeat resolution ZERO wire ops.

### Mechanism

A 64-entry table on `p9_client` (`struct p9_dirfid_cache`, 9p_client.h) parks
walk-fresh (never-opened) DIRECTORY fids keyed by qid.path; ALL policy lives
in dev9p.c:

- **Consume** (`dev9p_walk_attrs`, bind form): when the Larder chain fully
  serves and the leaf is a dir with a parked fid, `dirfid_take` REMOVES the
  entry and the walked Spoor is minted from it -- no RPC. A perm_only leaf
  serves this path (the new `leaf_perm_only` out-param on
  `larder_walk_serve`): the bind consumer reads only mode/uid/gid + the
  immutable qid fields, all fresh under a G3 downgrade; every other caller
  still treats perm_only-leaf as a miss.
- **Donate** (`dev9p_close`): an unopened dir fid on a cacheable client parks
  instead of clunking (dedup + round-robin evict victims are clunked outside
  the leaf lock). The by-name flow -- resolve parent, op, close -- thus
  recycles one fid indefinitely: unlink/rename/mkdir storms resolve 0-RT
  after the first touch.
- Ownership is exclusive at every step (take removes; the closing priv stops
  owning at put) -- one Spoor per live fid, I-11 preserved. Parked fids die
  with the session (no destroy-time wire).

### The stale-fid defense (three layers)

A fid tracks an INODE. The hazard: a parked/checked-out fid for a dir that is
rmdir'd (or replaced, or whose ino a create reuses) names a dead object; a
fresh walk re-resolving the REUSED qid.path must never be served it, and a
poisoned fid must never re-park in a loop.

1. **Drop hooks** (parked fids): `dev9p_create` drops the entry at the
   returned qid (ino reuse -- the L1f-F1 twin); `dev9p_unlink` and the
   rename-REPLACE arm resolve the victim's qid from the cached (parent,name)
   dentry BEFORE the wire op (`larder_dentry_lookup`), drop + clunk the
   parked fid, and invalidate the victim's own attr -- the HARD event that
   arms layer 2.
2. **The donate staleness gate** (checked-out fids): every priv records
   `fid_gen` (the Larder invalidation-gen at its fid's mint or take);
   `dev9p_close` parks only if NO HARD event named the qid since
   (`larder_qid_staled_since` over the G4 ring). Events are kind-tagged
   (`inval_hard[]`): attr_invalidate = HARD (identity death: rmdir/rename
   victims, create-reuse, wstat); the G3 parent downgrade + dentry + page
   events = SOFT -- a by-name op downgrades its own parent on every use, and
   treating that as fid-death would block exactly the recycle the cache
   exists for. The install guard still scans ALL events.
3. **The suspect backstop** (the unknowable residual): if the victim's
   dentry was evicted before the mutation, no event names the qid -- so any
   by-name op ERRORING through a priv latches `fid_suspect`, and the close
   clunks instead of re-parking. A stale fid dies after at most one failed
   cycle; the observable cost is one honest server error (the same
   close-to-open race outcome as a pre-G2 walk that resolved before the
   mutation), never corruption.

### RT accounting (why this is the honest shape)

A CONSUMING op still needs >= 1 RT whatever cache exists: Tlcreate transitions
the passed fid to the new file, Tlopen mode-binds it, and a 0-name Twalk clone
is itself 1 RT -- so creates and opens keep their single fused-wga RT. The
0-RT wins are the NON-consuming by-name ops (unlinkat/renameat operate on the
dirfid by name without transitioning it) and every repeat parent resolution
between them. Consumed-then-opened dirs (readdir) save their bind RT and the
fid dies at close (opened fids are never parked).

Tests (all revert-probed; 2 probe builds -> 5 distinct targeted failures):
dev9p.dirfid_{consume_and_recycle, perm_only_leaf_consume, create_reuse_drop,
rmdir_drop_and_no_stale_repark, suspect_not_reparked}. The co_prime test
helper drains its async Rclunk only when the close actually clunked (a donate
elides it, and a pump with nothing pending latches the single-slot loopback
dead).
