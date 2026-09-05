---
id: sub-kernel-larder
type: sub
title: "The Larder — the guest-side 9P FS cache (attr + dentry + page)"
parent: moc-kernel-ninep
code: [kernel/larder.c, kernel/include/thylacine/larder.h]
audit: hard
guarded-by: [inv-i38]
validated-by: [spec-fs-cache, gate-smp]
locks: [lock-larder-l-lock]
hazards: []
abis: []
design: [docs/LARDER-DESIGN.md, docs/FID-LIFECYCLE-DESIGN.md]
created: 2026-07-31
updated: 2026-07-31
---
## Purpose

The store of FS metadata + data the guest has already fetched over 9P, so
a repeated stat/walk/read is served locally instead of re-hunted (a
larder is a predator's store of provisions). The measured top lever of
the on-device `go build`: 56–90% of a build's FS ops are redundant round
trips (cold reads 83.9%, warm stats 99.5%, walks ≥56%), and a
single-threaded build runs at in-flight depth 1, so each redundant op is
a full serial RTT. The Plan 9 `cfs` / v9fs `cache=loose` convergence
(LWN 1060656: 100% of the kernel devs' 9p build speedup was
metadata/negative-lookup caching).

This dossier owns the MECHANISM (`kernel/larder.c`). The POLICY — every
serve/populate/invalidate/downgrade call site, the write-behind engine
that drives `larder_page_install_own`, the cached-open consumer of the
snapshot readers, and the cacheability latch — lives in
[[sub-kernel-ninep-dev9p]].

**One owner, one key, one lock.** `struct larder` is embedded in
`struct p9_client` (per-session, shared by every Proc/thread resolving
through the mount via the elected reader), keyed by the 9P `qid.path`
(`dataset_id << 32 | ino`, session-unique), protected by one dedicated
near-leaf spinlock ([[lock-larder-l-lock]]). Coherence is close-to-open
keyed on a true content-version: Stratum's `si_cvers` (bumped on every
content mutation, DECOUPLED from the lifecycle `si_gen`), surfaced as
`qid.version` (the L1a Stratum-side foundation).

## Contract

All entry points are NULL-defensive (a NULL Larder is a miss / no-op);
every serve copies its result out UNDER the lock. `larder_init` zeroes +
inits the lock; `larder_destroy` (from `p9_client_destroy`) frees every
lazily-allocated array + page buffer.

**Attr** (`qid.path → {t_stat, cvers, perm_only}`):
- `larder_attr_serve` — Read: hit copies the attr out + returns true (no
  cvers re-check — the deliberate L1c posture, see Mechanism); miss hands
  back the gen snapshot for the caller's install guard. A `perm_only`
  entry is a miss here (its consumers read exactly the staled fields).
- `larder_attr_fresh_size` — the task-#44 EOF serve: size iff the entry's
  `cvers` equals the reading fid's open-time `qid.vers` (cvers-GATED,
  unlike `attr_serve`); lets a plain-file read at `offset >= size` answer
  0 RPC-free.
- `larder_attr_install` — Open/Refetch: install `{attr, cvers}` iff no
  invalidation event named this qid since `seq0` (the G4-scoped gen
  guard); upgrades a `perm_only` entry to full.
- `larder_attr_invalidate` — OwnWrite: drop the entry + log a HARD event
  (always, even when nothing was cached — the concurrent-populate guard).
- `larder_attr_downgrade` — G3: mark a PARENT dir's entry `perm_only`
  (a child create/unlink/rename stales size/mtime/nlink/cvers but CANNOT
  touch mode/uid/gid — only a wstat on the dir does, and that path
  full-drops); logs a SOFT event.
- `larder_gen_snapshot` — capture the gen for a populate site that does
  not serve first (the walk_attrs free-populate).

