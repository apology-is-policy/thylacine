// REVENANT R-3: the Image cache (qid-keyed shared-text registry).
//
// Exercises kernel/image.c::image_lookup_or_create in isolation (no production
// exec path maps a FILE Burrow until R-4). Six tests cover the cache semantics +
// the refcount/spoor lifecycle:
//
//   image.miss_then_hit_shares
//     First lookup MISSES -> creates a FILE Burrow (handle_count = cache 1 +
//     caller 1). A second lookup of the SAME file identity HITS -> returns the
//     SAME Burrow (the cross-Proc text share) with a third handle ref; the
//     redundant second spoor is consumed (clunked). Callers unref; the strong
//     cache ref keeps it cached; evict frees it + clunks the adopted spoor.
//
//   image.distinct_qid_distinct_entry / image.qid_vers_bump_new_entry /
//   image.distinct_offset_distinct_entry
//     A different qid.path, a bumped qid.vers (atomic-replace coherence), or a
//     different file_offset (a distinct segment) each key a SEPARATE entry.
//
//   image.eviction_bounds_cache
//     Creating more idle images than IMAGE_CACHE_MAX never grows the cache past
//     the cap (the LRU idle victim is evicted); every spoor is clunked.
//
//   image.bad_arg_retains_spoor
//     length==0 -> NULL WITHOUT consuming the spoor (the caller still owns it),
//     matching burrow_create_file's NULL-on-bad-arg contract.
//
// The lost-create-RACE branch of pass 2 (two Procs exec'ing one binary
// concurrently) needs real SMP concurrency a single-threaded harness cannot
// force; it is covered by the file-header proof + the R-5 SMP gate (once R-4
// maps FILE Burrows under load).

#include "test.h"

#include <thylacine/burrow.h>
#include <thylacine/dev.h>
#include <thylacine/image.h>
#include <thylacine/page.h>          // PAGE_SIZE
#include <thylacine/spoor.h>
#include <thylacine/types.h>

void test_image_miss_then_hit_shares(void);
void test_image_distinct_qid_distinct_entry(void);
void test_image_qid_vers_bump_new_entry(void);
void test_image_distinct_offset_distinct_entry(void);
void test_image_eviction_bounds_cache(void);
void test_image_bad_arg_retains_spoor(void);

// A trivial backing Dev. The cache never reads it (only R-4's fault path will),
// and the test spoors are never opened, so spoor_clunk skips dev->close -- no
// .read / .close hook is needed.
static struct Dev g_image_test_dev = {
    .dc   = 'I',
    .name = "imgtest",
};

// Mint a fresh stub Spoor with a chosen qid identity (path + vers). spoor_alloc
// gives ref=1 (the test's reference, consumed by image_lookup_or_create).
static struct Spoor *mk_spoor(u64 qpath, u32 qvers) {
    struct Spoor *s = spoor_alloc(&g_image_test_dev);
    if (s) {                 // OOM -> NULL; image_lookup_or_create NULL-checks,
        s->qid.path = qpath; // so the caller's `b != NULL` assert fails cleanly.
        s->qid.vers = qvers;
        s->qid.type = QTFILE;
    }
    return s;
}

