# LARDER — the guest-side FS cache (the 9P metadata + data round-trip fix)

Status: **L1 ARC COMPLETE (L1a → L1f)** — the Stratum content-version (`si_cvers`)
foundation + the coherence spec + the full guest Larder (attr + dentry + page +
the cacheability gate) are landed, audited (0 P0 / 1 P1-fixed / 2 P3-seams), SMP-
gated (40/40), and benched. The measured top lever of the on-device `go build`
mission (the FS-perf deep dive, 2026-07-09).
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
serve/populate/invalidate hooks; §3/§9, `docs/reference/132-larder.md`), **L1d**
(the dentry sub-cache incl. negative — `larder_walk_serve` / `larder_dentry_*` +
the `dev9p_walk_attrs` serve/populate + create/rename/unlink invalidate; coherence
is **own-write invalidation, NOT a cvers gate** — the ground-truth correction, §4),
**L1e** (the page sub-cache — `larder_page_*` + `larder_destroy` + the `dev9p_read`
serve/populate + `dev9p_write` page invalidate, keyed `(qid.path, page_index)`,
cvers-gated + own-write-invalidated; **the load-bearing cacheability gate** — a
per-`p9_client` `cacheable` flag, proven by a successful `Twalkgetattr` (POUNCE),
that engages the whole Larder ONLY for a content-versioned FS so a stream server
[netd `/net`] is never cached — §3.3/§7; also closes the latent L1c netd-attr gap),
**L1f** (the arc close: the focused Opus-4.8-max prosecutor + a concurrent
self-audit — **0 P0 / 1 P1 / 0 P2 / 2 P3**, NOT dirty — F1 [P1] the reused-ino
page-invalidate-on-create gap FIXED [the page twin of the attr defense; regression
`create_invalidates_reused_child_pages`, non-vacuous], F2/F3 documented as v1.x
seams §11; the **full SMP gate** default+UBSan × smp4/smp8 N=10 = 40/40 PASS 0
corruption; the gofmt re-measure warm 1352→1147 ms [−15%] / cold 2506→2474 ms).
**The L1 arc is COMPLETE.**

**The gofmt re-measure — an honest, oracle-driven result (§10).** The Larder
captured its full addressable FS-redundancy band (−15% warm) but fell short of the
predicted warm ~3× host, and the bench GROUND-TRUTHED why: a *trivial* warm hello
build is already 987 ms, so the 91-package `gofmt-warm` (1147 ms) adds only ~160 ms
over that floor — the warm build is ~86% FIXED go-tool overhead (REVENANT exec/
page-in of the ~12 MB toolchain + build-graph + link), only ~14% package-specific
FS work. The §10 prediction over-attributed the warm cost to eliminable FS ops; the
warm bottleneck is exec/build-graph, a DIFFERENT surface (the next lever). The
Larder's engagement is proven independently (the page-hit + serve-and-gate unit
tests, the 40/40 SMP gate, and the −15% delta itself — a 0%-hit cache gives 0
delta), so this is a recalibration of the expected payoff, not a broken cache.

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

