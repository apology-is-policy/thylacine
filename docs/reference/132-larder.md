# 132 — The Larder (guest-side 9P FS cache)

**Status:** L1c LANDED — the substrate + the **attr** sub-cache. The dentry (L1d)
and page (L1e) sub-caches extend the same owner/lock/key and are not yet built.

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
```

All four are NULL-defensive (a NULL Larder is a miss / no-op). `larder_attr_serve`
copies the whole `t_stat` out under the lock — a concurrent invalidate can never
tear a serve.

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

- `kernel/test/test_larder.c` — `larder.install_serve` / `serve_miss` /
  `invalidate` / `gen_guard` (the resurrection close) / `root_qid_zero` /
  `overwrite_wins` / `eviction_bounded` (I-32).
- `kernel/test/test_dev9p.c::test_dev9p_create_invalidates_reused_child` — the
  create-reuse regression (non-vacuous: `1054/1055 FAIL` with the child-invalidate
  reverted).
- In-guest: `stalk-2` cross-mount E2E (the delete+recreate+create-in-it sequence
  that surfaced the reuse bug), `fs-mut-smoke` (21 checks), `probe46` (fstat).
  1055/1055 + boot OK.

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
- **The Larder is uniform across dev9p sessions.** Attr caching is sound for any
  9P server (attrs are close-to-open coherent; for `/net` the attrs are
  kind-static). The L1e page cache — which caches file CONTENT — will need a gate
  for a non-content-versioned server (e.g. `/net` dynamic reads).