void test_image_miss_then_hit_shares(void) {
    image_cache_evict_idle_for_test();   // isolate from any prior case
    TEST_EXPECT_EQ(image_cache_live_count_for_test(), 0, "cache empty at start");

    u64 freed0   = spoor_total_freed();
    u64 hits0    = image_cache_hits_for_test();
    u64 creates0 = image_cache_creates_for_test();

    // First exec: MISS -> create. s1 is ADOPTED into B1.
    struct Spoor *s1 = mk_spoor(0x1000, 1);
    struct Burrow *b1 = image_lookup_or_create(s1, 0, PAGE_SIZE, /*exec=*/true);
    TEST_ASSERT(b1 != NULL, "first lookup creates a Burrow");
    TEST_EXPECT_EQ(burrow_handle_count(b1), 2, "B1 handle_count = cache 1 + caller 1");
    TEST_EXPECT_EQ(image_cache_live_count_for_test(), 1, "one live entry");
    TEST_EXPECT_EQ(image_cache_creates_for_test() - creates0, 1, "one create");

    // Second exec, SAME identity: HIT -> shares B1. s2 is redundant -> clunked.
    struct Spoor *s2 = mk_spoor(0x1000, 1);
    struct Burrow *b2 = image_lookup_or_create(s2, 0, PAGE_SIZE, /*exec=*/true);
    TEST_ASSERT(b2 == b1, "second lookup SHARES the same Burrow (cross-Proc text share)");
    TEST_EXPECT_EQ(burrow_handle_count(b1), 3, "handle_count = cache 1 + 2 callers");
    TEST_EXPECT_EQ(image_cache_live_count_for_test(), 1, "still one entry");
    TEST_EXPECT_EQ(image_cache_hits_for_test() - hits0, 1, "one hit");
    TEST_EXPECT_EQ(spoor_total_freed() - freed0, 1, "redundant spoor s2 clunked on the hit");

    // Both callers drop their refs (exec's post-map burrow_unref).
    burrow_unref(b2);
    burrow_unref(b1);
    TEST_EXPECT_EQ(burrow_handle_count(b1), 1, "back to the cache's single (strong) ref");
    TEST_EXPECT_EQ(image_cache_live_count_for_test(), 1, "still cached after callers drop");

    // Evict the idle image: drops the cache ref -> B1 frees -> s1 (adopted) clunked.
    int ev = image_cache_evict_idle_for_test();
    TEST_EXPECT_EQ(ev, 1, "one idle image evicted");
    TEST_EXPECT_EQ(image_cache_live_count_for_test(), 0, "cache empty after evict");
    TEST_EXPECT_EQ(spoor_total_freed() - freed0, 2, "s1 clunked at B1 free (2 spoors total)");
}

void test_image_distinct_qid_distinct_entry(void) {
    image_cache_evict_idle_for_test();
    struct Burrow *b1 = image_lookup_or_create(mk_spoor(0xA000, 1), 0, PAGE_SIZE, /*exec=*/true);
    struct Burrow *b2 = image_lookup_or_create(mk_spoor(0xB000, 1), 0, PAGE_SIZE, /*exec=*/true);
    TEST_ASSERT(b1 != NULL && b2 != NULL, "both created");
    TEST_ASSERT(b1 != b2, "distinct qid.path -> distinct Burrow");
    TEST_EXPECT_EQ(image_cache_live_count_for_test(), 2, "two entries");
    burrow_unref(b1);
    burrow_unref(b2);
    TEST_EXPECT_EQ(image_cache_evict_idle_for_test(), 2, "both evicted");
    TEST_EXPECT_EQ(image_cache_live_count_for_test(), 0, "cleaned");
}

void test_image_qid_vers_bump_new_entry(void) {
    image_cache_evict_idle_for_test();
    struct Burrow *b1 = image_lookup_or_create(mk_spoor(0xC000, 1), 0, PAGE_SIZE, /*exec=*/true);
    struct Burrow *b2 = image_lookup_or_create(mk_spoor(0xC000, 2), 0, PAGE_SIZE, /*exec=*/true);
    TEST_ASSERT(b1 != NULL && b2 != NULL, "both created");
    TEST_ASSERT(b1 != b2, "qid.vers bump (atomic replace) -> NEW entry (coherence)");
    TEST_EXPECT_EQ(image_cache_live_count_for_test(), 2, "two entries");
    burrow_unref(b1);
    burrow_unref(b2);
    image_cache_evict_idle_for_test();
    TEST_EXPECT_EQ(image_cache_live_count_for_test(), 0, "cleaned");
}

void test_image_distinct_offset_distinct_entry(void) {
    image_cache_evict_idle_for_test();
    struct Burrow *b1 = image_lookup_or_create(mk_spoor(0xD000, 1), 0,         PAGE_SIZE, /*exec=*/true);
    struct Burrow *b2 = image_lookup_or_create(mk_spoor(0xD000, 1), PAGE_SIZE, PAGE_SIZE, /*exec=*/true);
    TEST_ASSERT(b1 != NULL && b2 != NULL, "both created");
    TEST_ASSERT(b1 != b2, "same qid, distinct file_offset -> distinct segment entry");
    TEST_EXPECT_EQ(image_cache_live_count_for_test(), 2, "two segment entries");
    burrow_unref(b1);
    burrow_unref(b2);
    image_cache_evict_idle_for_test();
    TEST_EXPECT_EQ(image_cache_live_count_for_test(), 0, "cleaned");
}

