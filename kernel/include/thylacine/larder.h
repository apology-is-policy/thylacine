#ifndef THYLACINE_LARDER_H
#define THYLACINE_LARDER_H

// The Larder -- the guest-side 9P FS cache (docs/LARDER-DESIGN.md; ARCH section
// 28 invariant I-38). A thylacine's larder is the store of provisions it returns
// to instead of re-hunting; this cache is the store of FS metadata + data the
// guest has already fetched, so a repeated stat/walk/read is served locally
// instead of re-hunted over 9P. The measured top lever of the on-device go-build
// (56-90% of a build's FS ops are redundant round trips).
//
// L1c lands the SUBSTRATE + the ATTR sub-cache (this file). The dentry (L1d) and
// page (L1e) sub-caches extend the same owner + lock + key.
//
// One owner, one key. The Larder lives on `struct p9_client` (the per-session 9P
// client, shared by every Proc/thread resolving through the mount via the #841
// elected reader), keyed by the 9P `qid.path` (dataset<<32 | ino, session-unique).
// A dedicated near-leaf lock protects it (never held across a blocking RPC -- the
// #360 lock-across-sleep rule); it composes with, but is distinct from, c->lock,
// and the two are never held together (an RPC that takes c->lock happens OUTSIDE
// the Larder lock).
//
// Coherence is close-to-open, keyed on the true content-version `cvers` (Stratum
// `si_cvers`, surfaced as `qid.version` -- L1a). It maps to specs/fs_cache.tla:
//
//   larder_attr_serve  = Read       -- serve a cached attr WITHOUT re-fetching
//                                       cvers (the win). NoWrongRead is absolute
//                                       in the single-writer regime BECAUSE own
//                                       writes invalidate (below).
//   larder_attr_install = Open/Refetch -- install {attr, cvers} from a fresh
//                                       getattr/walk_attrs reply. The install is
//                                       GEN-GUARDED (see below) to realize the
//                                       spec's ATOMIC read-and-install.
//   larder_attr_invalidate = OwnWrite -- a guest write/chmod/create/rename/unlink
//                                       drops the affected entry (write-through),
//                                       so a subsequent Read cannot serve the
//                                       guest's own stale content.
//
// The load-bearing SMP property (LARDER-DESIGN section 7) is the invalidate-vs-
// serve race on the shared client. Two disciplines close it:
//
//   (1) Serve/invalidate atomicity. serve copies the cached t_stat out UNDER the
//       lock; invalidate drops the entry UNDER the lock. A serve therefore reads
//       a whole entry that was valid at its linearization point, or misses -- an
//       invalidate can never tear a concurrent serve.
//
//   (2) The populate GEN guard. The spec's Open reads the current cvers and
//       installs ATOMICALLY. The impl reads the cvers via an RPC (getattr /
//       walk_attrs) and installs LATER -- a non-atomic window in which a
//       concurrent own-write could commit a higher cvers AND invalidate the
//       entry. Installing the stale RPC result then would RESURRECT a value the
//       write's invalidate already dropped, and a later causal reader would serve
//       it (an unbounded staleness -> an I-38/I-28 violation). The Larder carries
//       a monotonic `gen` bumped on EVERY invalidate; a populate captures `gen`
//       BEFORE its RPC (larder_attr_serve's miss out-param / larder_gen_snapshot)
//       and installs only if `gen` is unchanged. A populate that raced an
//       invalidate is skipped (a harmless missed fill -- the next access
//       refetches), so the install linearizes cleanly and the spec's atomic Open
//       is realized. `gen` is global (any invalidate blocks any concurrent
//       populate); over-conservative but sound, and the target workload is
//       in-flight depth 1 (serial RPCs) so skips are rare. A per-key gen is a
//       v1.x refinement if a concurrent workload ever thrashes the fill.
//
// Bounded (I-32 / I-38): a fixed-capacity inline array, LRU-evicted. No unbounded
// growth from a hostile workload.

