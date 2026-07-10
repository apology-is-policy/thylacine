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
  `qid.path` AND all covered pages are resident → mint a **fidless "cached-open"
  Spoor** (`dev9p_priv.fid = P9_NOFID`, a new `CACHED_OPEN` state): its
  `dev9p_read` serves from `larder_page_serve` (gated on the fresh `cvers`), and
  its `dev9p_close` is a **no-op** (no Tlopen was done, no fid to Tclunk). **Zero
  further server RTs.**
- On any miss (attr not cached, a covered page not resident, `cvers` mismatch,
  or write mode) → **fall back** to the current path (bind-walk + Tlopen + reads
  that populate the caches). Cached-open is a *fast path*, never a requirement.

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

**Invariants: refines I-38, no new invariant.** The fidless open is close-to-open
because the query-getattr at open IS the `Open` revalidation (fs_cache.tla); reads
serve only while `cvers` matches; an own-write invalidates (L1e); the fallback is
byte-identical to today. The served open+read+close is exactly what a bind-walk +
lopen + reads + clunk would return, minus the fid RTs. **Spec-first:** the
cached-open path maps onto the EXISTING `fs_cache.tla` `Open`+`Read`+`OwnWrite`
actions (the query-getattr = `Open`; the page serve = `Read`; no new action) — the
model already proves it; a prose SPEC-TO-CODE mapping suffices, or a focused
`cached_open` cfg if the audit surfaces subtlety. Prosecute in the focused audit:
the fidless-Spoor lifetime (no server fid to leak; close is a true no-op); the
"fully page-cached" detection soundness (a partial-cache open MUST fall back, not
serve a hole); the `cvers` revalidation completeness (every cached-open path
gated); the read-only gate (a write mode never takes the fidless path); the
fallback byte-identity.

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

## 6. Cross-links

[[project-warm-floor-decomposition]] (the measurement) ·
[[project-go-build-clean-perf]] (the mission register) ·
[[project-fs-cache-design]] + `docs/LARDER-DESIGN.md` (the Larder this extends) ·
`docs/POUNCE-DESIGN.md` (the Twalkgetattr the revalidation reuses) ·
ARCH §28 I-38 / I-10 / I-11 · `specs/fs_cache.tla` + `specs/9p_client.tla`.
