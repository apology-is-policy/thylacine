# 132 — The Larder (guest-side 9P FS cache)

**Status:** L1c + L1d LANDED — the substrate + the **attr** sub-cache (L1c) + the
**dentry** sub-cache incl. negative entries (L1d). The page (L1e) sub-cache extends
the same owner/lock/key and is not yet built.

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

// Invalidate (OwnWrite). Drop EVERY dentry whose parent is parent_qid_path +
// bump gen. The dentry cache's SOLE coherence mechanism (no cvers gate).
void larder_dentry_invalidate_parent(struct larder *l, u64 parent_qid_path);
```

All are NULL-defensive (a NULL Larder is a miss / no-op). `larder_attr_serve` and
`larder_walk_serve` copy their `t_stat`(s) out under the lock — a concurrent
invalidate can never tear a serve.

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
| `OwnWrite` (write-through invalidate) | `larder_attr_invalidate` at `dev9p_write` / `dev9p_wstat_native` (the file), `dev9p_create` (parent AND child), `dev9p_rename` (both dirs), `dev9p_unlink` (parent). |
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
dentry cache's sole coherence mechanism is dropping a directory's cached dentries
on every guest **create / rename / unlink** in it (`larder_dentry_invalidate_parent`
at `dev9p_create` / `dev9p_rename` [both dirs] / `dev9p_unlink`; the guest holds
the parent's qid.path at each site). This maps onto `fs_cache.tla`'s `Read` +
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

## State / coherence properties

- **Own-write happens-before a later read → the read sees the write.** The
  invalidate is part of `dev9p_write`/etc.'s completion (before it returns), so
  any read that starts after the write returns misses → refetches fresh. A read
  CONCURRENT with the write is an unsynchronized application race (POSIX allows
  either value).
- **Base X-check bounded staleness (I-28).** A tightened dir permission via the
  guest's own `chmod` invalidates immediately (`dev9p_wstat_native`); an
  out-of-band tighten is bounded by the next `walk_attrs` revalidation. This is
  the accepted unsynchronized-snapshot TOCTOU the resolver already carries (§4).

## Tests

- `kernel/test/test_larder.c` (attr) — `larder.install_serve` / `serve_miss` /
  `invalidate` / `gen_guard` (the resurrection close) / `root_qid_zero` /
  `overwrite_wins` / `eviction_bounded` (I-32).
- `kernel/test/test_larder.c` (dentry, L1d) — `larder.dentry_serve` /
  `dentry_serve_miss` / `dentry_negative` / `dentry_multi_hop` /
  `dentry_partial_chain_bails` / `dentry_attr_miss_bails` /
  `dentry_invalidate_parent` (the ground-truth core: own-write invalidation drops
  a stale negative) / `dentry_gen_guard` / `dentry_name_too_long` /
  `dentry_bounded`.
- `kernel/test/test_dev9p.c::test_dev9p_create_invalidates_reused_child` — the
  L1c create-reuse attr regression (non-vacuous: `1054/1055 FAIL` with the
  child-invalidate reverted).
- `kernel/test/test_dev9p.c::test_dev9p_create_invalidates_negative_dentry` — the
  L1d create→dentry-invalidate hook regression (non-vacuous: FAILs with the
  `larder_dentry_invalidate_parent` call reverted). `test_dev9p_walk_attrs` resets
  the Larder between its wire sub-tests (they flip the wire result for one path
  without a mutation — non-physical for single-writer — so the cache would
  otherwise serve a stale entry).
- In-guest: `stalk-2` cross-mount E2E (the delete+recreate+create-in-it sequence),
  `fs-mut-smoke`, `probe46` (fstat). 1066/1066 + boot OK.

## Error paths

None user-visible: a cache miss falls through to the real RPC; an OOM cannot occur
(the array is inline, fixed); a NULL Larder degrades to always-miss. A gen-guard
skip is a missed fill, not an error.

## Performance

L1c targets the base X-check re-stat storm + fstat redundancy (~1/3 of warm ops;
"a one-entry cache of root's attr alone kills ~1/3 of warm ops"). The full gofmt
cold/warm re-measure vs the LARDER-DESIGN §10 predictions lands at L1f (after the
dentry + page sub-caches). The `attr_*` diagnostics counters expose the hit rate.

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
- **The Larder is uniform across dev9p sessions.** Attr + dentry caching is sound
  for any 9P server (attrs are close-to-open coherent; name-bindings are own-write
  invalidated; for `/net` the attrs are kind-static and the tree is flat). The L1e
  page cache — which caches file CONTENT — will need a gate for a
  non-content-versioned server (e.g. `/net` dynamic reads).
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