2. **Dentry cache** — `(parent-qid.path, name) → { child-qid.path, type }`,
   **including negative entries** (`… → ENOENT`, the LWN #1 mechanism). A cached
   resolution skips the `Twalkgetattr` RPC and returns the child qid directly
   (the child's attr for the walk reply is served from the attr sub-cache — see
   the serve mechanics in §4). Negative dentries kill the failed-lookup storm
   (import/search-path probes) without a byte crossing the wire. **Coherence =
   own-write invalidation** (§4): a create/rename/unlink in a directory drops
   that directory's cached dentries. A build's walks share enormous prefix (11
   distinct first-hops warm), so the dentry cache serves far more than the
   run-memo floor.
   **Populate source (load-bearing coherence rule):** a dentry entry is installed
   ONLY from a `walk_attrs`/`getattr` reply. The Larder is **never** populated
   from a `readdir` qid — Rreaddir's `qid.version` is a link-time `si_gen`
   snapshot stored in the dirent record, not `si_cvers`, so a readdir-sourced
   version would read backwards against a getattr-sourced one for the same inode
   (the L1a-2 audit F1). v1.0 does not cache directory listings at all (§11);
   this rule is the guest-side realization of that.
   **The dentry binding carries NO content-version, and it is NOT cvers-gated
   (the L1d ground-truth correction — see §4).** Unlike an attr or a page (each
   validated by its OWN file's `cvers`), a name→child binding is a fact about
   the PARENT directory's dirent set, and Stratum does not surface a
   directory-content version that tracks dirent changes: verified in the Stratum
   tree (`src/fs/fs.c`), a child **create** and **unlink** touch only the
   separate dirent index and do **not** run `stm_inode_set` on the parent inode,
   so the parent's `si_cvers` does **not** bump on a create/unlink (only
   **rename** stamps the parent mtime → bumps it). The parent's content-version
   is therefore an unreliable signal for a dirent change, so the dentry cache's
   sole coherence mechanism is own-write invalidation (never a parent-cvers
   compare).

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

### 3.3 The cacheability gate (the content-versioned-server requirement)

The Larder's soundness rests on a contract the *server* must honor: **reads are
offset-stable (the same offset returns the same bytes until the content-version
changes), and there is a real content-version** (`cvers`) to key freshness.
Stratum (the FS) honors it. A **stream / control server does not** — netd's
`/net/<proto>/N/data` reads are *consuming* (the same offset returns different
bytes as the stream advances) and carry no content-version (`qid.version` is
always 0), so page-caching a `/net` read would serve stale stream bytes on any
offset re-read (an `lseek`-back-and-reread, a `pread(0)`), and attr-caching a
`/net` file would serve a stat the network mutated out of band (own-write
invalidation cannot cover an *external* writer). The guest cannot *detect* this
by observation, so it must be **declared per-mount** — the Plan 9 idiom, where
whether to interpose `cfs` is a per-mount choice.

Mechanism: a per-`p9_client` **`cacheable`** flag, **default false**, latched
true by the guest's first **successful `Twalkgetattr` (POUNCE)** reply — the v1.0
proxy for "a content-versioned FS." Stratum speaks POUNCE, so its first
resolution latches `cacheable`; netd answers `Twalkgetattr` with Rlerror ENOSYS,
so it never latches, and its attr + page caches never engage (`dev9p_stat_native`
and `dev9p_read` gate serve *and* populate on the flag; the dentry `walk_serve` is
additionally guarded by the pre-existing `wga_unsupported` latch). The flag is
**fail-safe**: an unproven mount is never cached (a perf loss for a hypothetical
POUNCE-less FS, never a stale read), and it is settled **before any read** (a file
is resolved via `walk_attrs` before it is read). The proxy is not identical to the
true property (a future server that speaks POUNCE but streams, or a
content-versioned FS without POUNCE, would need an explicit attach-time capability
— a clean v1.x add), but it is sound for the v1.0 server set (Stratum, netd) and
fail-safe in the direction that matters. This is the only non-Stratum dev9p client
at v1.0 — corvus is a byte-mode SrvConn (not dev9p), so it has no Larder; `/proc`
/ `/ctl` / `/env` / `/dev` are native kernel Devs (not dev9p), so they bypass the
Larder entirely.

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
- **Dentries (positive AND negative) are coherent by own-write invalidation,
  NOT by a cvers gate** (the L1d ground-truth correction). A name→child binding
  (or a negative "no such name") is a fact about the PARENT directory's dirent
  set. Ground truth (verified in Stratum `src/fs/fs.c`): a child **create** and
  **unlink** touch only the separate dirent index — the parent inode is never
  `stm_inode_set`, so the parent's `si_cvers` does **not** bump (only **rename**
  stamps the parents' mtime → bumps them). So the parent's content-version does
  not track a create/unlink, and a cvers compare against it would falsely match
  a stale negative dentry after a create (serve `ENOENT` for a now-existing
  file). The dentry cache therefore drops a directory's cached dentries on every
  guest **create / rename / unlink** in that directory (the guest holds the
  parent's `qid.path` at each mutation site — the same L1c own-write discipline,
  now scanning by parent). Under single-writer this is **absolute** (own-writes
  drop the parent's dentries → a stale name→child or negative entry can never be
  served); it maps onto `fs_cache.tla`'s `Read` + `OwnWrite` (the single-writer
  subset, `NoWrongRead` absolute), exactly as the attr sub-cache does — there is
  no `Open` (cvers) gate for dentries. An out-of-band (external) writer's dirent
  change is bounded by LRU eviction / the next own-write, not by a
  cvers-revalidation window (§11).