#include <thylacine/types.h>
#include <thylacine/spinlock.h>
#include <thylacine/syscall.h>   // struct t_stat

// Attr + dentry capacity (FID-LIFECYCLE re-size, 2026-07-10). 256 covered the
// L1c target (the base X-check dir set, ~7 hot dirs + fstat locality). The
// cached-open hint (docs/FID-LIFECYCLE-DESIGN.md section 3.3) expands the
// metadata working set to EVERY opened file -- a warm go build touches ~800+
// distinct leaves, and at 256 slots the leaf entries evict before re-open
// (measured: 86% of cached-open attempts died on the metadata chain; the #343
// dcache lesson -- cross the working-set threshold or get nothing). 4096 holds
// the build working set. Both arrays are HEAP (lazily allocated on first
// install, the task-#25 page-array pattern) with an O(1) chained hash + a
// free-cursor + CLOCK eviction -- a linear scan at 4096 would cost more than
// the RPCs it saves, and heap-lazy means a non-cacheable client (netd)
// allocates neither. ~0.9 MiB/client when populated. Tunable.
#define LARDER_ATTR_ENTRIES 4096u

// Dentry-cache capacity + the inline component-name bound. Sized with the attr
// cache (each opened leaf needs BOTH its dentry and its attr resident for the
// cached-open hint chain). NAME_MAX 88 covers a go-cache content-hash file name
// (a 64-hex hash + suffix ~66 B); a longer component is simply not cached (the
// serve/populate fail-safe to the RPC -- correctness is unaffected, only that
// walk isn't elided).
#define LARDER_DENTRY_ENTRIES  4096u
#define LARDER_DENTRY_NAME_MAX 88u

// Page-cache capacity + unit (L1e). Each slot caches one LARDER_PAGE_SIZE window of
// a file's content, keyed (qid.path, page_index = file offset / LARDER_PAGE_SIZE).
// The unit is one 4 KiB page (aligns with the buddy/demand-page system and Go's
// page-aligned .a reads). LARDER_PAGE_ENTRIES slots x 4 KiB = the per-client page
// budget. BOTH the slot buffers AND the slot-entry ARRAY are heap: the array is
// lazily allocated on the first page install (so a stream/control server -- netd,
// never cacheable -- allocates NEITHER the array NOR any buffer), and the buffers
// are lazily kmalloc'd per slot, reused across evictions, freed at larder_destroy.
// 32768 slots = 128 MiB CEILING (lazy: only touched pages allocate a 4 KiB buffer, so a
// light client pays ~1.7 MiB metadata + only its live pages; a stream/control client --
// netd -- pays nothing). Measured 2026-07-10 (task #29, the cached-open re-measure): 8192
// (32 MiB) THRASHED the go-build read working set -- build2-warm pe=12119 evictions,
// gofmt-cold pe=18733 -- because a Go build reads the package archives SEQUENTIALLY (an
// LRU-hostile scan, not the Zipf hot-subset the 8192 note assumed): the cache helps only
// once it holds the WHOLE working set. Sizing to hold it (build2 ~20k pages, gofmt-cold
// ~27k) drove pe -> 0 on every window, a COMPOUND win: (a) reads hit (build2 read RTs
// 2489 -> 982) AND (b) cached-open COVERAGE now passes (pages stay resident to the next
// open) so MINTED 3.6x (396 -> 1437) + hcover -88% (1185 -> 144), collapsing the fid
// lifecycle too. Warm floor build2 605 -> 510 ms, gofmt 805 -> 653 ms (both ~-16-19%).
// The knee is ~27k (gofmt-cold); 32768 is the pow2 above it with margin. A real project
// larger than gofmt re-thrashes -> a memory-pressure-adaptive cap (a shrinker) is the
// v1.x design (Thylacine has no reclaim framework yet); the buffers stay resident until
// larder_destroy (mount teardown) -- the v1.0 no-reclaim window, bounded per client
// (~2-3 cacheable FS-mount clients at v1.0 -> ~256-384 MiB peak on the 2 GiB dev target,
// ~13-19%). STEWARDSHIP (task-#29 audit F1): on a small-RAM target (RPi4 / Lazarus) the
// 128 MiB x client-count makes the absent shrinker load-bearing -- land the adaptive cap
// (or shrink the constant) before a constrained-RAM bring-up.
// Task-#25 audit P3 dispositions (all correctness-safe):
//   F1: the entry array is 32768*sizeof(ent) ~= 1.7 MiB -> alloc_pages(order 9) = a 2-MiB
//       CONTIGUOUS block. Under a fragmented buddy that alloc can fail -> page_ensure
//       returns false -> the client silently serves as a pure miss (I-38 holds; NO
//       correctness impact) and RE-ATTEMPTS on the next install, so it SELF-HEALS when
//       fragmentation clears. A chunked/non-contiguous entry pool is the v1.x robustness
//       fix if a large-cap contiguous alloc ever proves to disable the cache in practice.
//   F2: the one-time first-install allocates + zeroes the array + inits both hashes UNDER
//       l->lock (preempt-off) -- a bounded spike, once per cacheable client (~2-3 at v1.0).
//       Double-checked-locking publish outside the lock is the v1.x cleanup.
//   F3 CLOSED (task #29): invalidate WAS O(page_cap) -- at 32768 that is a 32k-slot locked
//       scan per own-write (gofmt-cold write=5896 -> ~193M scans). Fixed with a SECONDARY
//       index keyed by qid_path alone (page_qhash + the qnext chain): every page of one
//       file shares one qbucket, so larder_page_invalidate walks only that file's pages
//       (+ hash collisions) -- O(pages-of-file), independent of cap. This is what makes
//       the large cap affordable for the write-heavy cold path.
#define LARDER_PAGE_ENTRIES 32768u
#define LARDER_PAGE_SIZE    4096u

