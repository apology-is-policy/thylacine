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

// Attr-cache capacity. The base X-check re-stat storm hits a handful of dirs
// (root 96.8%, ~7 top-level dirs); the full dir working set is ~313 + the hot
// fstat'd files. 256 covers the base X-check completely with ample headroom for
// fstat locality; the memory is ~26 KiB/client (a p9_client already carries a
// 32 KiB out_buf, and there are a handful of live clients). Tunable.
#define LARDER_ATTR_ENTRIES 256u

// Dentry-cache capacity + the inline component-name bound. The hot re-resolution
// set (root -> ~7 top-level dirs re-walked thousands of times) + the failed-lookup
// storm (import/search-path negative probes) both fit comfortably; 256 mirrors the
// attr cache. NAME_MAX 88 covers a go-cache content-hash file name (a 64-hex hash +
// suffix ~66 B); a longer component is simply not cached (the serve/populate
// fail-safe to the RPC -- correctness is unaffected, only that walk isn't elided).
#define LARDER_DENTRY_ENTRIES  256u
#define LARDER_DENTRY_NAME_MAX 88u

// Page-cache capacity + unit (L1e). Each slot caches one LARDER_PAGE_SIZE window of
// a file's content, keyed (qid.path, page_index = file offset / LARDER_PAGE_SIZE).
// The unit is one 4 KiB page (aligns with the buddy/demand-page system and Go's
// page-aligned .a reads). LARDER_PAGE_ENTRIES slots x 4 KiB = the per-client page
// budget: the slot buffers are HEAP (unlike the inline attr/dentry entries), lazily
// kmalloc'd per slot, reused across evictions, freed at larder_destroy. 512 slots
// = 2 MiB caches the hot cold-read re-read set (cross-package .a export data --
// 83.9% cold read redundancy); tunable at L1f. Only a cacheable client (a proven
// content-versioned FS -- p9_client.cacheable) ever allocates page buffers, so a
// stream/control server (netd) costs only the inline slot metadata, never heap.
#define LARDER_PAGE_ENTRIES 512u
#define LARDER_PAGE_SIZE    4096u

struct larder_attr_ent {
    u64            qid_path;   // key. root's qid.path is 0x0, so `valid` (not
                               //   path==0) marks an empty slot.
    u32            cvers;      // content-version (qid.version == si_cvers) at fetch;
                               //   consumed by the L1e page revalidation.
    bool           valid;      // slot occupied
    u64            lru;        // last-access sequence (LRU eviction)
    struct t_stat  attr;       // cached metadata (mode/uid/gid/size/times/nlink/...)
};

// A cached name->child binding (the Plan 9 dentry). Keyed by (parent_qid_path,
// name[0..name_len)). A NEGATIVE entry caches "name does not exist in parent"
// (the failed-lookup-storm win). NO content-version field: a name-binding is a
// PARENT-dirent fact, and Stratum surfaces no directory-content version that
// tracks a dirent change (a child create/unlink does not bump the parent's
// si_cvers -- verified src/fs/fs.c; only rename stamps the parent mtime), so the
// dentry cache is NOT cvers-gated -- its sole coherence is own-write invalidation
// (larder_dentry_invalidate_parent on a create/rename/unlink; LARDER-DESIGN §4).
// A positive entry's reply attr is served from the attr sub-cache (the child's
// qid.path keys it), so the dentry stores only the linkage, not the attr.
struct larder_dentry_ent {
    u64  parent_qid_path;   // key part 1
    u64  child_qid_path;    // resolved child (positive only; 0 when negative)
    u32  name_len;          // key part 2: name[0..name_len)
    bool valid;             // slot occupied
    bool negative;          // true: (parent,name) -> ENOENT (no child)
    u64  lru;               // last-access sequence (LRU eviction)
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
    u64  lru;          // last-access sequence (LRU eviction)
    u8  *page;         // heap buffer (lazily kmalloc'd; reused; freed at larder_destroy)
};

struct larder {
    spin_lock_t               lock;       // dedicated near-leaf; never held across an RPC
    u64                       lru_clock;  // monotonic; stamps ent->lru on access
    u64                       gen;        // monotonic invalidation sequence (populate guard)
    struct larder_attr_ent    attr[LARDER_ATTR_ENTRIES];
    struct larder_dentry_ent  dentry[LARDER_DENTRY_ENTRIES];
    struct larder_page_ent    page[LARDER_PAGE_ENTRIES];
    // Diagnostics (hit-rate measurement for the L1f bench; not load-bearing).
    u64 attr_hits;
    u64 attr_misses;
    u64 attr_installs;
    u64 attr_install_skips;   // gen-guard skips (populate raced an invalidate)
    u64 attr_invalidations;
    u64 attr_evictions;
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
    u64 page_invalidations;   // pages dropped by own-write invalidate
    u64 page_evictions;
};

// Initialize an embedded Larder (memset-equivalent + lock init). No teardown --
// the inline array frees with its owning p9_client.
void larder_init(struct larder *l);

// Serve (Read). On a HIT, copy the cached attr for `qid_path` into *out under the
// lock and return true (no RPC). On a MISS, set *seq0_out to the current gen (the
// populate guard for the caller's subsequent larder_attr_install) and return
// false. A NULL Larder / out is a miss (defensive).
bool larder_attr_serve(struct larder *l, u64 qid_path,
                       struct t_stat *out, u64 *seq0_out);

// Capture the current gen for a populate site that does NOT serve first (the
// walk_attrs free-populate). Call BEFORE the RPC; pass the result to
// larder_attr_install.
u64 larder_gen_snapshot(struct larder *l);

// Populate (Open/Refetch). Install {attr, cvers} for `qid_path` -- overwriting an
// existing entry (revalidate-by-overwrite), using a free slot, or evicting the
// LRU entry when full -- IFF `l->gen == seq0` (no invalidate raced the RPC since
// the seq0 snapshot). Otherwise skipped (the gen guard). `attr` is copied.
void larder_attr_install(struct larder *l, u64 seq0, u64 qid_path,
                         u32 cvers, const struct t_stat *attr);

// Invalidate (OwnWrite). Drop `qid_path`'s cached entry (if present) and bump gen
// (always -- the file was mutated even if it was not cached, so a concurrent
// populate that fetched the pre-mutation value must be skipped).
void larder_attr_invalidate(struct larder *l, u64 qid_path);

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

// Invalidate (OwnWrite). Drop EVERY cached dentry whose parent is
// `parent_qid_path` (a create/rename/unlink changed this directory's dirent set),
// and bump gen (always -- a concurrent populate that fetched the pre-mutation
// listing must be skipped). This is the dentry cache's SOLE coherence mechanism
// (there is no parent-cvers gate -- LARDER-DESIGN §4).
void larder_dentry_invalidate_parent(struct larder *l, u64 parent_qid_path);

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
void larder_page_install(struct larder *l, u64 seq0, u64 qid_path, u64 page_index,
                         u32 cvers, const u8 *data, u32 len);

// Invalidate (OwnWrite). Drop EVERY cached page of `qid_path` (the guest wrote the
// file) and bump gen (always -- a concurrent populate that read the pre-write
// content must be skipped). Slot buffers are retained for reuse.
void larder_page_invalidate(struct larder *l, u64 qid_path);

// Free every lazily-allocated page buffer. Called from p9_client_destroy (the
// attr/dentry sub-caches are inline -- only the page buffers are heap). No op is
// in flight at destroy (the last attached ref dropped), so the lock is defensive.
void larder_destroy(struct larder *l);

#endif // THYLACINE_LARDER_H