**Serve mechanics (skip the `Twalkgetattr`).** A resolution serves from the
dentry cache only when it can construct the whole `walk_attrs` reply locally: for
a run of components from a base parent `c`, walk the dentry chain
`(c.qid.path, name₀) → child₀`, `(child₀, name₁) → child₁`, …; for each POSITIVE
hop the child's attr for the reply comes from the attr sub-cache (a miss there
bails the serve to the RPC), and the next hop's parent is the resolved child. A
NEGATIVE hop is the walk's miss (return the partial-walk verdict, no RPC). A FULL
positive run can skip the RPC only in the **query** form (`SYS_STAT`'s final run,
`nc == NULL` — no server fid to bind); a **bind**-form full walk still issues the
RPC (the server must bind the fid), though it re-populates from the reply. The
chained lookup runs under one Larder-lock hold (an atomic snapshot — a concurrent
own-write invalidate either precedes or follows the whole serve). Because the
dentry cache holds the underlying dev9p tree's raw bindings (Territory-independent
— `walk_attrs` walks the 9P server, not the per-Proc mount overlay), the resolver
applies the per-Proc mount view to the served qids exactly as it does to an RPC
reply; the serve needs no mount knowledge.

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
- **L1d — the dentry cache** (incl. negative). **LANDED.** `larder_walk_serve`
  (serve a whole run from the dentry+attr caches, under one lock hold) +
  `larder_dentry_install` (positive per walked component + negative on a miss,
  gen-guarded) + `larder_dentry_invalidate_name`; hooked at `dev9p_walk_attrs`
  (serve before the RPC — full positive in the query form + any-form negative;
  populate after) and `dev9p_create` / `rename` / `unlink` (invalidate the mutated
  `(parent, name)` binding ONLY — siblings preserved, the whole-parent drop that
  forced sibling re-walks retired; O(1) via the serve's hash). **Coherence is
  own-write invalidation, NOT a cvers gate** — the L1d ground-truth correction
  (§4): Stratum's parent `si_cvers` does not bump on a child create/unlink, so a
  cvers gate would falsely match a stale negative; the dentry cache is
  `fs_cache.tla`'s `Read`+`OwnWrite` subset (the attr discipline keyed by
  `(parent,name)` — per-token, so name-specific invalidation is the faithful
  realization). Tests: `larder.dentry_*` (10, incl. the `dentry_invalidate_name`
  ground-truth core — drops the named binding, preserves a sibling) + `dev9p.create_invalidates_negative_dentry`
  (non-vacuous hook regression); `dev9p.walk_attrs` resets the cache between its
  wire sub-tests. 1066/1066 + boot OK + stalk-2 E2E.
- **L1e — the page cache. LANDED.** `larder_page_{serve,install,invalidate}` +
  `larder_destroy` on `struct larder` (a 512-slot LRU table keyed
  `(qid.path, page_index)`; each slot's 4 KiB buffer is HEAP — lazily kmalloc'd,
  reused across evictions, freed at `larder_destroy` from `p9_client_destroy`),
  hooked at `dev9p_read` (serve the one page containing the offset, fresh + in
  range, RPC-free — a single-page copy under the leaf lock, a short serve the
  caller loops on; populate each page the read covered *from its aligned start*,
  so there is never a hole) and `dev9p_write` (own-write page invalidate). The
  cvers freshness gate (Read/Open) + own-write invalidate (OwnWrite) compose, and
  a partial (small-file / EOF) page serves only within `[0, valid_len)` and misses
  beyond, so no EOF determination is needed (§4). **The load-bearing cacheability
  gate (§3.3): the whole Larder engages ONLY for a `cacheable` client** — a
  per-`p9_client` flag latched true by a successful `Twalkgetattr` (the v1.0 proxy
  for a content-versioned, offset-stable FS). A stream/control server (netd `/net`
  — consuming reads, `qid.version` always 0) answers `Twalkgetattr` ENOSYS, never
  latches `cacheable`, and is never cached (a re-read of an offset would serve
  stale stream bytes). Fail-safe (default false; a file is resolved via
  `walk_attrs` before it is read, so the gate is settled first). This also closes
  the latent L1c gap — attr caching had no server gate, so netd attrs (mutated out
  of band by the network) could serve stale. Maps to `fs_cache.tla`'s
  `Read`+`Open`+`OwnWrite` on content tokens keyed `(qid.path, page_index)` — **no
  spec extension** (a page is a content token like an attr). Tests: `larder.page_*`
  (10 — serve / miss / offset / cvers-mismatch / partial / invalidate / gen-guard /
  overwrite / bounded / destroy-frees) + `dev9p.page_cache_serve_and_gate` (a
  non-vacuous integration proof: a cacheable client's re-read is served with NO
  second Tread, a non-cacheable client's re-read RPCs, and an own-write
  invalidates — proven to fail with the serve disabled). 1077/1077 + boot OK.
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

- **Writeback caching.** ~~v1.0 is write-through (own-writes invalidate + go to
  the server immediately). Open-to-close writeback (v9fs `writeback`) is a v1.x
  knob.~~ **PULLED FORWARD as §12 (F1, Senate-voted 2026-07-11)** — the CHASE
  C-2 decomposition measured 97.9% of the S3 go-build's Twrites at ≤ 4 KiB
  (19.6k ops / 85 MB — the op-count band ≈ 1.36 s); write-behind on the LOOSE
  mount is the removing lever. Strict mounts keep write-through verbatim.
- **Persistent (on-disk) cache.** The Larder is in-memory per-session; the
  fscache/`cfs`-on-disk analog (a cache that survives a session) is v1.x.