// G4 (term-4): the qid-scoped populate guard's invalidation ring log. Every
// invalidation event (attr/dentry/page, any form) bumps `gen` AND records the
// staled qid at inval_qid[gen % RING]. An install with pre-RPC snapshot seq0
// skips IFF its own key qid appears among the events in (seq0, gen] -- scanning
// at most RING slots; a window wider than the ring loses evidence, so it
// fail-safes to the pre-G4 global skip. Ring-log soundness: for any seq s <=
// gen, slot s % RING holds the qid of the LATEST event with that residue <= gen,
// so when gen - seq0 <= RING every in-window slot is exactly its in-window
// event (a later same-residue writer would exceed gen). Sizing: one in-flight
// RPC window on the shared client sees the concurrent writers' events (a
// create logs 4; a flush logs 2); 128 covers a 4-CPU write burst over a
// ~100-200us RPC with wide margin, and overflow only costs a skipped fill.
#define LARDER_INVAL_RING 128u

struct larder_attr_ent {
    u64            qid_path;   // key. root's qid.path is 0x0, so `valid` (not
                               //   path==0) marks an empty slot.
    u32            cvers;      // content-version (qid.version == si_cvers) at fetch;
                               //   consumed by the L1e page revalidation.
    bool           valid;      // slot occupied
    bool           ref;        // CLOCK second-chance bit (set on serve/install)
    bool           perm_only;  // G3 (term-4): a child-mutation in this DIR staled
                               //   size/mtime/nlink/cvers but NOT mode/uid/gid (a
                               //   create/unlink/rename never edits the parent's
                               //   perm bits -- only wstat does, and wstat full-
                               //   drops). A perm_only entry serves ONLY the
                               //   resolver's intermediate-hop X-check (which
                               //   reads mode/uid/gid + the immutable qid.path/
                               //   type); attr_serve / fresh_size / a walk LEAF
                               //   reject it (their consumers read size/times/
                               //   cvers). Any full install clears it.
    s32            hnext;      // hash chain: next slot in this key's bucket, -1 = end
    struct t_stat  attr;       // cached metadata (mode/uid/gid/size/times/nlink/...)
};