**Dentry** (`(parent-qid.path, name[0..name_len)) → child | ENOENT`):
- `larder_walk_serve` — serve a whole resolver run under ONE lock hold:
  chain the dentries, fill each positive hop's `sts[i]` from the attr
  sub-cache (an attr miss mid-chain bails to the RPC), stop at a
  NEGATIVE hop (the walk's miss, a partial prefix). A `perm_only` attr
  serves an INTERMEDIATE hop (the X-check reads mode/uid/gid + immutable
  qid fields, fresh by construction) but not the leaf — unless the
  caller passes `leaf_perm_only` (the G2 bind-form dir-fid consume, which
  reads only those fields) to receive it as a flag instead of a bail.
- `larder_dentry_install` — positive or negative, gen-guarded on the
  PARENT qid; a component longer than `LARDER_DENTRY_NAME_MAX` (88) is
  simply not cached (fail-safe to the RPC).
- `larder_dentry_invalidate_name` — drop ONLY the mutated `(parent,
  name)` binding (siblings preserved — creating `foo` cannot change
  whether `bar` exists); O(1) via the serve's hash; logs a SOFT event
  keyed by the parent. The dentry cache's SOLE coherence mechanism.
- `larder_dentry_lookup` — G2: read-only positive lookup (no CLOCK
  touch), for the rmdir/rename-replace victim resolution by name.
- `larder_qid_staled_since` — G2: has any HARD event named this qid since
  `seq0`? Fail-safe TRUE on ring overflow or a NULL Larder. The dir-fid
  donate gate (a fid tracks the INODE; only identity-death kills it).

**Page** (`(qid.path, page_index) → {bytes[0..valid_len), cvers, own}`):
- `larder_page_serve` — one page per call (bounds the under-lock copy at
  4 KiB; a short serve is a legal short read the caller loops on). Serves
  iff `cvers == want_cvers` OR the page is `own`, and `page_off <
  valid_len`; a partial page misses beyond `valid_len` so no EOF
  determination is ever made from a page.
- `larder_page_install` — gen-guarded install of `[0, len)` from the
  page's ALIGNED start (never a hole); lazily kmallocs the slot's 4 KiB
  buffer; clears `own` (a read-populate observed SERVER content).
- `larder_page_install_own` — G1 write-populate at the wb flush: no cvers
  (`own` serves ungated under the loose single-writer premise), NO gen
  guard (the bytes are the writer's just-landed content and the wb flush
  freeze excludes same-file mutators); `page_off > 0` EXTENDS only an
  existing OWN page whose `valid_len == page_off` (the append-chain
  continuation) — any other shape is refused.
- `larder_page_invalidate` — drop EVERY page of the file, O(pages-of-file)
  via the qid-only secondary hash; SOFT event.
- `larder_page_invalidate_range` — G1b: drop only `[first_idx, last_idx]`
  (the write-through discipline — a ~100-byte buildid pwrite must not
  nuke a just-populated archive); same event logging.

**Cached-open pure readers** (FID-LIFECYCLE; mutate nothing beyond the
CLOCK ref bit):
- `larder_pages_cover` — is `[0, size)` fully resident at `cvers` (own
  pages count), each page holding content to its needed boundary?
- `larder_pages_snapshot` — verify coverage AND copy `[0, size)` out
  under ONE lock hold, FAIL-CLOSED if any invalidation event named this
  qid since the caller's pre-decision `seq0` (the B1 gen witness — see
  Prosecution). The snapshot IS the `Open` linearization for the fidless
  cached-open path.

## Mechanism

### The shared O(1) index shape

All three sub-caches use the same index (the task-#25 page-cache pattern,
generalized at the FID-LIFECYCLE re-size): a HEAP entry array lazily
allocated on the first install (a non-cacheable client — netd — allocates
NOTHING), a chained hash (pow2 buckets ≈ 2×cap, intrusive `hnext`), a
free-cursor for the fill phase, and CLOCK second-chance eviction once
full (`ref` set on serve/install; the hand clears-and-advances; bounded
2×cap with a take-current fallback). The install slot chooser is
overwrite-existing (revalidate-by-overwrite, key/bucket kept) > free
cursor > CLOCK victim (unlinked from its OLD key's bucket before the
re-key, all under one lock hold — no serve can observe a
linked-but-wrong-key intermediate). The load-bearing index invariant:
**a valid slot is linked in the bucket of its CURRENT key** — and for
pages, **a slot is in `page_qhash` IFF it is in `page_hash`** (the two
are linked/unlinked in lockstep at every install / victim-reuse /
buffer-OOM / invalidate site).

The page cache carries the SECONDARY index (`page_qhash`, keyed by
`qid.path` alone, intrusive `qnext`): every page of one file chains into
one qbucket, so an own-write invalidate walks O(pages-of-file + qbucket
collisions), never O(cap) — the task-#29 F3 fix that makes the 32768-slot
cap affordable on the write-heavy cold path. The invalidate walk is a
textbook remove-while-iterating splice; a `!valid` same-file slot (a
fresh install mid-flight) is left linked — the gen bump makes its
install's guard skip, and CLOCK reclaims it later.