- **Cross-session / cross-mount sharing.** Each `p9_client` has its own Larder;
  a shared server-cache tier (Plan 9's primary mitigation — Stratum's own dcache
  is that tier) is Stratum's job, not the guest Larder's.
- **General multi-writer coherence.** Close-to-open is single-writer-sound (the
  serve-your-own-FS-to-one-guest case). A concurrent *external* writer (out-of-
  band Stratum mutation) is bounded by the revalidation window, not instantly
  coherent — acceptable at v1.0, tightenable via the writeback modes.
- **The Loom async path bypasses the Larder (L1c/L1d seam).** The Larder is
  populated + invalidated ONLY on the SYNCHRONOUS dev9p path (`dev9p_stat_native`
  / `dev9p_walk_attrs` populate; `dev9p_write` / `dev9p_wstat_native` / create /
  rename / unlink invalidate). The Loom async engine (`kernel/loom.c` —
  `LOOM_OP_WRITE` / `MKNOD` / `SYMLINK` / `LINK` / `UNLINKAT` / `RENAMEAT`) drives
  `p9_client_*` directly and touches neither sub-cache, so a client that mixes
  Loom FS *mutations* with synchronous *resolution* on the SAME mount is, from
  the Larder's view, a second (out-of-band) writer: a Loom write can leave a
  stale attr, and a Loom symlink/mknod/link/unlinkat/rename can leave a stale
  dentry (a negative entry a Loom create should have filled, or a positive a Loom
  unlink should have dropped), bounded by the next own-*sync* mutation of that
  file/dir or by LRU eviction. **Not driven by any v1.0 consumer** — no v1.0
  consumer mixes them (the go-build is pure synchronous pouch/musl; the Loom
  consumers do network / FSYNC / NOP, not FS-dirent mutations on the shared
  Stratum client). *Self-inflicted-reachable, not unreachable* (L1f audit F2, the
  wording corrected from "unreachable"): a crafted EL0 program COULD open a
  cacheable Stratum file, register it into a Loom ring, and submit a
  `LOOM_OP_WRITE`/dirent-mutation, bypassing the invalidate — but the stale data
  is the file's OWN prior content under its OWN `qid.path` (the page/attr key), so
  it is a self-inflicted stale view of one's own file, with **no** cross-file /
  cross-Proc / privilege leak (P3). The v1.x fix invalidates the Larder from the
  Loom completion path (the completion carries the op's fid → the affected
  dir/file qid.path → the same `larder_*_invalidate` calls), or fail-closed
  rejects a Loom mutation op submitted against a `cacheable` dev9p fid. Tracked;
  not fixed at v1.0 (no consumer makes it load-bearing, and the completion-side
  qid.path plumbing is non-trivial).
- **rename/unlink leave the moved/unlinked file's OWN attr — stale `ctime`/`nlink`
  (L1f audit F3 / self-audit SA-1).** `dev9p_rename` invalidates both directories'
  attrs + dentries but not the *moved file's* own attr (a rename bumps the file's
  `ctime`); `dev9p_unlink` invalidates the parent but deliberately leaves the child
  attr (`dev9p.c` comment), so an unlink-while-open leaves a stale `nlink`. A
  held-open fid `fstat`'d after the guest's own unlink/rename (keyed by the file's
  unchanged `qid.path`) therefore serves stale `ctime`/`nlink`. **Metadata-only:**
  `mode`/`uid`/`gid` are UNCHANGED by rename/unlink and `perm_check` reads only
  those, so there is NO privilege / X-search / I-28 consequence; the content is
  untouched; `nlink` staleness is largely moot at v1.0 (no sync `link`/`symlink`/
  `mknod` path — hardlinks are Loom-only, the seam above). Bounded by the next
  own-write or LRU eviction. The fix needs the moved/unlinked file's `qid.path`
  (not available at the rename/unlink site, which takes parent + name), so it is a
  v1.x refinement, not a v1.0 code fix. (The *reused-ino* twin — a create at a
  freed ino serving a prior occupant's stale attr AND now pages — IS fixed at v1.0
  via the child-invalidate in `dev9p_create`; see §4 / L1f audit F1.)