// A cached name->child binding (the Plan 9 dentry). Keyed by (parent_qid_path,
// name[0..name_len)). A NEGATIVE entry caches "name does not exist in parent"
// (the failed-lookup-storm win). NO content-version field: a name-binding is a
// PARENT-dirent fact, and Stratum surfaces no directory-content version that
// tracks a dirent change (a child create/unlink does not bump the parent's
// si_cvers -- verified src/fs/fs.c; only rename stamps the parent mtime), so the
// dentry cache is NOT cvers-gated -- its sole coherence is own-write invalidation
// (larder_dentry_invalidate_name on a create/rename/unlink -- the mutated name
// only, siblings preserved; LARDER-DESIGN §4).
// A positive entry's reply attr is served from the attr sub-cache (the child's
// qid.path keys it), so the dentry stores only the linkage, not the attr.
struct larder_dentry_ent {
    u64  parent_qid_path;   // key part 1
    u64  child_qid_path;    // resolved child (positive only; 0 when negative)
    u32  name_len;          // key part 2: name[0..name_len)
    bool valid;             // slot occupied
    bool negative;          // true: (parent,name) -> ENOENT (no child)
    bool ref;               // CLOCK second-chance bit (set on serve/install)
    s32  hnext;             // hash chain: next slot in this key's bucket, -1 = end
    char name[LARDER_DENTRY_NAME_MAX];   // the component (NOT NUL-terminated)
};

// A cached file-content page (L1e). Keyed (qid_path, page_index). Coherence is
// close-to-open, keyed on the file's content-version: a page is served only while
// its `cvers` matches the reader's fid version (Spoor qid.vers), and is dropped on
// the guest's own write to the file (larder_page_invalidate). Bytes [0, valid_len)
// hold content from the page's ALIGNED START -- a read that did not cover the page
// start does not populate it (LARDER-DESIGN section 4), so there is never a hole; a
// serve reads only within [0, valid_len) and misses beyond (falls to the RPC), so
// a partial (small-file / EOF) page is served soundly without an EOF determination.
// `page` is a lazily-allocated heap buffer, reused across evictions, freed only at
// larder_destroy (a slot's buffer outlives the (qid_path,page_index) it caches).
struct larder_page_ent {
    u64  qid_path;     // key part 1
    u64  page_index;   // key part 2: file offset / LARDER_PAGE_SIZE
    u32  cvers;        // content-version (fid qid.vers) at fetch -- the freshness gate
    u32  valid_len;    // bytes [0, valid_len) of `page` hold content (<= LARDER_PAGE_SIZE)
    bool valid;        // slot occupied (buffer may still be allocated when !valid)
    bool ref;          // CLOCK second-chance reference bit (set on serve-hit / install)
    bool own;          // G1 write-populate: the writer's OWN landed bytes; the serve
                       // bypasses the cvers gate (sound under the loose single-writer
                       // premise the wb asserts -- fs_cache.tla EnableFlushPopulate)
    s32  hnext;        // (qid_path,page_index) hash chain: next slot in this bucket, -1 = end
    s32  qnext;        // qid_path-only hash chain (page_qhash): O(pages-of-file) invalidate
    u8  *page;         // heap buffer (lazily kmalloc'd; reused; freed at larder_destroy)
};