// #45 audit F1: exec is part of the key. Two segments with an IDENTICAL file
// window (same qid + file_offset + size) but different X-ness -- the crafted-ELF
// alias -- MUST resolve to DISTINCT Burrows, so one FILE Burrow is never mapped
// at both an executable and a non-executable prot (the property the fault arm's
// exec-gated I-cache sync depends on). Fails on the pre-fix prot-less key
// (the exec=false lookup would HIT the exec=true entry -> same Burrow, one create).
void test_image_exec_discriminates_key(void) {
    image_cache_evict_idle_for_test();
    u64 creates0 = image_cache_creates_for_test();
    // SAME identity + window; differ only in exec.
    struct Burrow *bx = image_lookup_or_create(mk_spoor(0xF000, 1), 0, PAGE_SIZE, /*exec=*/true);
    struct Burrow *br = image_lookup_or_create(mk_spoor(0xF000, 1), 0, PAGE_SIZE, /*exec=*/false);
    TEST_ASSERT(bx != NULL && br != NULL, "both created");
    TEST_ASSERT(bx != br, "same window, different X-ness -> DISTINCT Burrows (no dual-prot alias)");
    TEST_EXPECT_EQ(image_cache_creates_for_test() - creates0, 2, "TWO creates, not one");
    TEST_EXPECT_EQ(image_cache_live_count_for_test(), 2, "two entries");
    // A repeat of each exec-class HITS its own entry (sharing still works per class).
    u64 hits0 = image_cache_hits_for_test();
    struct Burrow *bx2 = image_lookup_or_create(mk_spoor(0xF000, 1), 0, PAGE_SIZE, /*exec=*/true);
    TEST_ASSERT(bx2 == bx, "exec=true repeat shares the exec entry");
    TEST_EXPECT_EQ(image_cache_hits_for_test() - hits0, 1, "one hit (per-class sharing intact)");
    burrow_unref(bx);
    burrow_unref(bx2);
    burrow_unref(br);
    TEST_EXPECT_EQ(image_cache_evict_idle_for_test(), 2, "both entries evicted");
    TEST_EXPECT_EQ(image_cache_live_count_for_test(), 0, "cleaned");
}

void test_image_eviction_bounds_cache(void) {
    image_cache_evict_idle_for_test();
    TEST_EXPECT_EQ(image_cache_live_count_for_test(), 0, "empty start");
    u64 ev0 = image_cache_evictions_for_test();

    // More distinct IDLE images than the cache holds: each miss->create then the
    // caller-ref is dropped (-> idle: handle_count==1, mapping_count==0).
    const int N = IMAGE_CACHE_MAX + 5;
    for (int i = 0; i < N; i++) {
        struct Burrow *b = image_lookup_or_create(mk_spoor(0x10000 + (u64)i, 1), 0, PAGE_SIZE, /*exec=*/true);
        TEST_ASSERT(b != NULL, "create");
        burrow_unref(b);     // -> idle
    }

    TEST_EXPECT_EQ(image_cache_live_count_for_test(), IMAGE_CACHE_MAX, "LRU-bounded at the cap");
    TEST_EXPECT_EQ(image_cache_evictions_for_test() - ev0, (u64)(N - IMAGE_CACHE_MAX),
                   "evicted exactly the overflow");

    TEST_EXPECT_EQ(image_cache_evict_idle_for_test(), IMAGE_CACHE_MAX, "all idle images evicted");
    TEST_EXPECT_EQ(image_cache_live_count_for_test(), 0, "cleaned");
}

void test_image_bad_arg_retains_spoor(void) {
    image_cache_evict_idle_for_test();
    u64 freed0 = spoor_total_freed();
    struct Spoor *s = mk_spoor(0xE000, 1);
    struct Burrow *b = image_lookup_or_create(s, 0, /*length=*/0, /*exec=*/true);
    TEST_ASSERT(b == NULL, "length==0 -> NULL");
    TEST_EXPECT_EQ(spoor_total_freed() - freed0, 0, "bad-arg path does NOT consume the spoor");
    spoor_clunk(s);          // the caller still owns it -> cleans up
    TEST_EXPECT_EQ(spoor_total_freed() - freed0, 1, "caller clunks its retained spoor");
}