- **Dentry external-writer window (L1d).** Unlike attr/page entries (each
  revalidated at open by its own file's `cvers`), a **dentry** has no
  content-version to revalidate against: Stratum surfaces no directory-content
  version that tracks dirent changes (a create/unlink does not bump the parent
  `si_cvers` — §4 ground truth). So an out-of-band create/unlink in a directory
  is not caught by a cvers revalidation; a stale dentry (a name→child that a
  peer renamed, or a negative entry a peer filled) is bounded only by LRU
  eviction or the guest's own next mutation of that directory. Under
  single-writer (the v1.0 model) this is moot — own-writes invalidate. The v1.x
  tightening is a true directory-content-version in Stratum (stamp the parent
  inode on create/unlink, at a parent-COW cost on the create/unlink-heavy build
  path — deliberately NOT paid at v1.0), which would then let a `walk_attrs`
  revalidate a parent's dentries by its `cvers`, exactly as attr/page do today.
- **Page-cache v1.x refinements (all perf/memory, none correctness).** (a)
  *Over-invalidation on write*: `dev9p_write` drops **every** cached page of the
  file (`larder_page_invalidate` by `qid.path`), not just the written range —
  sound (never stale) but re-reads unchanged pages; a precise per-range invalidate
  is a v1.x tuning (writes in a build are mostly whole-file, so the cost is
  negligible). (b) *Single-page serve*: `dev9p_read` serves at most the one page
  containing the offset (a bounded ≤ 4 KiB copy under the leaf lock; the caller
  loops on the short read), so a fully-cached 128 KiB read is 32 fast serves;
  multi-page serve (with the pin-and-copy-outside-lock below) is a v1.x
  throughput refinement. (c) *Copy-out under the lock*: v1.0 copies served bytes
  under the Larder lock (so an evict/invalidate cannot free a page mid-copy); the
  `#847`-style pin-page-then-copy-outside-lock is the v1.x SMP-scaling refinement
  (§7). (d) *Partial-front page not cached*: an **unaligned** read's first page
  (its aligned start precedes the read offset) is not populated, to keep every
  cached page hole-free `[0, valid_len)`; Go's reads are page-aligned, so this
  rarely bites. (e) *Inline page metadata on non-cacheable clients*: every
  `p9_client` carries the 512-slot page-metadata array inline (~24 KiB), even
  netd, which never allocates a page buffer (only a cacheable client does); a
  lazy-heap page table (allocated only when `cacheable` latches) is a v1.x memory
  refinement.
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

## 12. Write-behind (F1 — the loose writeback leg; Senate-voted 2026-07-11)

The CHASE C-2 decomposition (LEDGER "C-2 write-band decomposition") measured
the S3 write band at 19.6k wire Twrites / 1795 ms for 85 MB — **97.9% ≤ 4 KiB**
(the Go toolchain writes through ~4 KiB `bufio`; every object lands twice,
$WORK + its GOCACHE copy). W-A transport+queueing (~933 ms) and W-B server
per-op fixed cost (~430 ms) scale linearly with op count; the removing lever is
**fewer, larger writes**. F2 (the Stratum transition-into-buffer) killed the
per-file sync-extent tax server-side; F1 is the guest half.

### 12.1 The model

The v9fs `cache=loose` **writeback** leg / NFS-async precedent, completing the
B1-voted loose premise (§ the I-38 row): on a **loose + cacheable** client,
small sequential writes STAGE guest-side and flush as msize-max Twrites. Plan 9
heritage is write-through (`cfs` is read-only caching) — but Plan 9 had no
build-farm-on-9P story; the capability-OS SOTA (v9fs writeback / NFS async /
Fuchsia minfs writeback) all buffer client-side under close-to-open.

### 12.2 The mechanism (deliberately minimal)

- **Staging home: per-open-file** (`dev9p_priv.wb`), NOT dirty bits in the
  shared L1e page cache. A single **contiguous append run**
  `{stage_off, stage_len, buf, cap, base_size, err}` under a per-priv spinlock:
  - A write STAGES iff it is a **pure append** to the run's logical end
    (`off == base_size` when empty, `off == stage_off + stage_len` when
    active) — 97.9% of the measured mix. Anything else (an interior pwrite —
    e.g. the Go buildid header patch — a hole, a non-file, an error-latched
    priv) **flushes the run then writes through**: fail-safe, zero new
    semantics for the odd cases. `base_size` is the **append anchor** and is
    KNOWN only for a priv born at a create / OTRUNC-open (size 0), advancing
    with completed flushes + completed write-throughs (`max`); an
    opened-existing file has no known end and never stages — the measured
    mix is entirely create-then-write. The anchor discipline is what makes
    the below-run read arm complete (the server holds every byte below the
    run — no hole a read would need to zero-fill). Writes larger than
    `DEV9P_WB_STAGE_MAX` (32 KiB) also flush-then-write-through: they are
    already wire-efficient, and the bound caps the staging copy under the
    priv spinlock.
  - The buffer grows by doubling (kmalloc) up to `DEV9P_WB_CAP` (256 KiB — two
    msize payloads); reaching the cap flushes the whole run and restarts. So
    steady-state staging turns the 4-KiB dribble into full-msize wire writes.
  - Staged buffers are bounded by a **GLOBAL** outstanding-bytes budget
    (`DEV9P_WB_BUDGET`, 8 MiB — the `DEV9P_CO_BUDGET` cached-open shape),
    NOT a per-Proc I-32 charge: the staging home is the per-open-file priv,
    which crosses Proc boundaries (handle_dup, rfork inheritance, the #926
    close-at-exit runs in whichever holder dies last), so a per-Proc charge
    has no sound uncharge site — exactly the reasoning already recorded on
    `DEV9P_CO_BUDGET`. Budget denial → write-through fallback (graceful;
    combined with the per-priv cap the kernel-heap exposure is bounded, and
    the degrade is the strict-mount behavior).
- **Flush sites** (all funnel through one `wb_flush`):
  1. **close** (`dev9p_close`) — BEFORE the async-clunk Tclunk (the
     fid must be live for the flush Twrites). This is the close-to-open
     anchor: the next open (any Proc) reads the flushed bytes.
  2. **fsync** — flush, then `p9_client_fsync`; errors return synchronously.
  3. **cap/threshold** — the mid-stream flushes above.
  4. **a non-append write / wstat / weft-bind** on the same priv — flush
     first, then the op (ordering: the staged bytes are older).
  5. **a read of the same priv** needs no flush: the run is contiguous at
     the file's known end, so reads split cleanly — below `stage_off` = old
     content (server/cache, complete: the append-anchor discipline means the
     server holds every byte below the run), within the run = served from
     the staged buffer (overlay, the guest's newest bytes; a short read up
     to the run's end — POSIX short-read composition carries a spanning
     caller across the boundary), at/past the run's end = fall through to
     the normal path (which answers EOF honestly from the server/attr —
     never a synthesized 0, so a racing write-through extension past the run
     stays visible).
- **fstat on the staging fd** patches `size = max(server_size,
  stage_off + stage_len)` (the Go truncate-gate/buildid pattern needs the
  post-write size mid-open). Path-stats via OTHER fids see the last-flushed
  state — legal under close-to-open (the file is open-dirty).
- **Shared-cache coherence**: the attr + page invalidates move from per-WRITE
  to per-FLUSH (the bytes reach the server exactly at flush; 19.6k invalidate
  pairs become ~1.4k). Same-priv readers never see stale (the overlay wins);
  other-priv readers are close-to-open-legal until the close flush.
- **Error semantics (the voted NFS model, honestly bounded by the ABI)**: a
  flush failure (ENOSPC/EIO) LATCHES on the priv; every subsequent
  write/fsync on that fd returns the latched errno (so a streaming writer
  aborts at the next op); **fsync is the reliable error channel**. A failed
  flush DROPS the staged run (the NFS-async posture: the bytes are lost, the
  latch reports it — retry-forever would wedge close). The
  `Dev.close` slot is `void` at v1.0, so a close-flush failure cannot reach
  the caller's close() return — documented seam; v1.x grows the slot. The
  threshold flushes bound the silently-at-risk tail to < 256 KiB.
- **Unlink** of a closed staged file needs nothing (the flush happened at
  close). An unlink-while-open flushes at close into the orphaned fid
  (9P keeps the fid live until clunk) — harmless; the skip-flush-on-unlinked
  optimization needs unlink→priv plumbing and is v1.x.
- **SMP** (the §7 shared-client discipline): the run stays **VISIBLE** under
  the priv lock across its flush — a flusher count freezes it (no detach:
  the flusher snapshots `{off, len, buf}` under the lock, does the wire I/O
  OUTSIDE it — blocking 9P never under a spinlock — and re-locks to retire
  the run). Flushes are **SINGLE-FLIGHT**: a second flush-needing party
  (a concurrent fsync / non-append write / wstat on a dup-shared fd)
  yield-waits at the flush entry until the in-flight flush retires — the
  `on_cpu`-spin class of wait (bounded by the flusher's independent
  progress, incl. its #811 death-unwind; a yield-loop, not a Rendez, so
  there is no park/wake surface, no single-waiter hazard, no new I-9
  leg). Duplicate concurrent flushes are FORBIDDEN, not merely
  idempotent: a duplicate that completes lets an ordering-dependent
  through-write land while the FIRST flusher's remaining stale chunks
  are still in flight — an interior write overlapping the frozen run
  would then be silently overwritten by the stale re-send (the
  self-audit SA-F1 race). While a flush is in flight, a concurrent
  APPEND goes write-through (order-free: disjoint, past the frozen
  run's end) and a concurrent read overlays the still-visible run —
  neither waits; only flush-needing ops do. The frozen-run rule also
  pins the buffer (no growth reallocation while a flusher reads it).
  Death mid-stage: #926 closes handles at exit → the close flush runs
  (dev9p_close runs at the LAST Spoor ref, so it is uncontended — the
  cached-open/weft last-ref invariant; close never waits).

### 12.3 What it does NOT change

Strict mounts: byte-identical write-through. Non-cacheable clients (netd),
byte-mode (corvus), weft-bound fids, directories, cached-open (read-only)
privs: untouched. The 9P wire protocol: unchanged (the flush is ordinary
Twrites). The server: unchanged. I-28/perm: the staged write happens on an
already-opened fid whose rights were checked at open — staging defers only the
TRANSPORT, never a check.

### 12.4 The spec (model-first; the L1b re-enabled surface)

`specs/fs_cache.tla` gains the write-behind actions on the existing
content-token abstraction: `StageWrite` (the newest content lives in a
per-file `staged` slot; a `Read` MUST serve staged-over-cache-over-server) and
`FlushClose` (staged → server, `cvers` bump, staged cleared, cache
invalidated). Two new buggy cfgs pin the two bug classes:
`BUGGY_READ_SKIPS_STAGED` (a read serves cache/server while staged is present
— the overlay-miss class) and `BUGGY_LOST_STAGE` (close drops the staged token
without flushing — the lost-write class; the next open reads the pre-write
server content). The byte-granular overlay split (below/within/past the run)
is beneath the token abstraction — pinned by the kernel tests instead.

### 12.5 Projected effect

19.6k → ~1.4k wire writes (85 MB / 128-KiB flushes + the sub-cap tails at
close). W-A+W-B ≈ −0.7..0.9 s of the post-F2 936 ms write band, plus the
queueing ripple on the other bands (the pre-F2→F2 step showed wga/read
shrinking ~40% from depth relief alone). F2+F1 projects S3 ≈ 2.5–2.8 s vs the
2648 bar; the wga cold band is the reserve lever.

---

## 13. Write-populate (G1 -- term-4; the flush installs OWN pages)

The F1 write-behind's per-flush page invalidate (section 12) discarded the
very bytes the build is about to read back: the link reads every just-written
`_pkg_.a`, and cmd/go re-reads objects for cache puts and buildids. G1 makes
the flush INSTALL instead of drop, and scopes the write-through invalidate to
the bytes actually written.

**G1a (install at flush).** On `wb_flush_locked`'s full-land arm (err == 0,
every chunk's Rwrite confirmed), the frozen run's pages install into the page
cache marked `own`. An `own` page serves WITHOUT the cvers gate: no post-flush
cvers is knowable client-side (Rwrite carries none), and under the loose
single-writer premise -- which the wb's existence on the client already
asserts (fs_cache.tla: `EnableStaging => ~EnableExternalWriter`) -- the
flushed bytes ARE the file's current content. `pages_cover` accepts own pages
too, so a just-written file can take the fidless cached-open path. A normal
read-populate over the same key upgrades the page back to cvers-gated (the
read observed server content). The boundary page of an append chain EXTENDS
only when the existing page is OWN and ends exactly at the new run's start
(never a hole, never a mixed cvers/own page); the failed/partial-flush arm
keeps the fail-safe whole-file drop.

**No populate gen guard on the own install** -- the resurrection class the
guard closes is a READ-populate racing an invalidate (pre-mutation bytes
re-installed); an own install's bytes are the writer's just-landed content,
and no same-file mutation can land concurrently: the caller holds the wb
flush freeze (`wb_flushers > 0` excludes same-file stagers and orders
overlapping through-writes behind the flush -- the SA-F1 single-flight).

**G1b (range-scoped write-through invalidate).** The measured killer: the
buildid rewrite (a ~100-byte in-place pwrite cmd/go performs on every built
object and on the linked binary) took the write-through path, whose
WHOLE-FILE page invalidate nuked the archive's freshly-installed pages
moments after the close flush -- `b009/_pkg_.a` alone was wire-read 976
times / 22.7 MB in one S3 window. The write-through now drops only the pages
in `[offset, offset+accepted)`. Pages outside the range keep serving: a
cvers-gated page's staleness is caught by the normal open-time revalidation
exactly as if it had been dropped, and an OWN page's untouched bytes are
still the file's current content under the single-writer premise. OTRUNC and
create-reuse (L1f F1) keep their whole-file drops.

**Spec.** `fs_cache.tla` extended model-first: `EnableFlushPopulate` makes
`FlushClose` install the landed content as the current token (reusing
`Refetch` -- capacity-honest, validated), gated to the staging single-writer
world by ASSUME. The new buggy cfg `fs_cache_buggy_populate_unflushed`
(populate fires on the lost-flush arm) violates `CacheNeverAhead` -- the
executable counterexample pinning the impl's err==0 coupling. The range
scoping is below the model's per-file token abstraction (the L1e
partial-page precedent); its soundness argument is the single-writer own-page
property above, pinned by the `wb_writethrough_range` regression.

**Measured (instrumented A/B, S3 window).** Wire reads 6.7k -> 3.5k, read
bytes 112 MB -> 31 MB, server read handler 125 -> 39 ms, covered 969-1039 ->
726 ms, S3 wall ~3.1-3.3 s -> 2.9 s, S1 warm 233-244 -> 195 ms.

## 14. Perm-valid attr downgrade (G3) + the qid-scoped gen guard (G4) -- term-4

T4-M measured the S3 wga band as 5.6x RE-WALK (wire 6938 vs 1228 distinct
chains): the resolver keeps re-issuing Twalkgetattr for chains it had already
cached, because the cold build's own mutations keep knocking the caches out.
Two guest-side mechanisms close the two knock-out channels; neither changes
the wire, the server, or the spec model.

### G3: the perm-valid attr downgrade

A create/unlink/rename in dir D stales D's size/mtime/nlink/cvers -- but CANNOT
change D's mode/uid/gid (only a wstat on D itself does, and that path
full-invalidates). Pre-G3 the call sites dropped D's attr whole, so the next
walk THROUGH D missed its mid-hop attr and re-issued the RPC (measured
am_mid=1104 mid-hop attr misses per S3 window). G3 replaces the drop with
`larder_attr_downgrade`: the entry is marked `perm_only` and

