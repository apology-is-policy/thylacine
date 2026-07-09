// Larder tests (L1c -- the guest FS cache substrate + attr sub-cache; I-38).
// The attr cache maps to specs/fs_cache.tla:
//   serve HIT      = Read          (serve a valid entry without an RPC)
//   install        = Open/Refetch  (install fresh {attr, cvers}, gen-guarded)
//   invalidate     = OwnWrite      (drop + bump gen, write-through)
//   gen guard      = the atomic-Open realization (the populate-after-invalidate
//                    resurrection close -- larder.h note (2))
//   eviction/bound = Bounded (I-32 / I-38)

#include "test.h"

#include <thylacine/larder.h>
#include <thylacine/types.h>

void test_larder_install_serve(void);
void test_larder_serve_miss(void);
void test_larder_invalidate(void);
void test_larder_gen_guard_skips_raced_install(void);
void test_larder_root_qid_zero(void);
void test_larder_overwrite_wins(void);
void test_larder_eviction_bounded(void);

// The Larder is ~26 KiB (a 256-entry attr array) -- file scope, not stack.
static struct larder g_larder;

// Build a t_stat whose `mode` is a distinct marker so a serve round-trips it.
static struct t_stat mk_stat(u32 mode_marker) {
    struct t_stat s;
    for (size_t i = 0; i < sizeof(s); i++) ((u8 *)&s)[i] = 0;
    s.mode = mode_marker;
    return s;
}

void test_larder_install_serve(void) {
    larder_init(&g_larder);
    struct t_stat in = mk_stat(0755);
    u64 seq = larder_gen_snapshot(&g_larder);
    larder_attr_install(&g_larder, seq, /*qid_path=*/5, /*cvers=*/10, &in);

    struct t_stat out;
    u64 seq0 = 0;
    bool hit = larder_attr_serve(&g_larder, 5, &out, &seq0);
    TEST_ASSERT(hit, "installed entry serves as a hit");
    TEST_EXPECT_EQ(out.mode, 0755u, "served attr round-trips (Read)");
    TEST_EXPECT_EQ(g_larder.attr_hits, 1ull, "one hit counted");
    TEST_EXPECT_EQ(g_larder.attr_installs, 1ull, "one install counted");
}

void test_larder_serve_miss(void) {
    larder_init(&g_larder);
    struct t_stat out;
    u64 seq0 = 999;   // must be overwritten by the miss
    bool hit = larder_attr_serve(&g_larder, 42, &out, &seq0);
    TEST_ASSERT(!hit, "empty cache misses");
    TEST_EXPECT_EQ(seq0, 0ull, "miss returns the current gen snapshot (0 here)");
    TEST_EXPECT_EQ(g_larder.attr_misses, 1ull, "one miss counted");
}

void test_larder_invalidate(void) {
    larder_init(&g_larder);
    struct t_stat in = mk_stat(0644);
    larder_attr_install(&g_larder, larder_gen_snapshot(&g_larder), 7, 1, &in);

    struct t_stat out;
    u64 seq0 = 0;
    TEST_ASSERT(larder_attr_serve(&g_larder, 7, &out, &seq0), "hit before invalidate");

    larder_attr_invalidate(&g_larder, 7);   // OwnWrite
    TEST_ASSERT(!larder_attr_serve(&g_larder, 7, &out, &seq0),
                "invalidate drops the entry (miss after)");
    TEST_EXPECT_EQ(g_larder.attr_invalidations, 1ull, "one invalidation counted");
}

// The load-bearing SMP property: a populate whose RPC raced an invalidate is
// skipped (the resurrection close). Model: seq captured pre-RPC; an invalidate
// bumps gen; the install with the stale seq is a no-op.
void test_larder_gen_guard_skips_raced_install(void) {
    larder_init(&g_larder);
    struct t_stat in = mk_stat(0700);

    u64 seq = larder_gen_snapshot(&g_larder);        // "before the RPC"
    larder_attr_invalidate(&g_larder, 123);          // a concurrent own-write bumps gen
    larder_attr_install(&g_larder, seq, 8, 5, &in);  // "install the now-stale RPC result"

    struct t_stat out;
    u64 seq0 = 0;
    TEST_ASSERT(!larder_attr_serve(&g_larder, 8, &out, &seq0),
                "gen-guard skips an install that raced an invalidate");
    TEST_EXPECT_EQ(g_larder.attr_install_skips, 1ull, "one guard skip counted");

    // A fresh snapshot after the invalidate installs normally.
    u64 seq2 = larder_gen_snapshot(&g_larder);
    larder_attr_install(&g_larder, seq2, 8, 5, &in);
    TEST_ASSERT(larder_attr_serve(&g_larder, 8, &out, &seq0),
                "a fresh-snapshot install succeeds");
}

