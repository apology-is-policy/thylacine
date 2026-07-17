// Image — the qid-keyed shared-text cache (REVENANT R-3; the Plan 9 Image).
//
// Per docs/REVENANT.md §4.4 + §4.6 + ARCH §28 I-36. The Image cache is a
// kernel-global, refcounted registry of BURROW_TYPE_FILE non-writable-segment
// Burrows (R+X text; + R-only rodata since #45), keyed on the executable's
// FILE IDENTITY:
//
//     (dc, devno, qid.path, qid.vers, file_offset, size, exec)
//
// Its purpose is cross-Proc text sharing. The first exec of a binary creates a
// FILE Burrow over the executable's pinned Spoor (R-1) and demand-pages its
// pages in (R-2); a SECOND exec of the same binary looks the qid up here and
// REUSES that Burrow (handle_count++), so the two Procs map the same Burrow and
// fault into the same `filepages[]` array — they share one set of physical text
// pages. This is the Plan 9 Image (the heritage name kept), realized directly on
// the dual-refcount Burrow lifecycle (#847 / I-7).
//
// It is the SAFE, file-identity-keyed sharing — NOT KSM: it never content-scans
// across unknown content; an attacker learns nothing they didn't already know
// (that Proc X runs binary Y). The content-keyed cross-binary dedup that IS KSM
// (an ASLR-defeating side channel) is declined permanently (REVENANT §7).
//
// COHERENCE is the qid.vers sampled at exec: a binary atomically REPLACED
// (FS-gamma rename-swap -> a new qid.vers) is a NEW cache entry (the qid.vers in
// the key misses the old one); a Proc stays pinned to the version it exec'd
// (REVENANT §3.1). DISTINCT SEGMENTS of one binary (same qid, different
// file_offset) are distinct entries — each segment is its own Burrow over its
// own Spoor ref (no consolidation needed). Since #45 a binary typically
// contributes TWO entries: its text segment + its R-only rodata segment. The
// key includes `exec` (#45 audit F1): two segments with an IDENTICAL file
// window but different X-ness (a crafted ELF's aliased R+X + R-only PT_LOADs)
// resolve to DISTINCT Burrows, so no single FILE Burrow is ever mapped at both
// an executable and a non-executable prot -- the property that keeps the
// fault arm's freq->exec-gated I-cache sync sound (a non-exec fill never
// leaves an executable resident-hit unsynced). A legit binary is unaffected:
// the same file's same segment always carries the same X bit -> the same key.
//
// LIFECYCLE: the cache holds ONE handle_count ref per cached Burrow (a STRONG
// ref), so text persists after the last Proc unmaps (the Plan 9 temporal cache —
// a re-exec after exit reuses the resident pages with no re-read). The cache is
// bounded at IMAGE_CACHE_MAX entries; on a full insert the LRU IDLE entry
// (handle_count==1 [cache only] && mapping_count==0 [no live mapping]) is
// evicted. A memory-pressure-triggered reclaim pass is the documented v1.x seam
// (REVENANT §9); the LRU cap bounds RAM at v1.0. An entry that is full of LIVE
// images (none idle) degrades to a cache BYPASS (the new Burrow is created
// un-registered — it lives on its mapping, just not shared), never an exec
// failure.
//
// Resource accounting (I-32, condition 7) is the map/exec layer's job (R-4),
// like ANON's attach-time charge — shared text is charged once via the I-7 dual
// refcount. The cache itself charges nothing.
//
// At R-3 there is NO production caller: exec still slurps (R-4 wires
// image_lookup_or_create in place of the eager whole-ELF read). The cache is
// exercised in isolation by the kernel test suite (KERNEL_TESTS hooks below).

#ifndef THYLACINE_IMAGE_H
#define THYLACINE_IMAGE_H

#include <thylacine/types.h>

struct Spoor;
struct Burrow;

// Bounded LRU cap on cached images. Sized generously for the v1.0 binary set
// (the toolchain + the native programs) at TWO entries per binary since #45
// (text + rodata; 128 slots ~= 64 binaries, ~4 KiB more BSS than the original
// 64); the pressure-triggered reclaim pass (REVENANT §9) is the v1.x growth
// path. On a full insert the LRU idle entry is evicted; a cache full of LIVE
// entries degrades to a bypass (never fails exec).
#define IMAGE_CACHE_MAX 128

// Bring up the Image cache. Idempotent guard extincts on re-call. The backing
// table is BSS (zero == all-free), so this only flips the inited flag — a clear
// bring-up point + a future allocation hook. Call after burrow_init.
void image_cache_init(void);

// image_lookup_or_create — the exec text-segment entry point (R-4 consumer).
// Resolve the file-backed text Burrow for the segment [file_offset,
// file_offset+length) of the file behind `spoor`:
//   - On a HIT (the qid+offset+size is already cached): return the cached Burrow
//     with one handle ref taken FOR THE CALLER (burrow_ref). The caller's
//     `spoor` is REDUNDANT and is consumed (spoor_clunk'd) internally.
//   - On a MISS: burrow_create_file(spoor, ...) (which ADOPTS `spoor`), register
//     it (the cache keeps one handle ref), and return it with a second handle
//     ref for the caller.
//
// CONTRACT: this ALWAYS consumes `spoor` on every success path (adopted into the
// new Burrow on a miss; clunk'd as redundant on a hit) — the caller must NOT
// spoor_clunk it. It returns a Burrow with ONE handle ref the caller owns: use
// it exactly like burrow_create_anon's return — burrow_map it, then burrow_unref
// (the mapping keeps it alive; the cache's own ref keeps it cached past the last
// unmap). On a NULL return (length==0 / overflow / SLUB OOM) NO ref was taken
// and `spoor` is NOT consumed — the caller still owns it (mirrors
// burrow_create_file).
//
// `exec` (#45 audit F1) discriminates an executable segment from a
// non-executable one with an otherwise-identical file window (see the key note
// above); pass `(seg->flags & PF_X) != 0`.
//
// SMP: serialized by the global cache lock; the blocking burrow_create_file runs
// OUTSIDE the lock with a re-search-on-reacquire (the create race — two Procs
// exec'ing the same binary concurrently; the loser frees its surplus Burrow).
struct Burrow *image_lookup_or_create(struct Spoor *spoor, u64 file_offset,
                                      size_t length, bool exec);

#ifdef KERNEL_TESTS
// Number of live (used) cache entries. Snapshot under the cache lock.
int image_cache_live_count_for_test(void);
// Evict every IDLE entry (handle_count==1 && mapping_count==0): drop the cache's
// ref so the image frees (clunking its Spoor). Entries with live mappings are
// left intact. Returns the count evicted. For test isolation between cases AND a
// real regression for the detach-under-lock / unref-outside eviction lifetime.
int image_cache_evict_idle_for_test(void);
// Cumulative diagnostic counters.
u64 image_cache_hits_for_test(void);
u64 image_cache_creates_for_test(void);
u64 image_cache_evictions_for_test(void);
#endif

#endif // THYLACINE_IMAGE_H
