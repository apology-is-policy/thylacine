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

// Models a content server: its images are shared and cached (vote 114).
static bool fake_content_versioned(struct Spoor *c) {
    (void)c;
    return true;
}

// A trivial backing Dev. The cache never reads it (only R-4's fault path will),
// and the test spoors are never opened, so spoor_clunk skips dev->close -- no
// .read / .close hook is needed.
static struct Dev g_image_test_dev = {
    .dc   = 'I',
    .name = "imgtest",
    .content_versioned = fake_content_versioned,
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
    struct Burrow *b1 = image_lookup_or_create(s1, 0, PAGE_SIZE, /*exec=*/true, BURROW_FILE_LIMIT_UNKNOWN);
    TEST_ASSERT(b1 != NULL, "first lookup creates a Burrow");
    TEST_EXPECT_EQ(burrow_handle_count(b1), 2, "B1 handle_count = cache 1 + caller 1");
    TEST_EXPECT_EQ(image_cache_live_count_for_test(), 1, "one live entry");
    TEST_EXPECT_EQ(image_cache_creates_for_test() - creates0, 1, "one create");

    // Second exec, SAME identity: HIT -> shares B1. s2 is redundant -> clunked.
    struct Spoor *s2 = mk_spoor(0x1000, 1);
    struct Burrow *b2 = image_lookup_or_create(s2, 0, PAGE_SIZE, /*exec=*/true, BURROW_FILE_LIMIT_UNKNOWN);
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

// Vote 114: the same identity off a Dev that does not answer content_versioned
// (a netd file, an unproven dev9p session) is two private images, each holding
// its Spoor only until its last unref and keeping its own size. The control,
// one variable away: the same identity off the versioned fake is cached.
static struct Dev g_image_private_dev = {
    .dc   = 'I',
    .name = "imgpriv",
};

static struct Spoor *mk_private_spoor(u64 qpath, u32 qvers) {
    struct Spoor *s = spoor_alloc(&g_image_private_dev);
    if (s) {
        s->qid.path = qpath;
        s->qid.vers = qvers;
        s->qid.type = QTFILE;
    }
    return s;
}

void test_image_private_unless_versioned(void) {
    image_cache_evict_idle_for_test();
    u64 freed0   = spoor_total_freed();
    u64 priv0    = image_cache_private_for_test();
    u64 creates0 = image_cache_creates_for_test();

    // Every value is read and every ref dropped before the first assertion: an
    // assertion returns, and a ref it strands is a live image no later test's
    // eviction can take, which fails every cache count after this one.
    struct Burrow *b1 = image_lookup_or_create(mk_private_spoor(0x7000, 0), 0, PAGE_SIZE,
                                               /*exec=*/false, 3 * PAGE_SIZE);
    struct Burrow *b2 = image_lookup_or_create(mk_private_spoor(0x7000, 0), 0, PAGE_SIZE,
                                               /*exec=*/false, PAGE_SIZE);
    bool both     = b1 != NULL && b2 != NULL;
    bool two      = both && b1 != b2;
    int  hc1      = b1 ? burrow_handle_count(b1) : -1;
    u64  live     = image_cache_live_count_for_test();
    u64  privs    = image_cache_private_for_test() - priv0;
    u64  creates  = image_cache_creates_for_test() - creates0;
    u64  lim1     = b1 ? b1->file_limit : 0;
    u64  lim2     = b2 ? b2->file_limit : 0;
    if (b1) burrow_unref(b1);
    u64  freed1   = spoor_total_freed() - freed0;
    if (b2) burrow_unref(b2);
    u64  freed2   = spoor_total_freed() - freed0;

    struct Burrow *b3 = image_lookup_or_create(mk_spoor(0x7000, 0), 0, PAGE_SIZE,
                                               /*exec=*/false, PAGE_SIZE);
    bool control  = b3 != NULL;
    if (b3) burrow_unref(b3);
    u64  live3    = image_cache_live_count_for_test();
    u64  freed3   = spoor_total_freed() - freed0;
    int  evicted  = image_cache_evict_idle_for_test();
    u64  freed4   = spoor_total_freed() - freed0;

    TEST_ASSERT(both, "both created");
    TEST_ASSERT(two, "one identity off an unversioned Dev is two private images");
    TEST_EXPECT_EQ(hc1, 1, "a private image's only ref is the caller's");
    TEST_EXPECT_EQ(live, 0, "nothing was cached");
    TEST_EXPECT_EQ(privs, 2, "two private creates");
    TEST_EXPECT_EQ(creates, 0, "no cached create");
    TEST_EXPECT_EQ(lim1, 3 * PAGE_SIZE, "the first keeps its own size");
    TEST_EXPECT_EQ(lim2, PAGE_SIZE, "the second keeps its own size");
    TEST_EXPECT_EQ(freed1, 1, "the last unref clunks a private image's Spoor: nothing else holds it");
    TEST_EXPECT_EQ(freed2, 2, "and the second's");
    TEST_ASSERT(control, "control created");
    TEST_EXPECT_EQ(live3, 1, "control: the versioned image is cached");
    TEST_EXPECT_EQ(freed3, 2, "control: its Spoor outlives its last caller");
    TEST_EXPECT_EQ(evicted, 1, "control evicted");
    TEST_EXPECT_EQ(freed4, 3, "control Spoor clunked at eviction");
}

void test_image_distinct_qid_distinct_entry(void) {
    image_cache_evict_idle_for_test();
    struct Burrow *b1 = image_lookup_or_create(mk_spoor(0xA000, 1), 0, PAGE_SIZE, /*exec=*/true, BURROW_FILE_LIMIT_UNKNOWN);
    struct Burrow *b2 = image_lookup_or_create(mk_spoor(0xB000, 1), 0, PAGE_SIZE, /*exec=*/true, BURROW_FILE_LIMIT_UNKNOWN);
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
    struct Burrow *b1 = image_lookup_or_create(mk_spoor(0xC000, 1), 0, PAGE_SIZE, /*exec=*/true, BURROW_FILE_LIMIT_UNKNOWN);
    struct Burrow *b2 = image_lookup_or_create(mk_spoor(0xC000, 2), 0, PAGE_SIZE, /*exec=*/true, BURROW_FILE_LIMIT_UNKNOWN);
    TEST_ASSERT(b1 != NULL && b2 != NULL, "both created");
    TEST_ASSERT(b1 != b2, "qid.vers bump (atomic replace) -> NEW entry (coherence)");
    TEST_EXPECT_EQ(image_cache_live_count_for_test(), 2, "two entries");
    burrow_unref(b1);
    burrow_unref(b2);
    image_cache_evict_idle_for_test();
    TEST_EXPECT_EQ(image_cache_live_count_for_test(), 0, "cleaned");
}

// devno-u64: two device instances 2^32 apart with the same qid are two files.
// A 32-bit key served the second the first's cached pages (an I-1 alias). The
// control, one variable away: the same wide devno again HITS its own entry.
static struct Spoor *mk_spoor_dev(u64 qpath, u32 qvers, u64 devno) {
    struct Spoor *s = mk_spoor(qpath, qvers);
    if (s) s->devno = devno;
    return s;
}

void test_image_devno_full_width_distinct_entry(void) {
    const u64 lo = 1u, hi = (1ull << 32) + 1u;
    image_cache_evict_idle_for_test();
    u64 hits0 = image_cache_hits_for_test();
    struct Burrow *b1 = image_lookup_or_create(mk_spoor_dev(0xE000, 1, lo), 0, PAGE_SIZE, /*exec=*/true, BURROW_FILE_LIMIT_UNKNOWN);
    struct Burrow *b2 = image_lookup_or_create(mk_spoor_dev(0xE000, 1, hi), 0, PAGE_SIZE, /*exec=*/true, BURROW_FILE_LIMIT_UNKNOWN);
    u64 gap_hits = image_cache_hits_for_test() - hits0;
    struct Burrow *b3 = image_lookup_or_create(mk_spoor_dev(0xE000, 1, hi), 0, PAGE_SIZE, /*exec=*/true, BURROW_FILE_LIMIT_UNKNOWN);
    bool created = b1 != NULL && b2 != NULL, distinct = b1 != b2, control = b3 == b2;
    // Every lookup took a ref; drop them BEFORE asserting, so a failure here
    // cannot leave live entries behind for the next test's counts.
    if (b1) burrow_unref(b1);
    if (b2) burrow_unref(b2);
    if (b3) burrow_unref(b3);
    image_cache_evict_idle_for_test();
    TEST_ASSERT(created, "both created");
    TEST_ASSERT(distinct, "devno 1 and 1+2^32 -> distinct Burrows");
    TEST_EXPECT_EQ(gap_hits, 0, "no hit across the 2^32 gap");
    TEST_ASSERT(control, "CONTROL: the same wide devno HITS its own entry");
    TEST_EXPECT_EQ(image_cache_live_count_for_test(), 0, "cleaned");
}

void test_image_distinct_offset_distinct_entry(void) {
    image_cache_evict_idle_for_test();
    struct Burrow *b1 = image_lookup_or_create(mk_spoor(0xD000, 1), 0,         PAGE_SIZE, /*exec=*/true, BURROW_FILE_LIMIT_UNKNOWN);
    struct Burrow *b2 = image_lookup_or_create(mk_spoor(0xD000, 1), PAGE_SIZE, PAGE_SIZE, /*exec=*/true, BURROW_FILE_LIMIT_UNKNOWN);
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
    struct Burrow *bx = image_lookup_or_create(mk_spoor(0xF000, 1), 0, PAGE_SIZE, /*exec=*/true, BURROW_FILE_LIMIT_UNKNOWN);
    struct Burrow *br = image_lookup_or_create(mk_spoor(0xF000, 1), 0, PAGE_SIZE, /*exec=*/false, BURROW_FILE_LIMIT_UNKNOWN);
    TEST_ASSERT(bx != NULL && br != NULL, "both created");
    TEST_ASSERT(bx != br, "same window, different X-ness -> DISTINCT Burrows (no dual-prot alias)");
    TEST_EXPECT_EQ(image_cache_creates_for_test() - creates0, 2, "TWO creates, not one");
    TEST_EXPECT_EQ(image_cache_live_count_for_test(), 2, "two entries");
    // A repeat of each exec-class HITS its own entry (sharing still works per class).
    u64 hits0 = image_cache_hits_for_test();
    struct Burrow *bx2 = image_lookup_or_create(mk_spoor(0xF000, 1), 0, PAGE_SIZE, /*exec=*/true, BURROW_FILE_LIMIT_UNKNOWN);
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
        struct Burrow *b = image_lookup_or_create(mk_spoor(0x10000 + (u64)i, 1), 0, PAGE_SIZE, /*exec=*/true, BURROW_FILE_LIMIT_UNKNOWN);
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
    struct Burrow *b = image_lookup_or_create(s, 0, /*length=*/0, /*exec=*/true, BURROW_FILE_LIMIT_UNKNOWN);
    TEST_ASSERT(b == NULL, "length==0 -> NULL");
    TEST_EXPECT_EQ(spoor_total_freed() - freed0, 0, "bad-arg path does NOT consume the spoor");
    spoor_clunk(s);          // the caller still owns it -> cleans up
    TEST_EXPECT_EQ(spoor_total_freed() - freed0, 1, "caller clunks its retained spoor");
}