struct larder {
    spin_lock_t               lock;       // dedicated near-leaf; never held across an RPC
    u64                       gen;        // monotonic invalidation sequence (populate guard)
    u64                       inval_qid[LARDER_INVAL_RING];  // G4: qid per event seq
                                          //   (slot = seq % RING; see the ring note above)
    // All three sub-cache entry arrays are HEAP (lazily allocated on the first
    // install -- a non-cacheable client allocates none) with the same O(1)
    // index shape: chained hash + free-cursor fill + CLOCK eviction.
    struct larder_attr_ent   *attr;         // heap; lazily allocated on 1st install
    u32                       attr_cap;
    s32                      *attr_hash;
    u32                       attr_nbuckets;
    u32                       attr_used;
    u32                       attr_hand;
    struct larder_dentry_ent *dentry;       // heap; lazily allocated on 1st install
    u32                       dentry_cap;
    s32                      *dentry_hash;
    u32                       dentry_nbuckets;
    u32                       dentry_used;
    u32                       dentry_hand;
    struct larder_page_ent   *page;         // heap; lazily allocated on 1st install
    u32                       page_cap;      // page[] slot count (0 until allocated)
    s32                      *page_hash;     // heap bucket heads (page_nbuckets); -1 empty
    u32                       page_nbuckets; // pow2, ~= 2*page_cap (open-chain hash)
    u32                       page_used;     // free-cursor: next never-used slot (fill phase)
    u32                       page_hand;     // CLOCK hand (eviction, once cap-full)
    s32                      *page_qhash;    // heap qid_path-only bucket heads (page_qnbuckets); -1 empty
    u32                       page_qnbuckets;// pow2; the F3 O(pages-of-file)-invalidate index
    // Diagnostics (hit-rate measurement for the L1f bench; not load-bearing).
    u64 attr_hits;
    u64 attr_misses;
    u64 attr_installs;
    u64 attr_install_skips;   // gen-guard skips (populate raced an invalidate)
    u64 attr_invalidations;
    u64 attr_downgrades;      // G3: full -> perm_only downgrades (child-mutations)
    u64 attr_evictions;
    u64 inval_scope_passes;   // G4: installs the global gen would have skipped but
                              //   the qid-scoped scan admitted (the recovered fills)
    u64 dentry_hits;          // positive dentry serves
    u64 dentry_neg_hits;      // negative dentry serves (walk misses served RPC-free)
    u64 dentry_misses;
    u64 dentry_installs;
    u64 dentry_install_skips; // gen-guard skips
    u64 dentry_invalidations; // entries dropped by invalidate-parent
    u64 dentry_evictions;
    u64 page_hits;            // page serves (RPC-free byte deliveries)
    u64 page_misses;          // page serve misses (miss / stale cvers / out of range)
    u64 page_installs;
    u64 page_install_skips;   // gen-guard skips
    u64 page_own_installs;    // G1 write-populate installs (flush-landed own pages)
    u64 page_invalidations;   // pages dropped by own-write invalidate
    u64 page_evictions;
    u64 co_snapshots;         // cached-open snapshots served (fidless opens minted)
    u64 co_misses;            // cached-open snapshot failures (coverage lost pre-copy)
};

// Initialize an embedded Larder (memset-equivalent + lock init). No teardown --
// the inline array frees with its owning p9_client.
void larder_init(struct larder *l);

// Serve (Read). On a HIT, copy the cached attr for `qid_path` into *out under the
// lock and return true (no RPC). On a MISS, set *seq0_out to the current gen (the
// populate guard for the caller's subsequent larder_attr_install) and return
// false. A perm_only entry (G3) is a MISS here -- the stat consumers read
// size/times, exactly the staled fields; the ensuing getattr re-installs full.
// A NULL Larder / out is a miss (defensive).
bool larder_attr_serve(struct larder *l, u64 qid_path,
                       struct t_stat *out, u64 *seq0_out);

// Capture the current gen for a populate site that does NOT serve first (the
// walk_attrs free-populate). Call BEFORE the RPC; pass the result to
// larder_attr_install.
u64 larder_gen_snapshot(struct larder *l);