Hashing: Fibonacci-mix of the two-part key into a pow2 bucket
(`larder_bucket2`); the dentry key folds the component bytes through
FNV-1a mixed with the parent qid, and the chain walk full-compares
(byte-exact, length-bounded — names are NOT NUL-terminated).

### The gen ring (the populate guard, G4-scoped + G2 kind-tagged)

The spec's `Open` reads the current cvers and installs ATOMICALLY; the
impl reads via an RPC and installs later — a window in which a concurrent
own-write could commit + invalidate. Installing the stale RPC result
would RESURRECT a value the invalidate already dropped (unbounded
staleness — the I-38 violation class). So: a monotonic `gen` bumped by
EVERY invalidation event, captured BEFORE the RPC (`seq0`), re-checked at
install; a raced populate is skipped (a harmless missed fill).

G4 scopes the guard per-file: each event logs its staled qid in the
128-slot `inval_qid` ring (slot = seq % ring); an install skips IFF its
OWN key appears among the events in `(seq0, gen]`. A window wider than
the ring loses evidence and fail-safes to the pre-G4 global skip.
Ring-log soundness: for a window n ≤ ring, slot `s % ring` (s in the
window) was last written by seq s itself — a later same-residue writer
would exceed gen. Pre-G4 the global guard discarded 726–886 unrelated
fills per S3 cold-build window.

G2 tags each event HARD (`larder_attr_invalidate` — the identity-death
class: rmdir/rename victims, create at a reused ino, wstat) or SOFT
(downgrade / dentry / page events — metadata staled, the inode lives).
The install guards scan ALL events; the dir-fid donate gate
(`larder_qid_staled_since`) scans HARD only — a by-name op downgrades its
own parent on every use, so a soft-inclusive gate would block exactly the
recycle it exists to permit.

**Event-logging completeness is the soundness obligation** (see
Prosecution): every mutation must log every qid whose cached state it
stales — create: parent (downgrade + dentry) + child (attr + pages);
unlink/rename: parent(s) + the resolved victim's attr; write-through /
flush / OTRUNC: the file; wstat: the file. Dentry installs key their
guard on the PARENT (all dentry-staling events log the parent);
attr/page installs + the snapshot witness key on the file.

### Per-sub-cache semantics (the deliberate asymmetries)

- **The attr serve is a `Read`, not an `Open`** — no cvers re-check on a
  hit. That is the RPC-elision win; coherence comes from own-write
  invalidation + walk_attrs re-populating fresh on every resolution.
  `NoWrongRead` is absolute in the single-writer regime. The cvers gate
  becomes load-bearing where a serve REPLACES a revalidation:
  `larder_attr_fresh_size` (EOF), the page serve, and the cached-open
  coverage check are all cvers-gated against the reading fid's open-time
  `qid.vers`.
- **The dentry cache has NO version and NO cvers gate** (the L1d
  ground-truth correction): a name→child binding is a fact about the
  PARENT's dirent set, and Stratum surfaces no directory-content version
  that tracks a dirent change (a child create/unlink never runs
  `stm_inode_set` on the parent, so the parent `si_cvers` does not bump;
  only rename stamps it). A parent-cvers gate would FALSELY MATCH a stale
  negative dentry after a create. Sole coherence: name-specific own-write
  invalidation. Negative entries (`→ ENOENT`) are first-class — the
  failed-lookup-storm win.
- **Own pages bypass the cvers gate** (G1): no post-flush cvers is
  knowable client-side (Rwrite carries none); under the single-writer
  premise the flushed bytes ARE the current content. A read-populate over
  the same key upgrades the page back to cvers-gated.

## Data structures

`struct larder` (embedded in `p9_client`): the leaf lock; `gen` + the
`inval_qid[128]` / `inval_hard[128]` ring; three heap entry arrays with
their hash/qhash bucket arrays, caps, free-cursors, and CLOCK hands; ~30
diagnostic counters (hits/misses/installs/install_skips/invalidations/
downgrades/evictions per sub-cache, `inval_scope_passes`,
`page_own_installs`, `co_snapshots`/`co_misses`). Not load-bearing; read
by the tests and the perf instruments.

