# 132 — The Larder (guest-side 9P FS cache)

**Status:** **L1c + L1d + L1e + L1f LANDED — the L1 arc is COMPLETE.** The
substrate + the **attr** sub-cache (L1c), the **dentry** sub-cache incl. negative
entries (L1d), and the **page** sub-cache (L1e) with the load-bearing
**cacheability gate** (a per-`p9_client` `cacheable` flag proven by a successful
`Twalkgetattr`) that engages the whole Larder ONLY for a content-versioned FS — a
stream/control server (netd `/net`) is never cached. **L1f** closed the arc: the
focused Opus-4.8-max prosecutor + a concurrent self-audit (**0 P0 / 1 P1 / 0 P2 /
2 P3**, NOT dirty) — F1 [P1] the reused-ino page-invalidate-on-create gap FIXED
(the page twin of the attr defense; regression `create_invalidates_reused_child_pages`,
non-vacuous), F2/F3 documented as v1.x seams (§11); the **full SMP gate** (default
+UBSan × smp4/smp8, N=10 = 40/40 PASS, 0 corruption); and the gofmt re-measure
(warm 1352→1147 ms, −15%; cold 2506→2474 ms). The re-measure GROUND-TRUTHED the
warm build as ~86% fixed go-tool overhead (a 987 ms trivial-hello floor), so the
Larder captured its full addressable FS-redundancy band but the warm bottleneck is
exec/page-in/build-graph, not FS redundancy — the next lever (a different surface).