// FRESH-attr size read (the EOF serve, task #44). Returns true + *size_out iff a
// cached attr for `qid_path` exists AND its cvers matches `cvers` (the reading
// fid's open-time qid.vers -- the SAME freshness gate as the page serve, unlike
// larder_attr_serve's ungated Read). A read at offset >= *size_out can then
// return 0 (EOF) RPC-free: the fresh size is the open-time size, and with
// own-write invalidation (+ the I-38 single-writer premise) the wire would
// answer the same. Conservative on any mismatch (miss -> the caller wires).
// A perm_only entry is a miss (its size is exactly what the downgrade staled).
bool larder_attr_fresh_size(struct larder *l, u64 qid_path, u32 cvers,
                            u64 *size_out);

// Populate (Open/Refetch). Install {attr, cvers} for `qid_path` -- overwriting an
// existing entry (revalidate-by-overwrite), using a free slot, or evicting the
// LRU entry when full -- IFF no invalidation event named `qid_path` since the
// seq0 snapshot (the G4 qid-scoped gen guard; an over-wide window fail-safes to
// skip). Otherwise skipped. `attr` is copied; a perm_only entry upgrades to full.
void larder_attr_install(struct larder *l, u64 seq0, u64 qid_path,
                         u32 cvers, const struct t_stat *attr);

// Invalidate (OwnWrite). Drop `qid_path`'s cached entry (if present) and log the
// invalidation event (always -- the file was mutated even if it was not cached,
// so a concurrent populate of THIS qid that fetched the pre-mutation value must
// be skipped).
void larder_attr_invalidate(struct larder *l, u64 qid_path);

// Downgrade (G3, term-4): the perm-preserving OwnWrite for a PARENT DIR whose
// child set mutated (create/unlink/rename target the child; the parent's
// size/mtime/nlink/cvers change but its mode/uid/gid CANNOT -- only a wstat on
// the dir itself edits those, and that path full-invalidates). Marks the entry
// perm_only (servable ONLY at a resolver intermediate hop, where the X-check
// reads mode/uid/gid + the immutable qid identity) and logs the invalidation
// event exactly like larder_attr_invalidate (a concurrent full populate of the
// dir that observed pre-mutation times must still be skipped -- the downgrade
// is an invalidation to the gen guard; only the perm-servable core survives).
// Measured motivation (T4-M): the S3 wga band is 5.6x re-walk, am_mid=1104 of
// it mid-hop attr misses minted by exactly these parent-attr drops.
void larder_attr_downgrade(struct larder *l, u64 qid_path);

// -- L1d: the dentry sub-cache --------------------------------------------------

// Serve a whole resolver run (fs_cache.tla Read) from the dentry + attr
// sub-caches WITHOUT an RPC. Chains (base_qid_path, names[0]) -> child0,
// (child0, names[1]) -> child1, ... under ONE lock hold (an atomic snapshot -- a
// concurrent own-write invalidate precedes or follows the whole serve, never
// tears it). Each POSITIVE hop's reply attr is served from the attr sub-cache
// into sts[i] (which carries the child's qid.path/vers/type). Outcomes:
//   - full positive run: returns true, *nresolved = nname, *is_miss = false,
//     sts[0..nname) filled.
//   - a NEGATIVE dentry at hop i: returns true, *nresolved = i, *is_miss = true,
//     sts[0..i) filled (the walk misses at i; the caller returns a partial walk).
//   - a dentry MISS, an attr MISS mid-chain, or a too-long component name:
//     returns false (the caller falls through to the RPC).
// G3: a perm_only attr serves an INTERMEDIATE hop (the X-check reads only
// mode/uid/gid + the immutable qid fields) but NOT the final hop of a full
// positive run -- the LEAF record feeds STALK_STAT's returned stat, the
// carried-attrs chain, and the cached-open size/cvers gates, all of which read
// the staled fields; a perm_only leaf bails to the RPC (which re-installs full).
// The CALLER decides whether to skip the RPC: a full positive run only in the
// QUERY form (no server fid to bind); a cached miss in either form (a miss binds
// nothing). `sts` is the caller's DEV_WALK_ATTRS_MAX array; a false return may
// leave it partially written (the RPC overwrites it). NULL Larder -> false.
bool larder_walk_serve(struct larder *l, u64 base_qid_path,
                       const char *const *names, const size_t *name_lens,
                       int nname, struct t_stat *sts,
                       int *nresolved, bool *is_miss);