Entries: `larder_attr_ent` (qid_path, cvers, valid/ref/perm_only, hnext,
inline 88-byte `t_stat`), `larder_dentry_ent` (parent qid, child qid,
name_len, valid/negative/ref, hnext, inline 88-byte name),
`larder_page_ent` (qid_path, page_index, cvers, valid_len, valid/ref/own,
hnext, qnext, heap 4 KiB `page` buffer — lazily kmalloc'd, REUSED across
evictions, freed only at destroy). The `valid` bit — not a `qid_path ==
0` sentinel — marks emptiness: root's `qid.path` is legitimately 0.

Capacities (all tunable; the sizing history is load-bearing context):
- `LARDER_ATTR_ENTRIES` / `LARDER_DENTRY_ENTRIES` = **4096** (born 256
  inline/linear at L1c/L1d; the FID-LIFECYCLE re-size moved both to
  heap + O(1) hash because the cached-open hint expanded the metadata
  working set to every opened file — at 256 slots 86% of cached-open
  attempts died on the evicted metadata chain).
- `LARDER_PAGE_ENTRIES` = **32768** (born 512 linear at L1e; 8192 at the
  task-#25 O(1) rewrite; 32768 at task #29). The task-#29 measurement:
  8192 (32 MiB) THRASHED — a Go build reads package archives
  SEQUENTIALLY (LRU-hostile scan, not a Zipf hot-subset), so the cache
  helps only once it holds the WHOLE working set (build2 ~20k pages,
  gofmt-cold ~27k — the knee); at 32768 evictions hit 0 and the win
  COMPOUNDED (reads hit AND cached-open coverage survives to the next
  open). 128 MiB lazy CEILING per cacheable client — see
  [[seam-larder-shrinker]].
- `LARDER_INVAL_RING` = 128 (a 4-CPU write burst over a ~100–200 µs RPC
  with wide margin; overflow only costs a skipped fill).
- `LARDER_PAGE_SIZE` = 4096 (aligns with the buddy/demand-page system and
  Go's page-aligned archive reads).

## Concurrency

Every entry point is a single lock/unlock pair on the one leaf lock — no
mid-op release, no cross-op held state ([[lock-larder-l-lock]]: only the
buddy zone lock ever nests below, via the non-blocking kmalloc/kfree;
never held with `c->lock`; never across an RPC). The two SMP disciplines
that make the shared-client exposure sound:

1. **Serve/invalidate atomicity** — a serve copies the whole entry (or
   the page bytes, or the entire snapshot) out under the lock; an
   invalidate/evict/destroy mutates under it. A serve reads a whole
   entry valid at its linearization point, or misses — never torn, never
   a freed buffer mid-copy (buffers are reused on evict, freed only at
   destroy, and an evicted slot's `valid_len` is rewritten before it can
   serve — stale tail bytes are unreachable).
2. **The gen guard** realizes the spec's atomic `Open` across the
   RPC-shaped populate window (above).

`larder_destroy` runs from `p9_client_destroy` — no op in flight (the
last attached ref dropped), the lock is defensive. Memory ordering: none
beyond the lock — the `p9_client.cacheable` gate that admits callers is
the policy side's RELAXED flag (sound because the lock orders the cache
content; the flag only gates whether a caller tries).

## Invariants enforced

![[inv-i38#Statement]]

The mechanism half: serve-copy-under-lock + the gen-ring guard + the
cvers/own gates are what make a hit "exactly what a fresh RPC would
return" under close-to-open; the bounded caps + CLOCK are the I-32
resource floor (no unbounded growth from a hostile workload; every
alloc-failure path degrades to a pure miss — I-38 correctness NEVER
depends on a fill). [[spec-fs-cache]] models the discipline on content
tokens; the byte-granular arithmetic (alignment, valid_len, the overlay
split) is beneath the model and pinned by the kernel tests.

## Error paths

None user-visible: a miss falls through to the real RPC; an OOM on the
lazy array or a page buffer skips the install (best-effort — the RPC
already served the bytes; re-attempted on the next install, so a
fragmented-buddy failure self-heals); a gen-guard skip is a missed fill,
not an error. `larder_pages_snapshot` may partially scribble its output
buffer on a failed run — the caller discards it. The order-9 contiguous
entry-array allocations are the one fragility class
([[seam-larder-lazy-array-robustness]]).

## Performance

The counters expose hit rates per sub-cache. Measured effect along the
arc (the series lives in [[msr-gofmt-warm]]): the warm gofmt build
1352 → 1147 ms at L1f (−15%; the re-measure ground-truthed the residual
as ~86% fixed go-tool overhead, not FS redundancy); the S1 warm scenario
439 → 367 ms at B1, → 249 ms at D44 (with the aligned-read + attr-EOF
policy fixes), → ~195 ms at G1. Costs: a hit is a bounded copy under a
spinlock (≤ 88 B attr, ≤ 4 KiB page, ≤ 128 KiB cached-open snapshot —
the upstream `DEV9P_CO_MAX_SIZE` cap keeps the under-lock copy bounded);
the first install per sub-cache builds + zeroes the array under the lock
(a bounded once-per-client spike — the same seam); populated footprint
~0.9 MiB metadata per client + up to 128 MiB of lazy page buffers.

## Prosecution

- **The gen-ring event-logging completeness**: every NEW mutation path
  must log every qid whose cached state it stales, with the right
  HARD/SOFT kind. A missed log reopens the populate-after-invalidate
  resurrection (unbounded staleness); a HARD where SOFT belongs blocks
  the dir-fid recycle; a SOFT where HARD belongs re-parks a fid for a
  dead inode.
- **The B1 third-actor gen witness**: the two-party "invalidate precedes
  or follows the snapshot" argument is INCOMPLETE — a third actor
  holding a fid opened pre-write can repopulate just-invalidated pages
  with POST-write bytes tagged the OLD cvers (the populate tags with the
  reading fid's open-time vers), re-satisfying coverage and minting a
  torn snapshot no fresh RPC could return. `larder_pages_snapshot`'s
  `seq0` check under the same lock hold as the copy closes it; any new
  coverage-shaped consumer needs the same witness.
- **The index invariants**: valid ⟹ linked under the CURRENT key; pages
  in both hashes or neither (every link/unlink site in lockstep); the
  evict unlink-before-rekey under one hold; CLOCK termination (ref bits
  only cleared under the lock).
- **The G3 field-freshness argument**: `perm_only` may serve ONLY
  consumers that read mode/uid/gid + immutable qid identity (the
  resolver's intermediate X-check; the G2 bind-consume leaf via the
  explicit out-flag). Every new `perm_only`-serving consumer must
  re-prove its field set; every new field a consumer reads re-opens the
  question.
- **The own-page rules**: `install_own` only from the wb flush's
  full-land arm (the `err == 0` coupling —
  `fs_cache_buggy_populate_unflushed` is the counterexample cfg); the
  append-chain extend only onto an OWN page ending exactly at the new
  start (never a hole, never a mixed cvers/own page); a read-populate
  clears `own`.
- **No EOF from pages**: a short page serves `[0, valid_len)` and misses
  beyond; EOF determination comes only from the wire or the cvers-gated
  fresh attr size.
- **The serve-copy-under-lock rule**: any future pin-and-copy-outside-
  lock optimization must bring the #847-style refcount discipline with
  it; today the lock hold IS the lifetime argument.

## Seams

- [[seam-larder-shrinker]] — the 128 MiB/client no-reclaim page ceiling;
  stewardship-flagged for any constrained-RAM bring-up (RPi4/Lazarus).
- [[seam-larder-loom-bypass]] — Loom async FS mutations drive
  `p9_client_*` directly and invalidate nothing (self-inflicted-
  reachable only; no v1.0 consumer mixes them).
- [[seam-larder-stale-child-attr]] — rename/unlink leave the moved/
  unlinked file's OWN attr (stale ctime/nlink on a held-open fstat;
  metadata-only, no perm consequence).
- [[seam-larder-reused-dir-dentries]] — a reused DIR qid's cached
  CHILDREN dentries survive rmdir+reuse (nothing is keyed on the dead
  qid-as-parent at the drop sites).
- [[seam-larder-lazy-array-robustness]] — the order-9 contiguous
  entry-array allocs (fail → pure miss, self-heals) + the
  build-under-lock first-install spike.
- [[seam-larder-cacheable-proxy]] — the POUNCE-success latch is a proxy
  for "content-versioned + offset-stable"; a POUNCE-speaking-but-
  streaming server needs an explicit attach-time capability.

## Caveats

- **Single-writer premise**: v1.0 coherence is own-write-strong,
  external-writer-bounded (close-to-open — an out-of-band Stratum
  mutation is caught at the next open-time revalidation; a stale DENTRY
  additionally has no revalidation hook at all and is bounded only by
  LRU or the guest's own next mutation of that directory — the
  parent-cvers tightening is deliberately unpaid at v1.0, a parent-COW
  cost on the create-heavy build path).
- **Never populate from a readdir qid** (the L1a-2 Stratum-round rule):
  Rreaddir's `qid.version` is a link-time `si_gen` snapshot stored in
  the dirent record, not `si_cvers` — a readdir-sourced version would
  read backwards against a getattr-sourced one. The guest side simply
  never installs from readdir; the v1.x directory-listing cache must
  bring its own per-child revalidation design.
- **`qid.path` truncates ino to 32 bits** — a low-32 alias would share a
  cache key; infeasible at v1.0 (no pool has 4-billion-inode caps),
  recorded as the v2.0-ino-cap bound.
- **Perf refinements deliberately not taken**: single-page-per-serve
  (a fully-cached 128 KiB read is 32 fast serves), copy-out under the
  lock (vs pin-and-copy — the v1.x SMP-scaling refinement), the
  unaligned partial-front page never cached (Go's reads are
  page-aligned; the D44 aligned-read policy heals the sequential-stream
  case at the dev9p layer).
- **The Stratum parent-mtime gap** (cross-tree, tracked there): a child
  create/unlink does not update the parent dir's mtime/ctime (only
  rename does) — a latent POSIX-compliance gap that does NOT affect
  Larder soundness (own-write invalidation is independent of parent
  cvers) but is exactly why the dentry cache can have no cvers gate.
- Persistent / cross-session / cross-mount caching: out of scope by
  design (in-memory per-session; Stratum's own dcache is the shared
  server tier).

## Provenance

(generated from incoming `touched` edges — the shaping chunks:
[[chg-2026-07-09-larder-l1c]] (scripture + spec + substrate/attr),
[[chg-2026-07-09-larder-l1d]] (dentry), [[chg-2026-07-09-larder-l1e]]
(page + gate), [[chg-2026-07-09-larder-l1f]] (arc close),
[[chg-2026-07-11-fid-lifecycle]] (heap re-size + O(1) index + qhash),
[[chg-2026-07-11-b1-loose]] (the gen witness),
[[chg-2026-07-11-d44-read-band]] (fresh_size),
[[chg-2026-07-12-term2-dentry-name]] (name-specific invalidation),
[[chg-2026-07-13-g1-write-populate]] (install_own + range invalidate),
[[chg-2026-07-13-g34-downgrade-genscope]] (perm_only + the qid-scoped
ring), [[chg-2026-07-13-g2-dirfid]] (lookup / staled_since /
leaf_perm_only / HARD-SOFT tagging), [[chg-2026-07-14-term4-close]].)

## Tests

`kernel/test/test_larder.c` — 32 registered `larder.*` cases driving the
mechanism directly: the attr battery (`install_serve` / `serve_miss` /
`invalidate` / `gen_guard_skips_raced_install` [the resurrection close] /
`root_qid_zero` / `overwrite_wins` / `eviction_bounded`), the dentry
battery (`dentry_serve` / `dentry_serve_miss` / `dentry_negative` /
`dentry_multi_hop` / `dentry_partial_chain_bails` /
`dentry_attr_miss_bails` / `dentry_invalidate_name` [drops the named
binding AND preserves a sibling — fails on the retired whole-parent
drop] / `dentry_gen_guard` / `dentry_name_too_long` / `dentry_bounded`),
the page battery (`page_serve` / `page_serve_miss` / `page_offset` /
`page_cvers_mismatch` / `page_partial` / `page_invalidate` /
`page_invalidate_multifile` [the two-index lockstep + O(pages-of-file)
discrimination proof] / `page_gen_guard` / `page_overwrite` /
`page_bounded` / `page_destroy_frees`), the term-4 additions
(`attr_downgrade_perm_only` / `downgrade_guards_raced_populate` /
`gen_scope_qid` [own-key skips, unrelated admits, overflow fail-safes]),
and `pages_snapshot_gen_witness` (the B1 F1 interleave: coverage
re-satisfied by a third actor, the witness fails it, a fresh capture
serves). The policy-level integration proofs (`dev9p.page_cache_serve_
and_gate`, the create-reuse pair, the wb-populate battery — all
revert-probed non-vacuous) live with [[sub-kernel-ninep-dev9p]]. The SMP
witness: the L1f full gate (40/40, 0 corruption — the boot chain's
shared-client stat/walk/read storm under smp4/smp8 × default/UBSan) +
every later gate on this surface ([[gate-smp]]).