// Root's qid.path is 0x0 -- the `valid` bit (not path==0) marks empty, so root
// caches like any other key.
void test_larder_root_qid_zero(void) {
    larder_init(&g_larder);
    struct t_stat out;
    u64 seq0 = 0;
    TEST_ASSERT(!larder_attr_serve(&g_larder, 0, &out, &seq0),
                "qid.path 0 misses on an empty cache (not a false hit)");

    struct t_stat in = mk_stat(040755);   // a dir mode
    larder_attr_install(&g_larder, seq0, 0, 3, &in);
    TEST_ASSERT(larder_attr_serve(&g_larder, 0, &out, &seq0),
                "root (qid.path 0) serves after install");
    TEST_EXPECT_EQ(out.mode, 040755u, "root's attr round-trips");
}

// Overwrite (revalidate-by-overwrite): a second install for the same qid.path
// replaces the entry (latest content-version wins), never duplicates it.
void test_larder_overwrite_wins(void) {
    larder_init(&g_larder);
    struct t_stat a = mk_stat(0111);
    struct t_stat b = mk_stat(0222);
    larder_attr_install(&g_larder, larder_gen_snapshot(&g_larder), 9, 1, &a);
    larder_attr_install(&g_larder, larder_gen_snapshot(&g_larder), 9, 2, &b);

    struct t_stat out;
    u64 seq0 = 0;
    TEST_ASSERT(larder_attr_serve(&g_larder, 9, &out, &seq0), "hit after overwrite");
    TEST_EXPECT_EQ(out.mode, 0222u, "the latest install wins");

    // Exactly one valid entry for qid 9 (no duplicate slot).
    u32 count9 = 0;
    for (u32 i = 0; i < LARDER_ATTR_ENTRIES; i++)
        if (g_larder.attr[i].valid && g_larder.attr[i].qid_path == 9) count9++;
    TEST_EXPECT_EQ(count9, 1u, "overwrite reuses the slot (no duplicate)");
}

// Bounded (I-32): filling past capacity evicts the LRU victim and never exceeds
// LARDER_ATTR_ENTRIES valid entries.
void test_larder_eviction_bounded(void) {
    larder_init(&g_larder);
    struct t_stat s = mk_stat(0600);
    // Fill exactly to capacity with qid.paths 0..N-1 (lru order 0 oldest).
    for (u64 k = 0; k < LARDER_ATTR_ENTRIES; k++)
        larder_attr_install(&g_larder, larder_gen_snapshot(&g_larder), k, 1, &s);

    u32 valid = 0;
    for (u32 i = 0; i < LARDER_ATTR_ENTRIES; i++) if (g_larder.attr[i].valid) valid++;
    TEST_EXPECT_EQ(valid, (u32)LARDER_ATTR_ENTRIES, "cache is full at capacity");
    TEST_EXPECT_EQ(g_larder.attr_evictions, 0ull, "no eviction while filling");

    // One more entry evicts the LRU (qid 0 -- installed first, never re-served).
    larder_attr_install(&g_larder, larder_gen_snapshot(&g_larder),
                        (u64)LARDER_ATTR_ENTRIES, 1, &s);
    TEST_EXPECT_EQ(g_larder.attr_evictions, 1ull, "one eviction on overflow");

    valid = 0;
    for (u32 i = 0; i < LARDER_ATTR_ENTRIES; i++) if (g_larder.attr[i].valid) valid++;
    TEST_EXPECT_EQ(valid, (u32)LARDER_ATTR_ENTRIES, "still bounded at capacity");

    struct t_stat out;
    u64 seq0 = 0;
    TEST_ASSERT(!larder_attr_serve(&g_larder, 0, &out, &seq0),
                "the LRU victim (qid 0) was evicted");
    TEST_ASSERT(larder_attr_serve(&g_larder, LARDER_ATTR_ENTRIES, &out, &seq0),
                "the newest entry is present");
}