// Populate a dentry (Read/OwnWrite install). Install (parent_qid_path, name) ->
// child_qid_path (negative == false) or -> ENOENT (negative == true, child
// ignored) IFF `l->gen == seq0` (no invalidate raced the RPC since the snapshot).
// A name longer than LARDER_DENTRY_NAME_MAX is not cached (a no-op). Overwrite an
// existing (parent,name) / use a free slot / evict the LRU entry when full.
void larder_dentry_install(struct larder *l, u64 seq0, u64 parent_qid_path,
                           const char *name, size_t name_len,
                           u64 child_qid_path, bool negative);

// Invalidate (OwnWrite). Drop ONLY the (parent_qid_path, name) binding -- the one
// dentry a single-name create/rename/unlink stales -- and bump gen (always -- a
// concurrent populate that snapshotted gen pre-mutation must be skipped). Siblings
// under the same parent are preserved (creating "foo" leaves "bar"'s cached
// binding valid), matching fs_cache.tla's per-token OwnWrite(f); O(1) via the
// serve's (parent,name) hash. This is the dentry cache's SOLE coherence mechanism
// (there is no parent-cvers gate -- LARDER-DESIGN §4).
void larder_dentry_invalidate_name(struct larder *l, u64 parent_qid_path,
                                   const char *name, size_t name_len);

// -- L1e: the page sub-cache ----------------------------------------------------
//
// The page cache holds file CONTENT, so it is engaged only for a CACHEABLE client
// (a proven content-versioned FS -- p9_client.cacheable; the dev9p hooks gate on
// it). A stream/control server (netd /net -- consuming reads, no offset stability,
// qid.version always 0) is never page-cached: a re-read of an offset would serve
// stale stream bytes. That gate lives in the dev9p caller (it holds the client);
// these functions are the mechanism.

// Serve (fs_cache.tla Read). On a HIT for (qid_path, page_index) whose cvers ==
// want_cvers and page_off < valid_len, copy min(want, valid_len - page_off) bytes
// into `out` UNDER the lock and return the count (> 0). Otherwise set *seq0_out to
// the current gen (the caller's populate guard) and return 0. ONE page per call
// (a bounded <= 4 KiB copy under the lock); the caller loops for a multi-page read
// (a short serve is a legal short read). A cvers mismatch is a cross-open external
// write (close-to-open) -> miss + refetch. NULL l/out or want == 0 -> 0.
u32 larder_page_serve(struct larder *l, u64 qid_path, u64 page_index,
                      u32 page_off, u32 want, u32 want_cvers,
                      u8 *out, u64 *seq0_out);

// Populate (Open/Refetch). Install page (qid_path, page_index) with `len` bytes
// [0, len) of `data` (content from the page's aligned start), tagged `cvers`, IFF
// l->gen == seq0 (no invalidate raced the read since the snapshot -- the
// resurrection close). `len` is clamped to LARDER_PAGE_SIZE. Lazily allocates the
// slot's 4 KiB buffer under the lock (kmalloc is non-blocking -> buddy zone->lock,
// the pinned larder -> buddy lock order); a kmalloc failure skips the install (the
// RPC already served the bytes -- the cache is a best-effort accelerator, I-38
// correctness never depends on a fill). Overwrite (qid_path,page_index) / free
// slot / LRU victim (reusing its retained buffer).
// G1 (term-4) write-populate: install the writer's OWN landed bytes at the wb
// flush. own pages serve WITHOUT the cvers gate (no post-flush cvers is knowable
// client-side -- Rwrite carries none) and WITHOUT the populate gen guard: the
// resurrection class the guard closes is a READ-populate racing an invalidate;
// these bytes are the writer's just-landed content and no same-file mutation can
// land concurrently (the caller holds the wb flush freeze, wb_flushers > 0).
// page_off == 0 installs/overwrites the page (own, cvers ignored); page_off > 0
// EXTENDS only an existing OWN page whose valid_len == page_off (the append-chain
// continuation across cap flushes) -- any other shape is skipped: never a hole,
// never a mixed cvers/own page. Later mutations invalidate per-qid as always,
// dropping own pages with the rest. Spec: fs_cache.tla FlushClose -> Refetch
// (EnableFlushPopulate); the buggy_populate_unflushed cfg pins the landed-only
// coupling.
void larder_page_install_own(struct larder *l, u64 qid_path, u64 page_index,
                             u32 page_off, const u8 *data, u32 len);