- SERVES a resolver INTERMEDIATE hop (larder_walk_serve): the X-check reads
  mode/uid/gid + the immutable qid.path/type -- every served field is fresh by
  construction (the downgrade-triggering mutations cannot touch them).
- MISSES everything that reads the staled fields: `larder_attr_serve` (fstat /
  SYS_STAT), `larder_attr_fresh_size` (the EOF serve), and the FINAL hop of a
  full positive run (the leaf record feeds STALK_STAT's returned stat, the
  carried-attrs chain, and the cached-open size/cvers gates).
- UPGRADES back to full on any install (revalidate-by-overwrite).

To the gen guard a downgrade IS an invalidation (it logs the event exactly like
`larder_attr_invalidate`), so a concurrent FULL populate of D that observed
pre-mutation times is still skipped -- the resurrection close is unchanged; only
the perm-servable core survives the event.

Call sites switched (dev9p.c): create -> parent downgrade (the CHILD's
attr+pages stay FULL drops -- the reused-ino defense needs the perm bits gone
too); rename -> both dirs; unlink -> parent. wstat stays a full drop (it edits
exactly the perm bits). Write paths are untouched (files are never mid hops).

### G4: the qid-scoped gen guard

The populate guard was GLOBAL: any invalidate bumped `l->gen`, and any in-flight
populate whose pre-RPC snapshot no longer matched was discarded -- including
fills of files entirely unrelated to the event (measured 726-886 lost installs
per S3 window; the cold build's write churn runs concurrently with its read
populates on the shared client). G4 keeps `gen` as the event sequence and adds
a 128-slot ring log `inval_qid[gen % RING]` recording WHICH qid each event
staled. The install guard (`inval_hits_locked`) skips iff its own key appears
among the events in `(seq0, gen]`; a window wider than the ring loses evidence
and fail-safes to the pre-G4 global skip.