The Larder is the guest-side cache of FS metadata + data the guest has already
fetched over 9P, so a repeated stat/walk/read is served locally instead of
re-hunted. It is the measured top lever of the on-device `go build` (56–90% of a
build's FS ops are redundant round trips). Design scripture: `docs/LARDER-DESIGN.md`;
invariant **I-38** (ARCH §28); coherence model `specs/fs_cache.tla` (L1b).

Files: `kernel/include/thylacine/larder.h`, `kernel/larder.c`, and the
serve/populate/invalidate hooks in `kernel/dev9p.c`; embedded in `struct p9_client`
(`kernel/include/thylacine/9p_client.h`), initialized in `kernel/9p_client.c::p9_client_init`.

## Purpose

The 9P protocol round-trips every metadata op; a single-threaded build runs at
in-flight depth 1 (one serial RPC per logical FS op). The base X-check
(`stalk.c`) re-stats the resolution root ~96.8% of warm stats, and fstat/stat
redundancy is 95.6–99.5%. The Larder serves those from a local, close-to-open
coherent cache — the Plan 9 `cfs` / v9fs `cache=loose` model — keyed on a true
content-version so a hit returns exactly what a fresh RPC would.

## Public API (`kernel/include/thylacine/larder.h`)

```c
void larder_init(struct larder *l);

// Serve (Read). HIT: copy the cached attr for qid_path into *out (under the lock)
// + return true. MISS: set *seq0_out to the current gen (the populate guard) +
// return false.
bool larder_attr_serve(struct larder *l, u64 qid_path,
                       struct t_stat *out, u64 *seq0_out);

// Capture the gen for a populate site that does not serve first (walk_attrs).
// Call BEFORE the RPC.
u64  larder_gen_snapshot(struct larder *l);

// Populate (Open/Refetch). Install {attr, cvers} for qid_path IFF l->gen == seq0
// (no invalidate raced the RPC). Overwrite existing / free slot / LRU-evict.
void larder_attr_install(struct larder *l, u64 seq0, u64 qid_path,
                         u32 cvers, const struct t_stat *attr);

// Invalidate (OwnWrite). Drop qid_path's entry (if present) + bump gen (always).
void larder_attr_invalidate(struct larder *l, u64 qid_path);

// --- L1d: the dentry sub-cache ---

// Serve a whole resolver run (Read) from the dentry + attr sub-caches WITHOUT an
// RPC, under ONE lock hold. Full positive run: true, *nresolved=nname, *is_miss=0,
// sts[0..nname) filled. Negative dentry at hop i: true, *nresolved=i, *is_miss=1,
// sts[0..i) filled. Dentry/attr miss or too-long name: false (caller RPCs).
bool larder_walk_serve(struct larder *l, u64 base_qid_path,
                       const char *const *names, const size_t *name_lens,
                       int nname, struct t_stat *sts,
                       int *nresolved, bool *is_miss);

// Populate a dentry (positive -> child_qid_path, or negative -> ENOENT). Gen-
// guarded. A name > LARDER_DENTRY_NAME_MAX is not cached (no-op).
void larder_dentry_install(struct larder *l, u64 seq0, u64 parent_qid_path,
                           const char *name, size_t name_len,
                           u64 child_qid_path, bool negative);

// Invalidate (OwnWrite). Drop ONLY the (parent_qid_path, name) binding -- the one
// dentry a single-name create/rename/unlink stales; siblings preserved -- + bump
// gen. The dentry cache's SOLE coherence mechanism (no cvers gate); O(1) via the
// serve's (parent,name) hash.
void larder_dentry_invalidate_name(struct larder *l, u64 parent_qid_path,
                                   const char *name, size_t name_len);

// --- L1e: the page sub-cache ---

// Serve (Read). On a HIT for (qid_path, page_index) with cvers == want_cvers and
// page_off < valid_len, copy min(want, valid_len - page_off) bytes into out UNDER
// the lock, return the count (> 0). Else set *seq0_out = gen and return 0. One
// page per call (bounded copy under the lock; caller loops the short serve).
u32 larder_page_serve(struct larder *l, u64 qid_path, u64 page_index,
                      u32 page_off, u32 want, u32 want_cvers,
                      u8 *out, u64 *seq0_out);

// Populate (Open/Refetch). Install (qid_path, page_index) with len bytes [0,len)
// from data (from the page's aligned start), tagged cvers, IFF gen == seq0. Lazily
// kmallocs the slot's 4 KiB buffer (a failure skips -- best-effort). len clamped.
void larder_page_install(struct larder *l, u64 seq0, u64 qid_path, u64 page_index,
                         u32 cvers, const u8 *data, u32 len);

// Invalidate (OwnWrite). Drop EVERY cached page of qid_path + bump gen. Buffers
// are retained for reuse.
void larder_page_invalidate(struct larder *l, u64 qid_path);

// Free every lazily-allocated page buffer (called from p9_client_destroy).
void larder_destroy(struct larder *l);
```

All are NULL-defensive (a NULL Larder is a miss / no-op). `larder_attr_serve`,
`larder_walk_serve`, and `larder_page_serve` copy their data out under the lock — a
concurrent invalidate/evict can never tear a serve or free a page mid-copy.

## Data structures

```c
#define LARDER_ATTR_ENTRIES 256u   // bounded (I-32); tunable

struct larder_attr_ent {
    u64            qid_path;   // key; root's qid.path is 0x0, so `valid` marks empty
    u32            cvers;      // content-version (qid.version == si_cvers) at fetch
    bool           valid;
    u64            lru;        // last-access sequence (LRU eviction)
    struct t_stat  attr;       // cached metadata (80 B)
};

struct larder {
    spin_lock_t             lock;       // dedicated near-leaf; never held across an RPC
    u64                     lru_clock;  // stamps ent->lru on access
    u64                     gen;        // monotonic invalidation sequence (populate guard)
    struct larder_attr_ent  attr[LARDER_ATTR_ENTRIES];
    u64 attr_hits, attr_misses, attr_installs, attr_install_skips,
        attr_invalidations, attr_evictions;   // diagnostics (hit-rate)
};
```

~26 KiB, embedded per `p9_client` (heap-only; a client already carries a 32 KiB
`out_buf`). The `valid` bit — not a `qid_path==0` sentinel — is load-bearing:
root's `qid.path` is legitimately `0x0` (`dev9p.c:130`).

## Implementation (mapping to `specs/fs_cache.tla`)

| Spec action | Impl |
|---|---|
| `Read` (serve, no re-check) | `dev9p_stat_native` calls `larder_attr_serve` first; a hit returns with no RPC. The base X-check (`stalk.c:326` → `spoor_stat_native`) and `SYS_FSTAT` are the consumers — the biggest cheap win. |
| `Open`/`Refetch` (install fresh) | `dev9p_walk_attrs` installs each walked component's `{sts[i], w->qid[i].vers}` after the `Twalkgetattr` RPC (free — attrs already fetched); `dev9p_stat_native`'s miss-populate installs after its `Tgetattr`. Both ALWAYS install from the fresh RPC (revalidate-by-overwrite). |
| `OwnWrite` (write-through invalidate) | **attr:** `larder_attr_invalidate` at `dev9p_write` / `dev9p_wstat_native` (the file), `dev9p_create` (parent AND child), `dev9p_rename` (both dirs), `dev9p_unlink` (parent). **page:** `larder_page_invalidate` at `dev9p_write` (the file) AND `dev9p_create` (the child — L1f audit F1: the reused-ino page twin of the attr defense, so a create at a freed+reused `qid.path` cannot serve a prior occupant's stale page). **dentry:** `larder_dentry_invalidate_name` at create / rename (both dirs) / unlink — the mutated `(parent, name)` binding ONLY; siblings preserved (matches `fs_cache.tla` per-token `OwnWrite(f)`; O(1), retires the whole-parent O(dentry_cap) scan). |
| `Evict` (bound) | `attr_install_slot_locked` picks existing-key / free / LRU-min victim; never exceeds `LARDER_ATTR_ENTRIES`. |

**The attr serve is a `Read`, not an `Open`.** L1c serves a cached attr WITHOUT
re-checking `cvers` — that is the RPC-elision win. Coherence is not from a
serve-time compare but from (a) own-write invalidation and (b) `walk_attrs`
re-populating fresh on every resolution. `NoWrongRead` is ABSOLUTE in the
single-writer regime (Thylacine serves its own FS to one guest) precisely because
own-writes invalidate. The `cvers` compare becomes load-bearing at the L1d
dentry-serve / L1e page-serve (which skip an RPC on a version match).

### The two SMP disciplines (LARDER-DESIGN §7 — the load-bearing property)

1. **Serve/invalidate atomicity.** Both take the leaf lock; the serve copies the
   `t_stat` out under it. A serve reads a whole valid entry or misses — never a
   torn entry. The lock is a pure leaf (never held with `c->lock`; the RPCs that
   take `c->lock` run outside it), so there is no lock-order edge to `c->lock` or
   the buddy allocator.

2. **The populate GEN guard.** The spec's `Open` reads the current `cvers` and
   installs atomically; the impl reads the `cvers` via an RPC and installs later
   — a non-atomic window. If a concurrent own-write commits a higher `cvers` AND
   invalidates the entry during that window, installing the stale RPC result
   would RESURRECT a value the write's invalidate already dropped, and a later
   causal reader would serve it (unbounded staleness → an I-38/I-28 violation).
   The `gen` counter (bumped on EVERY invalidate) is captured before the RPC
   (`larder_attr_serve`'s miss out-param / `larder_gen_snapshot`) and re-checked
   at install; a populate that raced an invalidate is skipped (a harmless missed
   fill — the next access refetches). This realizes the spec's atomic `Open`.
   `gen` is global (any invalidate blocks any concurrent populate) — sound but
   over-conservative; the target workload is depth-1, so skips are effectively
   zero. A per-key `gen` is a v1.x refinement.

### The reuse hazard (create invalidates the child)

Stratum reuses freed inos (`si_cvers` is monotonic across reuse, `si_gen` bumps).
A qid.path whose file was deleted can carry a stale prior-occupant attr until that
qid.path is invalidated or overwritten. The paths to a Spoor for a reused ino:
`walk_attrs` (overwrites fresh) or `create` (Tlcreate/Tmkdir). The create path
never runs `walk_attrs`, so `dev9p_create` invalidates BOTH the parent (its
nlink/mtime/cvers changed) AND the child's new qid.path (drop any stale
prior-occupant attr). Without the child-invalidate, the base X-check on a
recreated dir served the deleted file's attr — the `stalk-2` E2E
delete+recreate+create-in-it failure, guarded by `dev9p.create_invalidates_reused_child`.

`unlink`/`rename` do not invalidate the child (they operate by name — the child's
qid.path is not in hand without an extra RPC); this leaves a minor stale-nlink on
an unlink-then-fstat-of-an-open-fd (the v9fs `cache=loose` residual). Any REUSE of
the freed ino is still closed — it goes through `create` (invalidated) or
`walk_attrs` (overwritten).

## The dentry sub-cache (L1d)

```c
#define LARDER_DENTRY_ENTRIES  256u
#define LARDER_DENTRY_NAME_MAX 88u   // a longer component is not cached (fail-safe)

struct larder_dentry_ent {
    u64  parent_qid_path;   // key part 1
    u64  child_qid_path;    // resolved child (positive only; 0 when negative)
    u32  name_len;          // key part 2: name[0..name_len)
    bool valid;
    bool negative;          // true: (parent,name) -> ENOENT (no child)
    u64  lru;
    char name[LARDER_DENTRY_NAME_MAX];   // the component (NOT NUL-terminated)
};
```

The dentry cache maps `(parent-qid.path, name) → child`, **including negative
entries** (`… → ENOENT`, the failed-lookup-storm win). It stores only the linkage:
a positive dentry's reply attr for the walk is served from the **attr** sub-cache
(the child's qid.path keys it), so there is no attr duplication.

**Coherence = own-write invalidation, NOT a cvers gate (the L1d ground-truth
correction).** Unlike an attr or a page — each validated by its own file's
`cvers` — a name→child binding is a fact about the PARENT directory's dirent set,
and Stratum surfaces no directory-content version that tracks a dirent change.
Verified in the Stratum tree (`src/fs/fs.c`): a child **create**
(`stm_fs_create_file`) and **unlink** (`fs_unlink_inode_and_dirent`) touch only
the separate dirent index — the parent inode is never `stm_inode_set`, so the
parent's `si_cvers` does **not** bump (only **rename** stamps the parents' mtime,
`fs.c:5507`). A parent-`cvers` compare would therefore FALSELY MATCH a stale
negative dentry after a create (serve `ENOENT` for a now-existing file). So the
dentry cache's sole coherence mechanism is dropping the mutated `(parent, name)`
binding on every guest **create / rename / unlink** in it (`larder_dentry_invalidate_name`
at `dev9p_create` / `dev9p_rename` [both endpoints] / `dev9p_unlink`; the guest holds
both the parent's qid.path AND the mutated name at each site). Only the named
binding stales — a create of `foo` cannot change whether a sibling `bar` exists —
so siblings are preserved (an over-broad whole-parent drop forced every sibling to
re-walk, the cold-band wga thrash), and the drop is O(1) via the serve's
`(parent,name)` hash (retiring the whole-parent O(dentry_cap) scan). This maps onto `fs_cache.tla`'s `Read` +
`OwnWrite` single-writer subset (`NoWrongRead` absolute) — exactly the attr
sub-cache's discipline, keyed by `(parent,name)`. No `Open` gate for dentries.

**Serve mechanics (`larder_walk_serve` at `dev9p_walk_attrs`, before the RPC).**
The whole resolver run is served from cache under ONE lock hold (an atomic
snapshot): chain `(base, name₀) → child₀`, `(child₀, name₁) → child₁`, …; each
positive hop fills `sts[i]` from the attr sub-cache (an attr miss bails the serve
to the RPC), and the next hop's parent is the resolved child. A negative hop is
the walk's miss (`*nresolved = i`, `*is_miss = 1`). The caller (`dev9p_walk_attrs`)
skips the RPC for a **full positive run only in the query form** (`nc == NULL` —
`SYS_STAT`'s final run; no server fid to bind) and for a **cached miss in either
form** (a miss binds nothing); a bind-form full walk still RPCs to bind the fid,
then re-populates. The dentry cache holds the raw dev9p-tree bindings
(Territory-independent — `walk_attrs` walks the 9P server, not the per-Proc mount
overlay), so the resolver applies the mount view to the served qids exactly as to
an RPC reply, and a negative dentry can never be a mount point (a mount point must
exist in the underlying tree). The resolver re-applies the per-component X-check
on the served prefix, so a negative serve preserves the fail-ordering (an
X-denial on an ancestor masks the miss).

**Populate.** `dev9p_walk_attrs` installs a positive dentry per walked component
(the parent is the base for hop 0, else the previous component) and, on a partial
walk (`nwqid < nname`) or a first-component `Rlerror ENOENT`, a negative dentry at
the miss. All gen-guarded by the same pre-RPC `wga_seq0` snapshot as the attr
populate. NEVER from a readdir qid (the L1a-2 rule).

## The page sub-cache (L1e)

`(qid.path, page_index) → { bytes[0..valid_len), cvers }`, a **32768-slot O(1) table**
(`LARDER_PAGE_ENTRIES`; **128 MiB CEILING** of heap at capacity — lazy, so only touched
pages allocate a buffer). Both the slot-entry array AND the per-slot 4 KiB buffers are
**heap, lazily allocated on the first install** — so a non-cacheable client (netd)
carries neither, and the cap scales past what an inline `struct larder` member allows.
Buffers are reused across evictions; the array, buffers, and both hashes are freed at
`larder_destroy` (from `p9_client_destroy`). Maps to `fs_cache.tla`'s `Read` + `Open` +
`OwnWrite` on content tokens — **no spec extension** (a page is a content token like an
attr).

**The index (task #25, O(1) at scale; the qid_path secondary index, task #29).** A
**chained hash** keyed `(qid.path, page_index)` (`page_hash` buckets, an intrusive
`hnext` chain per slot) gives O(1) `page_find`; a **free-cursor** (`page_used`) fills
fresh slots O(1); a **CLOCK second-chance** hand (`page_hand` + a per-slot `ref` bit)
evicts O(1)-amortized once full. A SECOND hash — `page_qhash`, keyed by `qid.path`
ALONE, with an intrusive `qnext` chain — makes `larder_page_invalidate` **O(pages-of-
file)** instead of O(`page_cap`): every page of one file shares one qbucket, so an
own-write walks only that file's pages (+ hash collisions), never the whole cap (the F3
fix — see below). The load-bearing invariant is that **a slot is in `page_qhash` IFF it
is in `page_hash`** (linked/unlinked in lockstep at every install / victim-reuse /
buffer-OOM / invalidate site). The prior 512-slot design used an inline array with O(N)
sequential-scan find/evict — which does not scale. The coherence contract (gen-guard /
cvers / own-write invalidate / serve-copy-under-lock) is byte-identical to the O(N) L1e
— only the index + eviction mechanics changed; all 13 page tests pass.

**Sizing + what it buys (measured 2026-07-10; task #29 re-measure).** 8192 slots
(32 MiB) THRASHED the go-build read working set — build2-warm **pe=12119** evictions,
gofmt-cold pe=18733 — because a Go build reads the package archives **SEQUENTIALLY** (an
LRU-hostile scan, NOT the Zipf hot-subset the 8192 note assumed): the cache helps only
once it holds the WHOLE working set. Sizing to hold it (build2 ~20k pages, gofmt-cold
~27k) drove **pe → 0** on every window — a **COMPOUND** win: (a) reads hit (build2 read
RTs 2489 → ~980) AND (b) cached-open **coverage** now passes (pages stay resident to the
next open) so cached-open MINTs jumped 3.6× (396 → ~1440) and its `hcover` fallback
collapsed −88% (1185 → ~144), cutting the fid lifecycle too. Warm floor: build2
605 → ~510 ms, gofmt 805 → ~663 ms (both ~−16-18 %); gofmt-cold 2221 → ~2060 ms (−7 %).
The knee is ~27k (gofmt-cold); 32768 is the pow2 above it with margin. **A real project
larger than gofmt re-thrashes** → a memory-pressure-adaptive cap (a shrinker) is the
v1.x design (Thylacine has no reclaim framework yet); buffers stay resident until
`larder_destroy` (mount teardown) — the v1.0 no-reclaim window, bounded per client.

**The cacheability gate (§3.3, the load-bearing L1e addition).** The page cache
caches file CONTENT, so it engages ONLY for a `cacheable` client (`p9_client.cacheable`
— latched true by a successful `Twalkgetattr`, the v1.0 proxy for a
content-versioned, offset-stable FS). `dev9p_read` gates serve *and* populate on it,
so a stream server (netd `/net` — consuming reads, `qid.version` 0) is never
page-cached (an offset re-read would serve stale stream bytes). Fail-safe (default
false, settled before the first read); it also gates `dev9p_stat_native` (closing
the latent L1c netd-attr gap).

**Serve.** `dev9p_read` computes `page_index = off / 4096`, `page_off = off % 4096`,
and serves the one page if it exists, `cvers == c->qid.vers` (the reader fid's
version — the close-to-open `Open` gate; catches a cross-open external write), and
`page_off < valid_len`. A hit copies `min(want, valid_len - page_off)` bytes under
the lock and returns that count — a short serve is a legal short read the caller
loops on (one-page-per-call bounds the ≤ 4 KiB copy under the leaf lock). A partial
(small-file / EOF) page serves only within `[0, valid_len)` and misses beyond (the
caller's next read refetches), so **no EOF determination is needed**.

**Populate.** After the read RPC, `dev9p_read` installs each page the read covered
**from its aligned start** (`page_start ∈ [offset, offset+got)`), so every cached
page holds `[0, valid_len)` with no hole; an unaligned read's partial-front page is
skipped. `cvers = c->qid.vers`; gen-guarded by the `seq0` snapshot from the serve
miss (skips a fill that raced an own-write — the resurrection close).

**Invalidate (O(pages-of-file), the F3 fix — task #29).** `dev9p_write` drops every
cached page of the file (`larder_page_invalidate` by `qid.path`) + bumps gen, so the
guest sees its own write strongly. It walks ONLY the file's `page_qhash` qbucket chain
(every page of `qid.path` hashes there), splicing each matching valid slot out of BOTH
indexes and marking it `!valid` (buffer retained for reuse) — **O(pages-of-file + qbucket
collisions), independent of `page_cap`**. A `!valid` same-`qid.path` slot (a fresh
install mid-copy) is left linked: the gen bump makes its install's `seq0` guard skip
(the resurrection close), and CLOCK reclaims it later (unlinking both). A collision from
another file (`qid.path` mismatch) is skipped, staying linked. Before task #29 this was
an O(`page_cap`) full-array scan per own-write — tolerable at 8192 but a ~193M-scan tax
at 32768 on the write-heavy cold path; the qid_path index is what makes the large cap
affordable. (A whole-file drop, not per-range — sound; a precise per-range invalidate is
a v1.x tuning.)

**Page-buffer lifetime (SMP).** A serve copies OUT under the lock; an evict / reuse
/ invalidate / destroy runs UNDER the lock; a slot's buffer is never freed on evict
(only reused or freed at destroy), so a concurrent serve can never read a freed
buffer. `kmalloc(4096)` is non-blocking (buddy `zone->lock`, a leaf below the leaf
`l->lock` — the **`l->lock → buddy` order**), so the lazy alloc is safe under the
lock (LARDER-DESIGN §7 anticipated this order).

## The D44 read-band fixes: aligned wire reads + attr-served EOF (2026-07-11)

The CHASE D44 miss-class instrument decomposed the warm go-build's wire-read
band and found 82% of the page-serve misses were **permanent partial-page
holes**, not capacity or coherence misses:

- The msize payload (131049 B = 128 KiB msize minus the 23-byte Rread
  framing) is not a page multiple, so a big sequential stream's chunks each
  end in a partial page (`valid_len` 4073).
- `larder_page_install` populates only from a page's ALIGNED start, so a
  partial-FRONT page could never be refilled by the continuing stream: the
  hole persisted forever, and every re-read of the stream pv-missed at it and
  re-paid the wire for the entire tail.
- The dominant victims were the go toolchain's multi-MB rodata+data segments,
  eagerly `dev->read` on EVERY exec by `kernel/exec.c::map_eager_from_file`
  (REVENANT demand-pages only the text segment): `/goroot/bin/go` re-read
  6.1 MB per invocation (even `go version`); `compile` 8.7 MB x 91 execs
  = 792 MB over one cold build.

Two guest-local fixes in `dev9p_read` (no ABI, no server change):

**(a) Aligned wire reads.** A big (> one page) unaligned read on a CACHEABLE
client is wired at the containing page's aligned start (`wire_off = offset
- offset % 4096`) into the caller's own buffer, populated from `wire_off`
(the front page installs FULL -- the heal), then the caller's slice is
shifted forward (`co_copy`, dst < src, overlap-safe) and returned as a legal
SHORT read. Each chunk of a sequential stream thereby fully rewrites its
predecessor's partial tail page: holes heal in one pass and the second pass
serves entirely from pages. Cost: <= 4095 duplicate bytes per chunk (~3%);
zero allocation on the common path. The `got <= lead` arm splits (the
D44-audit F1 [P1] close): `got == 0` proves the file ends at/before
`wire_off <= offset` -> a TRUE EOF (0); but `0 < got <= lead` is a server
short-return BEFORE the caller's offset -- a single Rread may legitimately
short-return mid-file (the R-5 SA-F1 ground truth) -- so it is NOT an EOF:
the fix retries UNSHIFTED at the caller's offset and returns that verbatim
(the pre-fix `return 0` manufactured a false mid-file EOF: the REVENANT
cluster fill would install zero-filled resident text pages, exec's eager
segment read would spuriously fail, and userspace streams would truncate).
Small reads (<= one page) keep the byte-exact path: they never populate, so
they cannot create holes, and naive non-looping small readers keep exact
semantics. Non-cacheable clients (netd streams) are untouched.

**(b) Attr-served EOF.** `larder_attr_fresh_size` (larder.c) returns the
cached attr's size iff its `cvers` matches the reading fid's open-time
`qid.vers` -- the SAME freshness rule as the page serve (unlike
`larder_attr_serve`, which is deliberately ungated per L1c). A read at
`offset >= size` on a PLAIN FILE (qid.type 0) then returns 0 RPC-free: the
fresh size is the open-time size, so under close-to-open + own-write
invalidation the wire would answer identically. This kills the sequential
reader's final read-returns-0 probe (one RPC per streamed file). The
plain-file gate keeps a directory's cached size from converting the server's
read-on-directory error into a silent 0. The EOF determination comes from
the attr's authoritative size, NOT from a short page -- the "no EOF
determination from pages" rule stands.

Neither fix extends `fs_cache.tla`: the aligned read changes only the wire
request shape (invisible to the model); the attr-EOF is a `Read` of the
modeled attr cache.

Like the page serve, the attr-served EOF rests on the I-38 single-writer
premise: an external writer's append (invisible to own-write invalidation)
would make the cached open-time size diverge from a fresh RPC until the next
revalidation -- the same close-to-open window the page cache already
carries (the D44-audit F2 disposition). And `dev9p_open` with OTRUNC now
drops the file's cached attr + pages (own-write-through -- the truncate is
an own write), so cache soundness does not rest on the server bumping
qid.version on truncate (the D44-audit F3 close, a PRE-EXISTING gap).

Tests: `dev9p.read_align_heals_partial` (pass 2 of a re-streamed 20000-byte
pattern file wires ONLY the tail EOF probe + its ambiguity retry -- the
shifted probe returns got == lead, indistinguishable from a mid-file short,
so the unshifted retry confirms; a fresh attr would serve it with zero wire
ops; the chunk-2 wire offset is asserted ALIGNED; fails pre-fix),
`dev9p.read_align_short_not_eof` (a one-shot
mid-file short-return injection below `lead`; the caller must still receive
the whole file -- fails pre-F1-fix by construction),
`dev9p.open_trunc_invalidates` (an OTRUNC open drops the cached attr), and
`dev9p.read_eof_attr_served` (fresh attr -> zero wire ops; absent attr /
post-own-write -> the probe wires). The loopback
fixture gained a big-file pattern mode (`g_tread_file_size`) and msize-sized
buffers (the 4096-byte `g_recv_buf`/`g_loopback_resp` were under the
negotiated msize 8192 -- dormant until the first > 4 KiB reply).

Measured (instrumented, N=3, clean sentinels, smp=4): warm gofmt 367 ->
249 ms median (read band 157 -> 64 ms; wire reads 1568 -> 935; bytes 64 MB
-> 20 MB; szbig 166 -> 0; page serves 1037 -> 12632); S3 cold 5486 -> 4088;
gofmt-cold 1540 -> 1123; version-warm 18 -> 7 ms. Remaining seam: the FIRST
exec per boot still eagerly wire-reads rodata+data; the structural fix is
REVENANT file-backing the R-only rodata segment (Image-cached like text,
R+XN) -- the next S3 lever.

## The FID-LIFECYCLE re-size (attr + dentry heap + O(1) hash, 2026-07-10)

The attr and dentry sub-caches moved from 256-slot INLINE linear arrays to
4096-slot HEAP arrays with the page cache's exact O(1) index shape (chained
hash + free-cursor fill + CLOCK second-chance eviction -- the task-#25 pattern
replicated; `attr_*`/`dentry_*` helpers mirror the `page_*` ones). Why: the
cached-open hint (docs/FID-LIFECYCLE-DESIGN.md section 3.3) expands the
metadata working set from the L1c base-X-check dir set (~7 hot dirs) to EVERY
opened file (~800+ distinct leaves per warm go build); at 256 slots the leaf
entries evicted before re-open (measured: 86% of cached-open attempts died on
the chain; 4096 slots -> `hchain` 1525 -> 10). Both arrays are lazily allocated
on the first install, so a non-cacheable client (netd) allocates neither and
`struct larder` (inline in `p9_client`) SHRANK by ~56 KiB. The coherence
contract (gen guard / own-write invalidate / serve-copy-under-lock) is
byte-identical; only the index + eviction mechanics changed (exact-LRU ->
CLOCK, an approximate LRU). The dentry hash folds the component name through
FNV-1a mixed with the parent qid; chains full-compare. `larder_destroy` frees
all three arrays + hashes. Order-7 kmalloc caveat: both arrays are ~426/491 KiB
contiguous allocations -- the same #25-F1 posture (an OOM skips the install,
self-heals on a later attempt, correctness never depends on a fill).

## The F1 write-behind (per-open-file append-run staging, 2026-07-11)

The loose writeback leg (LARDER-DESIGN §12, Senate-voted; the CHASE C-2
measured motivation: 97.9% of the S3 cold build's 19.6k wire Twrites were
<= 4 KiB — Go's bufio dribble, each object written twice). On a LOOSE +
cacheable client, `dev9p_write` STAGES small pure-append writes into a
per-open-file contiguous run instead of paying a wire RPC each; the run
flushes as msize-max Twrites. Measured effect (the F1 N=3 bench): wire
writes 19.6k → 2.2k (−89%), the write band 936 → 283 ms, S3 median
3614 → 3297 ms.

State lives on `dev9p_priv` (NOT in `struct larder` — the run is per-OPEN,
not per-file): `wb_lock` (a pure leaf spinlock; byte copies + kmalloc/kfree
only under it, the larder leaf→buddy order; wire I/O never), the run
`{wb_off, wb_len, wb_buf, wb_cap}`, the append anchor `wb_base` (valid iff
`wb_known`), the freeze count `wb_flushers`, the error latch `wb_err`, and
the fast-path hint `wb_eligible`.

Key contracts (`kernel/dev9p.c` `wb_*` helpers):

- **Eligibility = create/OTRUNC-born** on a loose+cacheable client, plain
  file. The anchor is born 0 and advances only through completed flushes +
  completed write-throughs (`max`), so `wb_base <= true file end` always,
  and the run — which only ever starts AT the anchor — sits at the file's
  end with no hole below it. An opened-existing file never stages; a
  `wstat` flushes then permanently de-eligibilizes (a truncate moves the
  end).
- **The stage gate**: pure append (`off == run end`, or `off == wb_base`
  when empty) AND `count <= DEV9P_WB_STAGE_MAX` (32 KiB — bounds the copy
  under the spinlock; bigger writes are already wire-efficient) AND
  `wb_flushers == 0`. Everything else: if a run exists and the write
  OVERLAPS it, flush-then-write-through (ordering: staged bytes are
  older); if disjoint (appends past a frozen run's end; interiors wholly
  below), write through directly — order-free.
- **The visible-run SINGLE-FLIGHT flush** (`wb_flush_locked`; the SA-F1
  close @4c823d30): the flusher snapshots `{off,total,buf}` under the
  lock, bumps `wb_flushers` (freezing the run: no stage, no growth
  realloc, so the out-of-lock buf reads cannot race a move), and wires
  msize-clamped chunks outside the lock. A second flush-needing party
  yield-waits at entry (`while (wb_flushers) { unlock; sched(); lock; }`
  — the on_cpu-spin class, bounded by the flusher's independent progress
  incl. its #811 death-unwind). Duplicate concurrent flushes are
  FORBIDDEN, not idempotent: a completed duplicate would let an
  ordering-dependent through-write land while the first flusher's stale
  residual chunks still fly, silently overwriting it. Close and the cap
  flush never contend (last-ref; staging requires `wb_flushers == 0`).
- **Flush sites**: close (BEFORE the async-clunk Tclunk — the fid must be
  live), fsync (then `Tfsync`; also waits for a run-less in-flight flush
  — its contract needs those bytes durable), the 256-KiB cap (inline in
  the writer, then re-stages), any overlapping non-append write, wstat.
  Death: #926 closes handles at exit → the close flush runs.
- **Reads on the staging fd** split three ways with NO flush: below the
  run = server/cache (complete — the anchor discipline means the server
  holds every byte below `wb_off`); within = served from `wb_buf` under
  the lock (a short read up to the run end; POSIX short-read composition
  carries spanning callers across); at/past the run end = falls through
  to the normal path (the wire/attr answer EOF honestly — never a
  synthesized 0, so racing extensions stay visible). The attr-EOF fast
  path stays sound during staging: the cached attr's size == `wb_off`
  (the pre-stage server size; cvers cannot move while staging), so it can
  only fire for `offset >= run end`, where 0 is the correct answer.
- **`fstat` patches** `size = max(server view, run end)` on both the
  larder-serve and wire-fetch paths (`wb_patch_stat_size`) — the Go #46
  truncate-gate fstats its own O_WRONLY fd mid-open. mtime stays the
  server's until the flush (the documented writeback posture); path-stats
  via other fids see the last-flushed state (close-to-open-legal).
- **Invalidates move per-write → per-FLUSH** (`fs_cache.tla` OwnWrite
  realized at the wire moment): the attr + page drops happen after the
  flush's wire writes, outside `wb_lock`. Write-throughs keep their
  immediate invalidates.
- **Errors (the voted NFS model)**: a flush failure latches a positive
  errno in `wb_err` and DROPS the run (retry-forever would wedge close);
  every subsequent write/fsync on the fd returns it — fsync is the
  reliable channel (`Dev.close` is void at v1.0, the documented seam).
  The buffer stays allocated until close (freed there; the global budget
  uncharged in full).
- **The global budget** `DEV9P_WB_BUDGET` (8 MiB; `dev9p_wb_budget_used`
  diagnostic) is charged by the growth DELTA and uncharged in full at
  close — GLOBAL, not per-Proc, because the priv crosses Proc boundaries
  (dup/rfork/#926 close-by-inheritor), exactly the `DEV9P_CO_BUDGET`
  reasoning. Denial (or kmalloc failure) degrades to write-through.

Known seams: Loom async payload ops bypass `dev9p_write`/`dev9p_read`, so
they neither stage nor overlay — the L1f-F2 self-inflicted-only class (no
v1.0 consumer drives Loom I/O on build files); v1.x unifies at
`loom_submit_payload`. The deterministic two-thread-same-fd flush-race
harness is impossible in the synchronous loopback fixture (a flush
completes within the call, so `wb_flushers` is never observably > 0) —
the same owed SMP-harness class as #349/#841/Loom.

The same chunk widened the kernel-image L3 window 4 → 8 MiB
(`mmu.c` `KERNEL_L3_TABLES` = 4 + both `kernel.ld` guards + the
direct-map alias tables in lockstep) and the KASLR slide alignment to
8 MiB (`KASLR_ALIGN_BITS` 23; 11 bits of slide entropy over 16 GiB —
I-16 preserved): the UBSan image crossed the 4-MiB ceiling, the second
iteration of the documented P5-kernel-l3-4mib move.

## State / coherence properties

- **Own-write happens-before a later read → the read sees the write.** The
  invalidate is part of `dev9p_write`/etc.'s completion (before it returns), so
  any read that starts after the write returns misses → refetches fresh. A read
  CONCURRENT with the write is an unsynchronized application race (POSIX allows
  either value). Under write-behind the same property holds via the overlay: a
  staged (not yet flushed) own-write is served from the run itself; the
  invalidate accompanies the eventual flush.
- **Base X-check bounded staleness (I-28).** A tightened dir permission via the
  guest's own `chmod` invalidates immediately (`dev9p_wstat_native`); an
  out-of-band tighten is bounded by the next `walk_attrs` revalidation. This is
  the accepted unsynchronized-snapshot TOCTOU the resolver already carries (§4).

## Tests

- `kernel/test/test_dev9p.c` (write-behind, F1) — `dev9p.wb_coalesce_one_twrite`
  (4 appends → zero wire → ONE close Twrite, payload byte-verified, ordered
  before the Tclunk, budget balanced; plus the strict-client non-regression) /
  `wb_overlay_read` (within/spanning/past/below arms + post-flush re-staging) /
  `wb_flush_at_close` (the OTRUNC-eligibility twin) / `wb_fsync_flush_and_error`
  (Twrite-before-Tfsync order; one-shot Rlerror injection → the latch on
  write+fsync → close emits nothing) / `wb_nonappend_writethrough` (flush-then-
  patch order + wstat flush/de-eligibility) / `wb_fstat_staged_size` (both stat
  paths) / `wb_cap_flush` (256-KiB inline flush; every wire byte pattern-
  verified) / `wb_budget_fallback` (bias-exhaust → write-through → recovery).
- `kernel/test/test_larder.c` (attr) — `larder.install_serve` / `serve_miss` /
  `invalidate` / `gen_guard` (the resurrection close) / `root_qid_zero` /
  `overwrite_wins` / `eviction_bounded` (I-32).
- `kernel/test/test_larder.c` (dentry, L1d) — `larder.dentry_serve` /
  `dentry_serve_miss` / `dentry_negative` / `dentry_multi_hop` /
  `dentry_partial_chain_bails` / `dentry_attr_miss_bails` /
  `dentry_invalidate_name` (the ground-truth core: own-write invalidation drops the
  stale named binding AND preserves a sibling — fails on the old whole-parent drop) /
  `dentry_gen_guard` / `dentry_name_too_long` / `dentry_bounded`.
- `kernel/test/test_dev9p.c::test_dev9p_create_invalidates_reused_child` — the
  L1c create-reuse attr regression (non-vacuous: `1054/1055 FAIL` with the
  child-invalidate reverted).
- `kernel/test/test_dev9p.c::test_dev9p_create_invalidates_negative_dentry` — the
  L1d create→dentry-invalidate hook regression (non-vacuous: FAILs with the
  `larder_dentry_invalidate_name` call reverted). `test_dev9p_walk_attrs` resets
  the Larder between its wire sub-tests (they flip the wire result for one path
  without a mutation — non-physical for single-writer — so the cache would
  otherwise serve a stale entry).
- `kernel/test/test_larder.c` (page, L1e) — `larder.page_serve` (bytes verbatim) /
  `page_serve_miss` (gen handed back) / `page_offset` (page_off + want) /
  `page_cvers_mismatch` (the Open gate) / `page_partial` (serve within valid_len,
  miss beyond) / `page_invalidate` (OwnWrite drops all pages + bumps gen) /
  `page_invalidate_multifile` (task #29 — the qid_path secondary index drops
  EXACTLY the written file's pages [count == 3] and leaves another file's pages
  intact, then re-installs into the freed slots: proves the O(pages-of-file) walk's
  discrimination + the two-index lockstep invariant) / `page_gen_guard` (the
  resurrection close) / `page_overwrite` (buffer reused) / `page_bounded` (I-32) /
  `page_destroy_frees`.
- `kernel/test/test_dev9p.c::test_dev9p_page_cache_serve_and_gate` — the L1e
  integration proof (non-vacuous: `1076/1077 FAIL` with the `dev9p_read` serve
  disabled): a cacheable client's re-read is served with NO second Tread (the
  sentinel-offset trick reuses the loopback's Tread-offset capture as a
  "was a Tread sent?" probe), a non-cacheable client's re-read RPCs (the gate), and
  an own-write invalidates the page.
- In-guest: `stalk-2` cross-mount E2E (the delete+recreate+create-in-it sequence),
  `fs-mut-smoke`, `probe46` (fstat). 1077/1077 + boot OK + reduced SMP gate 20/20
  (default+ubsan-smp4, 0 corruption).

## Error paths

None user-visible: a cache miss falls through to the real RPC; an OOM cannot occur
(the array is inline, fixed); a NULL Larder degrades to always-miss. A gen-guard
skip is a missed fill, not an error.

## Performance

L1c targets the base X-check re-stat storm + fstat redundancy (~1/3 of warm ops;
"a one-entry cache of root's attr alone kills ~1/3 of warm ops"). The full gofmt
cold/warm re-measure vs the LARDER-DESIGN §10 predictions lands at L1f (after the
dentry + page sub-caches). The `attr_*` diagnostics counters expose the hit rate.

## Write-populate (G1, term-4)

`larder_page_install_own(l, qid, idx, page_off, data, len)` installs the
writer's own landed bytes at the wb flush (`wb_flush_locked`, err==0 arm
only -- the `fs_cache_buggy_populate_unflushed` counterexample pins the
coupling). `own` pages serve without the cvers gate and count toward
`pages_cover` (cached-open works on just-written files); a normal
`larder_page_install` over the same key clears `own` (upgrade to cvers-
gated). `page_off > 0` extends only an OWN page with `valid_len == page_off`
(the append-chain continuation); other shapes are skipped. No populate gen
guard: the bytes are the writer's just-landed content and the wb flush
freeze excludes same-file mutators (LARDER-DESIGN section 13).

`larder_page_invalidate_range(l, qid, first_idx, last_idx)` is the
write-through discipline (G1b): a non-append write drops only the touched
pages; own pages outside the range keep serving (single-writer: their bytes
are still current), cvers pages outside miss at the next revalidation
exactly as if dropped. Whole-file drops remain on OTRUNC, create-reuse
(L1f F1), and the failed-flush arm. Tests: `dev9p.wb_populate_readback` /
`wb_populate_append_chain` / `wb_populate_failed_flush` /
`wb_writethrough_range` (all revert-probed) + the updated
`dev9p.wb_overlay_read` below-run contract.

## Perm-valid attr downgrade (G3) + qid-scoped gen guard (G4, term-4)

G3 (`larder_attr_downgrade`, larder.c): a parent dir's child-mutation
(create/unlink/rename call sites in dev9p.c) marks the dir's attr entry
`perm_only` instead of dropping it. A perm_only entry serves ONLY a resolver
intermediate hop inside `larder_walk_serve` (the X-check reads mode/uid/gid +
the immutable qid fields -- all unchanged by the downgrade-triggering
mutations); `larder_attr_serve`, `larder_attr_fresh_size`, and the final hop
of a full positive run treat it as a miss (their consumers read the staled
size/times/cvers). Any full install clears the flag (upgrade). The downgrade
logs an invalidation event exactly like `larder_attr_invalidate`, so the
raced-populate resurrection close is unchanged. wstat (which CAN edit perm
bits) and the create-child reused-ino defense keep full drops.

G4 (`inval_log_locked` / `inval_hits_locked`, larder.c): every invalidation
event records its staled qid in the 128-slot `inval_qid` ring (slot = seq %
LARDER_INVAL_RING); the three install guards + the `larder_pages_snapshot`
gen witness skip only when THEIR key appears among the events in (seq0, gen]
-- an unrelated file's event no longer discards the fill (pre-G4: 726-886
lost installs per S3 cold-build window). Keys: attr/page installs + the
snapshot witness = the file's qid; the dentry install = the PARENT qid (all
dentry-staling events log the parent). A window wider than the ring
fail-safes to the global skip. `inval_scope_passes` counts recovered fills;
`attr_downgrades` counts G3 downgrades.

Tests: `larder.attr_downgrade_perm_only`,
`larder.downgrade_guards_raced_populate`, `larder.gen_scope_qid`,
`dev9p.create_downgrades_parent_attr`; the three pre-existing gen-guard tests
now race the SAME key (the scoped contract). All revert-probed.

## Known caveats / footguns

- **v1.0 is single-writer-sound** (own-write invalidation; the accepted
  close-to-open window for an out-of-band writer). Writeback / persistent /
  cross-session / readdir caching are v1.x seams (LARDER-DESIGN §11).
- **qid.path keys, not full ino** (R92 P3-3): `qid_path` truncates ino to 32 bits,
  so a low-32 ino alias shares a cache key — infeasible at v1.0 (no pool has 4 B
  inodes), documented as the v2.0-ino-cap bound.
- **Never populate from a readdir qid** (the L1a-2 audit-F1 rule): Rreaddir's
  `qid.version` is a link-time `si_gen` snapshot, not `si_cvers`. L1c populates
  only from getattr/walk_attrs; `dev9p_readdir` has no populate.
- **The Larder engages only for a `cacheable` client (the L1e gate).** A
  per-`p9_client` `cacheable` flag (default false) latches true on the first
  successful `Twalkgetattr` (POUNCE) — the v1.0 proxy for a content-versioned,
  offset-stable FS. `dev9p_stat_native` (attr) and `dev9p_read` (page) gate serve
  *and* populate on it; the dentry `walk_serve` is additionally guarded by the
  `wga_unsupported` latch. A stream/control server (netd `/net` — consuming reads,
  `qid.version` 0) answers `Twalkgetattr` ENOSYS and never latches, so it is never
  cached (no stale stream bytes, no out-of-band-mutated stale attr). This closed
  the latent L1c gap (attr caching previously had no server gate). netd is the only
  non-Stratum dev9p client at v1.0; corvus is a byte-mode SrvConn (no Larder);
  `/proc` `/ctl` `/env` `/dev` are native kernel Devs (no dev9p, no Larder). The
  POUNCE-support proxy is not identical to the true property (a future POUNCE-but-
  streaming server, or a content-versioned FS without POUNCE, needs an explicit
  attach-time capability — a v1.x add); it is fail-safe (unproven → not cached).
- **The Loom async path bypasses the Larder (L1c/L1d seam).** The Larder is
  populated + invalidated only on the SYNCHRONOUS dev9p path. The Loom async
  engine (`kernel/loom.c` — `LOOM_OP_WRITE` / `MKNOD` / `SYMLINK` / `LINK` /
  `UNLINKAT` / `RENAMEAT`) drives `p9_client_*` directly and touches neither
  sub-cache, so a client mixing Loom FS *mutations* with synchronous *resolution*
  on the SAME mount is a second (out-of-band) writer from the Larder's view: a
  Loom write can leave a stale attr, a Loom dirent-mutation a stale dentry,
  bounded by the next own-sync mutation or LRU. Unreachable at v1.0 (no consumer
  mixes them). The v1.x fix invalidates from the Loom completion path. See
  LARDER-DESIGN §11.
- **The Stratum parent-mtime gap (tracked, not a Larder bug).** A child
  create/unlink does not update the parent directory's mtime/ctime in Stratum
  (only rename does) — a latent POSIX-compliance gap. It does not affect L1d
  soundness (own-write invalidation is independent of the parent `cvers`), and
  fixing it has a create-path COW cost; a v1.x tradeoff.