void larder_page_install(struct larder *l, u64 seq0, u64 qid_path, u64 page_index,
                         u32 cvers, const u8 *data, u32 len);

// Invalidate (OwnWrite). Drop EVERY cached page of `qid_path` (the guest wrote the
// file) and bump gen (always -- a concurrent populate that read the pre-write
// content must be skipped). Slot buffers are retained for reuse.
void larder_page_invalidate(struct larder *l, u64 qid_path);
// G1b (term-4): drop ONLY the file's pages whose page_index lies in
// [first_idx, last_idx] -- the write-through discipline for a non-append
// write of a known [offset, offset+len) range. Pages OUTSIDE the range keep
// serving: a cvers-gated page's staleness is caught by the normal open-time
// revalidation exactly as if it had been dropped, and an OWN page's bytes at
// untouched offsets are still the file's current content (the single-writer
// premise; the buildid-pwrite class must not nuke a whole just-populated
// archive for a ~100-byte patch). Bumps gen like the whole-file form.
void larder_page_invalidate_range(struct larder *l, u64 qid_path,
                                  u64 first_idx, u64 last_idx);

// -- FID-LIFECYCLE cached-open helpers (docs/FID-LIFECYCLE-DESIGN.md §3.3) ------
//
// PURE READERS for the fidless open: neither mutates cache state beyond the
// CLOCK ref bit, so the L1f-audited mutation surface is untouched. Coverage =
// pages [0, ceil(size/PAGE)) all resident at `cvers`, each holding content to
// min(PAGE, size - i*PAGE) (a mid-file msize-clamped partial page FAILS -- no
// served hole). size == 0 is trivially covered.

// The RPC-free hint: is [0, size) of qid_path fully cached at cvers?
bool larder_pages_cover(struct larder *l, u64 qid_path, u32 cvers, u64 size);

// Verify coverage AND copy [0, size) into `out` (caller-owned, >= size bytes)
// under ONE lock hold -- atomic vs a concurrent invalidate (it precedes, failing
// the coverage, or follows the whole copy). `seq0` is the caller's
// larder_gen_snapshot capture from BEFORE its coverage decision: the snapshot
// fails closed if ANY invalidate moved the gen since (the B1-audit F1 gen
// witness -- a third actor's stale-fid repopulate could otherwise re-satisfy
// coverage at the old cvers with post-write bytes; see larder.c). Returns
// false on a gen mismatch or any coverage gap (`out` may be partially
// scribbled; the caller discards it). The open-time snapshot IS the
// fs_cache.tla Open linearization for the cached-open path.
bool larder_pages_snapshot(struct larder *l, u64 qid_path, u32 cvers,
                           u64 size, u8 *out, u64 seq0);

// Free every lazily-allocated array + page buffer (all three sub-caches are
// heap). Called from p9_client_destroy. No op is in flight at destroy (the last
// attached ref dropped), so the lock is defensive.
void larder_destroy(struct larder *l);

#endif // THYLACINE_LARDER_H
