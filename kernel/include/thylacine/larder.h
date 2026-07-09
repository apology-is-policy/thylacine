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

struct larder_attr_ent {
    u64            qid_path;   // key. root's qid.path is 0x0, so `valid` (not
                               //   path==0) marks an empty slot.
    u32            cvers;      // content-version (qid.version == si_cvers) at fetch;
                               //   consumed by the L1d/L1e revalidation.
    bool           valid;      // slot occupied
    u64            lru;        // last-access sequence (LRU eviction)
    struct t_stat  attr;       // cached metadata (mode/uid/gid/size/times/nlink/...)
};

struct larder {
    spin_lock_t             lock;       // dedicated near-leaf; never held across an RPC
    u64                     lru_clock;  // monotonic; stamps ent->lru on access
    u64                     gen;        // monotonic invalidation sequence (populate guard)
    struct larder_attr_ent  attr[LARDER_ATTR_ENTRIES];
    // Diagnostics (hit-rate measurement for the L1f bench; not load-bearing).
    u64 attr_hits;
    u64 attr_misses;
    u64 attr_installs;
    u64 attr_install_skips;   // gen-guard skips (populate raced an invalidate)
    u64 attr_invalidations;
    u64 attr_evictions;
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

#endif // THYLACINE_LARDER_H
