# LARDER — the guest-side FS cache (the 9P metadata + data round-trip fix)

Status: **BUILDING** — the Stratum content-version (`si_cvers`) foundation and
the coherence spec are landed; the guest Larder impl is next. The measured top
lever of the on-device `go build` mission (the FS-perf deep dive, 2026-07-09).
Resolves the forks by user vote (2026-07-09): the coherence key is a true Stratum
**content-version** (`si_cvers`, surfaced as `qid.version`); the first arc lands
the **full** cache (attr + dentry + data); **spec-first is re-enabled** for the
coherence race. This document is binding; implementation lands against it
(scripture-before-code).

**Landed:** L1a-1 (`si_cvers` inode primitive + `inode.tla`, @stratum `f4fbdf4`),
L1a-2 (surface `si_cvers` as the 9P `qid.version`, decoupled from `si_gen`;
@stratum `3288c02`+`5763876`, audit-clean), **L1b** (`specs/fs_cache.tla` — the
close-to-open coherence model, TLC-green with a 5-cfg matrix; §8), **L1c** (the
Larder substrate + attr sub-cache — `kernel/larder.{c,h}` + the `dev9p.c`
serve/populate/invalidate hooks; §3/§9, `docs/reference/132-larder.md`). **Next:**
L1d (dentry), L1e (page), L1f (audit + SMP gate + gofmt re-measure). See §9.

**Naming (proposal — open to your preference).** A thylacine is a pursuit/ambush
predator; a **larder** is a predator's store of provisions it returns to instead
of re-hunting. That is exactly this cache: the guest keeps a larder of FS data +
metadata it has already fetched, so a repeated read/stat/walk is served locally
instead of re-hunted over 9P. The mechanism is the substance; the name is a
one-line rename if you prefer another.

---

## 1. The problem, measured

The on-device `go build` is FS-bound: `cmd/gofmt` (91 pkgs) costs **cold 2506 ms
device / 600 ms host (4.2x)** and **warm 1352 ms device / 110 ms host (12x —
the damning ratio)**. The lower levers are already built (the AEAD hardware
lever, the commit path, POUNCE's fused walk, CF-3's 128 KiB msize). The residual
is the round-trip structure itself.

**Ground truth (the FSPROBE instrument — a temporary `dev9p` per-op emitter, 2
deterministic boots; stripped after).** A single-threaded build runs at
**in-flight depth 1**: each logical FS op takes the client lock, submits one 9P
frame, blocks in the elected-reader recv, unlocks — a full serial round-trip per
op (client submit → the in-guest ring, every byte crossing `c2s`/`s2c` twice →
serial stratumd → return). The host serves the same logical ops from its unified
buffer cache in sub-microseconds, so host-warm's 110 ms is mostly Go's *own*
compute. And **56–90% of the device's ops are redundant** — the cache-hit
ceiling an infinite close-to-open guest cache would eliminate:

| class (the cache that serves it) | COLD | WARM within-build | WARM cross-build |
|---|---|---|---|
| **reads** (page cache) | **83.9%** | 14.0% | **77.6%** (measured overlap) |
| **stats** (attr cache) | **95.6%** | **99.5%** | 99.5% |
| **walks** (dentry cache, memo floor) | ≥69.2% | ≥56.4% | higher |

Op-mix: cold R41/S30/W29; warm R34/S33/W33 — metadata is 59–66% of ops.

**The mechanism, nailed.** Every absolute-path resolution re-walks from root and
re-stats root for the base X-check (`stalk.c:326`). Measured (warm window): root
(`qid 0x0`) is re-stat'd **4113 of 4248 stats = 96.8%**; every walk starts from
root through just **7 top-level dirs** (goroot 2633×, go-cache 1164×, …). A
*few-hundred-entry* attr+dentry cache captures nearly all of it; a **one-entry**
cache of root's attr alone kills ~1/3 of warm ops. POUNCE's walk-fusion is
confirmed working (~3.2 components/RPC), so the redundancy is at the *resolution*
level (re-resolving the same paths), not per-component chattiness.

**Verdict: the gap is the missing guest-side cache, not the protocol.** (See §2.)

---

## 2. Prior art (the fork research)

9P is a round-trip-per-op RPC protocol by design. Plan 9 accepted that and
*mitigated* it; every fast 9P client since has done the same. The research
(three independent threads: Plan 9 heritage, modern SOTA, codebase ground-truth)
converges hard:

- **Plan 9 `cfs`** — the client-side caching file server. Caches file **data**
  blocks (not dirs), **close-to-open** coherence validated by **`qid.version`**
  (the version bumps on every content modification; a cached block is trusted
  while the version is unchanged), write-through. The structural reason it can
  only do close-to-open: a 9P server never pushes invalidations, so the client
  learns of change only by re-opening and comparing `qid.version`. Plan 9's
  *primary* mitigation was additionally a big warm **server** cache ("9P has no
  explicit support for caching files on a client; the large memory of the
  central file server acts as a shared cache").
- **Linux v9fs** — the `cache=` mode is an additive bitmask; only `loose`/
  `fscache` cache **metadata** (attrs + dentries). `cache=none` (the slow
  default) round-trips every stat/lookup/open. **LWN 1060656**: when kernel devs
  optimized 9p *for build workloads*, **100% of the speedup was pure
  metadata/negative-lookup caching** (negative-dentry +23%, symlink +18%), and
  `cache=loose` 9p then *beat* virtiofs on a compile (1m26.6s vs 1m32.1s). No
  data-plane change. This is the load-bearing datum: the compile gap is
  **metadata round-trips**, and eliminating them is the whole win.
- **virtio-fs + DAX** — replaced 9p-over-virtio in QEMU, but **virtiofs-no-DAX
  *ties* 9p** (Red Hat's own table); the win is the SUBSTRATE (DAX = the host
  page cache mmap'd into the guest), not the FUSE-vs-9P wire. Swapping the
  protocol alone can *lose* to 9p. So 9P's data-path slowness was always the
  substrate (round-trips + copies), never the wire format.
- **The convergence.** The single highest-impact lever for a warm rebuild is a
  **guest-side attribute + name-lookup (incl. negative) + data cache with
  close-to-open / content-version coherence** — exactly `cfs` and `cache=loose`.

**Where Thylacine already sits (the harder levers, built).** POUNCE = the fused
multi-element walk (Plan 9's `MAXWELEM` batching); Loom = io_uring-inverted async
pipelining (the "hide the RTT you can't cache" lever); Weft = the cross-Proc
shared-page DAX-analog (the bulk-streaming lever, lowest for a compile). The
**unbuilt** piece is the client cache — the top lever. The Larder is that piece.

---

## 3. The design: the Larder — three sub-caches on the `p9_client`

**One owner, one key.** The Larder lives on `struct p9_client` (the per-session
9P client, shared by every Proc/thread resolving through that mount via the #841
elected reader). It is keyed by **`qid.path`** (= `dataset_id << 32 | ino`,
unique within a session — `server.c:349`). It is protected by a dedicated cache
lock (see §7); it composes with, but is distinct from, `c->lock`.

Three sub-caches share the owner + key:

1. **Attr cache** — `qid.path → { attr, cvers, valid }`. Serves
   `dev9p_stat_native` (SYS_STAT / fstat) and the stalk **base X-check**
   (`stalk.c:326`). The biggest, cheapest win: the base X-check re-stats a
   handful of dirs (root, /goroot, /go-cache…) thousands of times; a
   few-entry attr cache serves ~96–99.5% of stats. Populated *for free* by every
   `walk_attrs`/`getattr` reply (each carries the qid + attr).

2. **Dentry cache** — `(parent-qid.path, name) → { child-qid, type, cvers }`,
   **including negative entries** (`… → ENOENT`, the LWN #1 mechanism). A cached
   resolution skips the `Twalkgetattr` RPC and returns the child qid directly.
   Negative dentries kill the failed-lookup storm (import/search-path probes)
   without a byte crossing the wire. Invalidated on create/rename/unlink in the
   parent (see §4). A build's walks share enormous prefix (11 distinct first-hops
   warm), so the dentry cache serves far more than the run-memo floor.
   **Populate source (load-bearing coherence rule):** a dentry entry is installed
   ONLY from a `walk_attrs`/`getattr` reply, whose `qid.version` is the true
   content-version `si_cvers` (L1a-2). The Larder is **never** populated from a
   `readdir` qid — Rreaddir's `qid.version` is a link-time `si_gen` snapshot
   stored in the dirent record, not `si_cvers`, so a readdir-sourced version
   would read backwards against a getattr-sourced one for the same inode (the
   L1a-2 audit F1). v1.0 does not cache directory listings at all (§11); this
   rule is the guest-side realization of that.

3. **Page cache** — `(qid.path, page_index) → { bytes, cvers }`. Serves
   `dev9p_read`. The biggest cold win (83.9% redundant — cross-package
   export-data re-reads) and the persistent iterative-dev win (77.6% of warm
   reads overlap the cold build's working set). Bounded page pool (see §6),
   LRU-evicted. Invalidated on write to the file (own-write) + on a `cvers`
   change at open.

**The three hooks (per sub-cache):**
- **Serve** — at the top of `dev9p_stat_native` / `dev9p_walk_attrs` (and the
  per-component `dev9p_walk`) / `dev9p_read`: if a valid entry exists, return it
  and skip the RPC.
- **Populate** — after each `getattr` / `walkgetattr` / `read` reply: install the
  attr / dentry+qid / pages, tagged with the reply's `cvers`.
- **Invalidate** — at `dev9p_write` (the file's pages + bump its cached entry),
  and at create / rename / unlink (the parent's dentries), and on a `cvers`
  mismatch at open (drop + refetch).

---

## 4. The coherence protocol: content-version + close-to-open

The cache is **coherent to close-to-open**, keyed on a true **content-version**
(`cvers`, §5) — the Plan 9 `cfs` model, not a hand-rolled attr-tuple.

- **Serve while `cvers` matches.** Each cached entry carries the `cvers` it was
  fetched at. A read/stat served from the Larder is valid as long as the file's
  `cvers` is unchanged.
- **Revalidate at open.** On `open`/resolution (the natural close-to-open point),
  one `getattr` returns the current `cvers`; if it differs from the cached
  entry's, the entry is stale → dropped + refetched. Crucially, **POUNCE's
  `walk_attrs` already returns the qid (with `cvers`) at resolution**, so for the
  common resolve-then-open path the revalidation is *free* — the fused op carries
  the fresh `cvers` the Larder validates against.
- **Own-writes invalidate immediately** (write-through + invalidate): a
  `dev9p_write` to a file drops that file's cached pages and marks its attr
  entry for refetch. The client sees its own writes with strong consistency.
- **Negative dentries** are validated by the **parent's** `cvers`: a create in a
  dir bumps the dir's `cvers`, so a cached "`name` does not exist" is invalidated
  the moment the dir changes.

**Why close-to-open is sound here.** Thylacine serves its own FS to the guest;
within a build the 95–99% redundant ops are read-only shared data (goroot
sources, `.a` export data) that never change mid-build, and the write-then-read
case (compile writes `_pkg_.a`, link reads it) is caught by the `cvers` bump at
the link's open. The one residual is a **tightened directory permission** during
a live build: a cached dir attr could let the base X-check pass a search that a
now-tightened perm should deny, for a window bounded by the close-to-open
revalidation. This is the same unsynchronized-snapshot TOCTOU the resolver
already accepts (`stalk.c` §6), now with a bounded staleness window; it is pinned
in the spec (§8) and the invariant (§6).

Stronger modes (open-to-close writeback, à la v9fs `readahead`/`writeback`) are a
**v1.x** knob, not v1.0.

---

## 5. The Stratum side: `si_cvers` (the content-version)

The coherence key requires a true content-version in `qid.version`. **Stratum's
current `qid.version` is `si_gen`, which bumps only on inode free+reuse, not on
content write** (the R94 P3-1 reflink caveat states it: "a v9fs client that keys
cache invalidation on `qid.version` may serve stale bytes"). So Stratum grows a
distinct content-version:

- **`le32 si_cvers`, carved from `si_reserved[44]` → `si_reserved[40]`.** The
  inode stays **256 bytes** (`STM_INODE_SIZE_BYTES`) → **no on-disk format
  break** (no `STM_UB_VERSION` bump). Old pools: `si_reserved` was zeroed, so
  `si_cvers` reads **0** — a valid starting content-version; the first write
  bumps it. Backward-compatible; the inode-write path (which today *zeroes* the
  reserved region) carves those 4 bytes out and **increments** instead of
  zeroing.
- **Bump on every content/metadata mutation** — `stm_inode_set` and the write /
  setattr / truncate / rename / unlink paths that change the file. Monotonic
  per-inode.
- **DECOUPLED from `si_gen`.** `si_gen` (offset 40) stays the inode-**lifecycle**
  generation — the fid-staleness `ESTALE` check compares `cached_gen` vs `si_gen`
  (`server.c:1004`); bumping `si_gen` on every write would `ESTALE` every
  outstanding fid. `si_cvers` is the **content** version, orthogonal.
- **Surface as `qid.version`** — `server.c:774` currently sets `out_gen = si_gen`
  → set it to `si_cvers`. Every `Rwalkgetattr`/`Rgetattr` qid then carries a true
  content version. (Fid-staleness internally still uses `si_gen`.)
- **Side benefit:** this also closes the reflink/FICLONE cache-coherence gap the
  R94 caveat flagged (a reflink now bumps `si_cvers`).

Stratum is in scope (user-authorized); this is a Stratum-side chunk (the
`thylacine-pouch-arm` branch) — no on-disk format break, but it needs the
`inode.tla` extension (§8) modeling "content mutation bumps `si_cvers`, `si_gen`
unchanged" — exactly the extension the R94 comment named as the prerequisite.

---

## 6. Security: invariant I-38 + composition

**I-38 (new, ARCH §28): Larder cache coherence.** A cache hit returns exactly
what a fresh RPC would under close-to-open: an entry is served only while its
`cvers` matches the file's current content-version (validated at open),
invalidated on own-write, and never served past its revalidation point. A stale
entry can never produce a wrong read, a wrong stat, a wrong resolution, or a
wrong permission decision. The Larder is a per-session resource bounded by a
fixed capacity (no unbounded growth).

**Composition (no new privilege surface):**
- **I-28 (path-resolution containment).** The base X-check still `perm_check`s —
  now against a memoized-but-`cvers`-validated attr, the same unsynchronized
  snapshot the resolver already accepts. A tightened dir perm has a *bounded*
  staleness window (the close-to-open revalidation); pinned in the spec.
- **I-32 (per-Proc/session resource floor).** The Larder is LRU-capped: a
  bounded attr+dentry entry count (the measured working set is ~313 dirs + ~1191
  files) + a bounded page pool (~72 MB working set → a fixed page budget). No
  unbounded kernel-memory growth from a hostile workload.
- **I-10 / I-11 (tag uniqueness / fid identity).** Untouched — the Larder sits
  *above* the fid/tag layer (it serves before, or populates after, an RPC; it
  never mutates the wire protocol).

The Larder is a new surface on the **most-audited FS path** (dev9p / the 9P
client / stalk), so it is an **audit-trigger surface** (ARCH §25.4 + CLAUDE.md):
a focused adversarial round prosecutes the coherence + SMP + lifetime + bound
before merge.

---

## 7. SMP safety (the shared-client hazard)

The `p9_client` is shared by every Proc/thread resolving through the mount, so
the Larder is reachable concurrently from peer threads of one Proc *and* from
distinct Procs (`rfork(RFNAMEG)` / the elected reader). This is the recurrent
multi-thread-Proc-shared-state hazard class (#844, #57a-F2, RW-2). Discipline:

- A **dedicated cache lock** (a spinlock; the entries are small, ops are short).
  A serve/populate/invalidate takes it, mutates, releases — no cross-op held
  state. A returned/aborted op leaves no dangling reference.
- **Lock order.** The cache lock is a near-leaf, taken WITHOUT `c->lock` held
  across a blocking op (the #360 lock-across-sleep rule); the serve fast-path
  (cache hit → skip the RPC) takes only the cache lock and never blocks. The
  exact order (cache lock vs `c->lock` vs the buddy allocator for page-pool
  alloc) is pinned in the impl + the audit.
- **Page-pool lifetime.** Cached pages are reference-safe: a page served to a
  reader is copied out under the lock (or pinned); eviction never frees a page a
  concurrent read is copying. (The #847 dual-refcount discipline is the model if
  pages are shared rather than copied — a v1.x optimization; v1.0 copies out.)
- The **stale-serve race** (invalidate vs a concurrent serve on the shared
  client) is the load-bearing correctness property — it is the reason spec-first
  is re-enabled (§8).

---

## 8. The spec-first plan (re-enabled for this surface)

Spec-first is re-enabled for the Larder (user-voted 2026-07-09) — the coherence
invalidation race is SMP-race-bearing, exactly the class the ASID / death-wake /
allowance re-enable precedents cover. The model comes BEFORE the impl:

- **`specs/fs_cache.tla`** (Thylacine side) — **LANDED (L1b), TLC-green.** Models
  the Larder + the `cvers` coherence: `Open` (the close-to-open revalidation +
  populate), `Read` (serve without re-check), `OwnWrite` (write-through
  invalidate), `ExternalWrite` (the out-of-band writer — the coherence hazard),
  `Evict` (the bound), interleaved on the shared client (TLC explores the
  invalidate-vs-serve race). The as-built cfg matrix:
  - `fs_cache.cfg` — single-writer, both bugs off: **NoWrongRead** is ABSOLUTE
    (own-writes invalidate → a valid entry is always fresh → a served value
    never lags the current content), plus **NoStalePastRevalidation**,
    **Bounded**, **CacheNeverAhead**. 72 distinct states, green.
  - `fs_cache_external.cfg` — the out-of-band writer exercises the open gate:
    **NoStalePastRevalidation** holds (the revalidation always catches the
    mismatch, bounding staleness to one episode). NoWrongRead is *not* checked
    here — an external write within an open episode is served stale until the
    next open (the accepted §4/§11 window). Green.
  - `fs_cache_liveness.cfg` — **WriteEventuallyVisible** (a fresh write is
    eventually served, via the revalidation), under WF(Open)+WF(Evict). Green,
    non-vacuous (the external writer makes staleness reachable).
  - `fs_cache_buggy_stale_serve.cfg` — the serve-without-`cvers`-check bug: `Open`
    keeps a stale entry validated → a minimal 4-state **NoStalePastRevalidation**
    counterexample.
  - `fs_cache_buggy_no_invalidate.cfg` — the own-write-not-invalidated bug: a read
    serves the guest's own stale write → a minimal 4-state **NoWrongRead**
    counterexample.

  The model pins two distinct disciplines with distinct bugs: the *open-time
  revalidation gate* (compare `cvers` before trusting an entry) AND the
  *own-write write-through invalidation*. Both are required; the two buggy cfgs
  show removing either reaches a wrong read.
- **`inode.tla` extension** (Stratum side): model "content mutation bumps
  `si_cvers`; `si_gen` unchanged" + a buggy cfg (bump `si_gen` on write →
  spurious `ESTALE` of a live fid). The companion the R94 caveat named.

Buggy-cfg counterexamples on the existing `9p_client.tla` re-run as pre-commit
gates (the Larder must not perturb tag/fid/ooo).

---

## 9. Implementation plan (sub-chunks; each lands green + tested)

One arc (the full attr+dentry+data cache, per the vote), sub-chunked so a
compaction at any boundary is recoverable. Stratum-first (the Larder validates
on the real `si_cvers`).

- **L1a — Stratum `si_cvers`.** Carve the field, bump-on-mutation, surface as
  `qid.version`, decouple from `si_gen`; the `inode.tla` extension + its buggy
  cfg; the host tests (a write bumps `si_cvers`, a fid survives a write, an old
  pool reads `si_cvers=0`). No on-disk format break; boot-OK on an old pool is
  the gate.
- **L1b — `specs/fs_cache.tla`.** The coherence model + the stale-serve buggy
  cfg, TLC-green, before the Thylacine impl.
- **L1c — the Larder substrate + attr cache. LANDED.** The `struct larder` on
  `p9_client` (a dedicated near-leaf lock + a bounded `LARDER_ATTR_ENTRIES`=256
  LRU array, `qid.path`-keyed with an explicit `valid` bit so root's `qid.path`==0
  caches like any key); the attr serve (`larder_attr_serve` at
  `dev9p_stat_native` — the base X-check re-stat storm + fstat), the free populate
  (`larder_attr_install` at `dev9p_walk_attrs` per component + the stat miss), and
  the write-through invalidate at `dev9p_write`/`wstat`/`create`/`rename`/`unlink`.
  Two impl subtleties the L1c build surfaced: (1) the **populate GEN guard** — the
  spec's `Open` is an atomic read-and-install, but the impl reads via an RPC and
  installs later, so a monotonic `gen` (captured pre-RPC, re-checked at install)
  skips a populate that raced an invalidate, closing the populate-after-invalidate
  resurrection (§7); (2) **create invalidates the CHILD** as well as the parent —
  Stratum can reuse a freed ino, so a newly-created qid.path may carry a stale
  prior-occupant attr, and the create path never runs `walk_attrs` (no
  revalidate-by-overwrite), so an explicit child-invalidate is required (the
  `dev9p.create_invalidates_reused_child` regression; caught in-build by the
  `stalk-2` delete+recreate+create-in-it E2E). Tests: `larder.*` (serve / miss /
  invalidate / gen-guard / root-qid-0 / overwrite / eviction-bounded) +
  `dev9p.create_invalidates_reused_child`; 1055/1055 + boot OK.
- **L1d — the dentry cache** (incl. negative). Serve/populate at
  `dev9p_walk_attrs`/`walk`; invalidate at create/rename/unlink.
- **L1e — the page cache.** The bounded page pool, LRU, serve/populate at
  `dev9p_read`; invalidate at `dev9p_write`.
- **L1f — the focused audit + the SMP gate + the bench.** The adversarial
  coherence/SMP/lifetime/bound round; `tools/ci-smp-gate.sh`; the gofmt cold/warm
  re-measure vs the §10 predictions.

---

## 10. Expected effect (honest ranges, from the measured ceilings)

From §1's op-mix × cache-hit ceilings:
- **Warm: 12× → ~3× host** (a persistent cache removes ~82% of warm ops:
  stats 99.5%, dentry ~70%, reads 77.6% cross-build). The iterative-dev rebuild
  approaches native.
- **Cold: 4.2× → ~1.5–2.5× host** (a within-build cache removes ~83% of cold
  FS ops; the residual is compile CPU — near-native under HVF — plus compulsory
  first-reads).
- The **attr-cache-on-dirs sub-lever alone** (L1c) banks ~1/3 of warm ops for
  near-zero complexity — a large early win before the dentry/page caches land.

The Larder is the largest single lever of the whole go-build performance mission,
and it is the piece that carries the "9P can be host-competitive" thesis from
proven-in-principle to delivered.

---

## 11. What v1.0 does NOT do (the seams)

- **Writeback caching.** v1.0 is write-through (own-writes invalidate + go to the
  server immediately). Open-to-close writeback (v9fs `writeback`) is a v1.x knob.
- **Persistent (on-disk) cache.** The Larder is in-memory per-session; the
  fscache/`cfs`-on-disk analog (a cache that survives a session) is v1.x.
- **Cross-session / cross-mount sharing.** Each `p9_client` has its own Larder;
  a shared server-cache tier (Plan 9's primary mitigation — Stratum's own dcache
  is that tier) is Stratum's job, not the guest Larder's.
- **General multi-writer coherence.** Close-to-open is single-writer-sound (the
  serve-your-own-FS-to-one-guest case). A concurrent *external* writer (out-of-
  band Stratum mutation) is bounded by the revalidation window, not instantly
  coherent — acceptable at v1.0, tightenable via the writeback modes.
- **Readdir (directory-content) caching.** v1.0 caches attrs/dentries/pages; a
  directory-listing cache is a candidate v1.x addition. Consequently the L1a-2
  seam — Rreaddir's `qid.version` stays a link-time `si_gen` snapshot (the dirent
  record stores no content-version) rather than `si_cvers` — is **closed on the
  guest side, not the server side**: the Larder never populates from a readdir
  qid (§3.2), so a readdir version can never read backwards against a getattr
  `si_cvers`. This is the ground-truth-corrected disposition of the L1a-2 audit
  F1 (the audit's two suggested server-side fixes both fail on ground truth: the
  dirent stores a *link-time snapshot*, so carrying `si_cvers` in the dirent
  record is BOTH an on-disk format break AND semantically stale — the snapshot
  never tracks the child's later content writes — while a per-child stat in
  `h_readdir` is a perf regression on the go-build readdir path, which is 30%+ of
  the op mix). The correct v1.x readdir-listing cache therefore needs a per-child
  content-version *revalidation* mechanism (e.g. a batched getattr / POUNCE over
  the listed children at open), designed with the listing cache itself — not a
  dirent-format snapshot.

---

## References
- The measured justification: the FS-perf deep dive (the FSPROBE ceilings + the
  decomposed gap model + the proven thesis).
- Prior art: Plan 9 `cfs(4)` + `walk(5)`; Linux v9fs `cache=` (kernel docs +
  LWN 1060656); virtio-fs/DAX design; io_uring (Axboe).
- The built lower levers: `docs/POUNCE-DESIGN.md` (fusion), `docs/LOOM.md`
  (pipelining), `docs/NET-THROUGHPUT.md` (Weft/DAX).
- The specs: `specs/fs_cache.tla` (the guest Larder coherence model, L1b) +
  the Stratum `inode.tla` `si_cvers`/`ContentMutate` extension (L1a-1).
- ARCH §28 I-38 (the invariant) + §25.4 (the audit-trigger row).