Soundness rests on the event-logging discipline: every mutation logs EVERY qid
whose cached state it stales -- create: parent (downgrade + dentry) + child
(attr + pages); unlink/rename: parent(s) via downgrade + dentry events;
write-through/flush/OTRUNC: the file via its attr + page invalidates; wstat:
the file. The dentry install guard keys on the PARENT qid (dentry events log
the parent); the page/attr installs key on the file. The
`larder_pages_snapshot` gen witness (the B1-audit F1 close) scopes to the
snapshot's own file for the same reason: the third-actor repopulate hole
requires an own-write to THAT file, and its invalidate logs that qid.

Ring-log correctness: for a window n <= RING, slot `s % RING` (s in the window)
was last written by seq s itself -- a later same-residue writer would exceed
gen -- so the scan sees exactly the in-window events.

### What this does NOT change

No wire op, no server change, no fs_cache.tla change: G3 is beneath the model's
abstraction (the model's attr is one content token; perm_only is a partial-
validity refinement whose soundness argument is the field-freshness prose
above), and G4 moves the impl CLOSER to the model's per-file semantics (the
global gen was an over-conservative realization of the modeled per-file atomic
Open). The 2 buggy cfgs + the 4 clean cfgs re-run green unchanged.

Tests: `larder.attr_downgrade_perm_only` (mid serves / leaf + stat + fresh_size
miss / install upgrades), `larder.downgrade_guards_raced_populate` (the
downgrade is an invalidation to the guard), `larder.gen_scope_qid` (unrelated
admits / own-key skips / ring overflow fail-safes),
`dev9p.create_downgrades_parent_attr` (the call-site switch, mid-hop chain
survives a create). All four revert-probed (3 probes, 3 distinct failures).

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
