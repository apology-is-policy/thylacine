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
// L1d dentry sub-cache. Maps to fs_cache.tla's Read + OwnWrite subset (NO Open
// gate -- a name-binding has no content-version to revalidate; the parent's
// si_cvers does not track a dirent change, so own-write invalidation is the sole
// coherence mechanism).
void test_larder_dentry_serve(void);
void test_larder_dentry_serve_miss(void);
void test_larder_dentry_negative(void);
void test_larder_dentry_multi_hop(void);
void test_larder_dentry_partial_chain_bails(void);
void test_larder_dentry_attr_miss_bails(void);
void test_larder_dentry_invalidate_parent(void);
void test_larder_dentry_gen_guard(void);
void test_larder_dentry_name_too_long(void);
void test_larder_dentry_bounded(void);

// The Larder is ~56 KiB (a 256-entry attr + a 256-entry dentry array) -- file
// scope, not stack.
static struct larder g_larder;

// Build a t_stat whose `mode` is a distinct marker so a serve round-trips it.
static struct t_stat mk_stat(u32 mode_marker) {
    struct t_stat s;
    for (size_t i = 0; i < sizeof(s); i++) ((u8 *)&s)[i] = 0;
    s.mode = mode_marker;
    return s;
}

static size_t nlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

// Install a positive dentry (parent, name) -> child AND the child's attr, so a
// positive serve round-trips (the serve reads the reply attr from the attr
// sub-cache). Neither install bumps gen, so one snapshot covers both.
static void put_pos(u64 parent, const char *name, u64 child, u32 mode) {
    u64 seq = larder_gen_snapshot(&g_larder);
    struct t_stat a = mk_stat(mode);
    a.qid_path = child;
    a.qid_vers = 1;
    a.qid_type = 0;
    larder_attr_install(&g_larder, seq, child, /*cvers=*/1, &a);
    larder_dentry_install(&g_larder, seq, parent, name, nlen(name), child,
                          /*negative=*/false);
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

// -- L1d dentry sub-cache ------------------------------------------------------

// Serve (Read): a cached positive dentry + the child's attr resolve a one-hop
// walk RPC-free.
void test_larder_dentry_serve(void) {
    larder_init(&g_larder);
    put_pos(/*parent=*/100, "foo", /*child=*/200, 0644);

    const char *names[] = {"foo"};
    size_t      lens[]  = {3};
    struct t_stat sts[4];
    int  nres = -1;
    bool miss = true;
    bool ok = larder_walk_serve(&g_larder, 100, names, lens, 1, sts, &nres, &miss);
    TEST_ASSERT(ok, "cached positive dentry serves the walk");
    TEST_ASSERT(!miss, "positive serve is not a miss");
    TEST_ASSERT(nres == 1, "one component resolved");
    TEST_EXPECT_EQ(sts[0].qid_path, 200ull, "served qid.path is the child");
    TEST_EXPECT_EQ(sts[0].mode, 0644u, "served attr round-trips from the attr cache");
    TEST_EXPECT_EQ(g_larder.dentry_hits, 1ull, "one dentry hit counted");
}

void test_larder_dentry_serve_miss(void) {
    larder_init(&g_larder);
    const char *names[] = {"foo"};
    size_t      lens[]  = {3};
    struct t_stat sts[4];
    int nres = 7; bool miss = false;
    TEST_ASSERT(!larder_walk_serve(&g_larder, 100, names, lens, 1, sts, &nres, &miss),
                "empty dentry cache bails to the RPC");
    TEST_EXPECT_EQ(g_larder.dentry_misses, 1ull, "one dentry miss counted");
}

// A NEGATIVE dentry serves the walk-miss RPC-free (the failed-lookup storm win).
void test_larder_dentry_negative(void) {
    larder_init(&g_larder);
    larder_dentry_install(&g_larder, larder_gen_snapshot(&g_larder), 100,
                          "nope", 4, 0, /*negative=*/true);
    const char *names[] = {"nope"};
    size_t      lens[]  = {4};
    struct t_stat sts[4];
    int nres = -1; bool miss = false;
    bool ok = larder_walk_serve(&g_larder, 100, names, lens, 1, sts, &nres, &miss);
    TEST_ASSERT(ok, "negative dentry serves");
    TEST_ASSERT(miss, "negative serve reports a miss");
    TEST_ASSERT(nres == 0, "the walk misses at component 0 (nothing resolved)");
    TEST_EXPECT_EQ(g_larder.dentry_neg_hits, 1ull, "one negative hit counted");
}

// A multi-hop run serves fully when every hop's dentry + attr is cached; the
// chain advances cur = base -> child0 -> child1.
void test_larder_dentry_multi_hop(void) {
    larder_init(&g_larder);
    put_pos(100, "a", 200, 040755);   // a dir
    put_pos(200, "b", 300, 0644);     // a file under it
    const char *nm[] = {"a", "b"};
    size_t      ln[] = {1, 1};
    struct t_stat sts[4];
    int nres = -1; bool miss = true;
    bool ok = larder_walk_serve(&g_larder, 100, nm, ln, 2, sts, &nres, &miss);
    TEST_ASSERT(ok && !miss && nres == 2, "two-hop chain serves fully");
    TEST_EXPECT_EQ(sts[0].qid_path, 200ull, "hop 0 -> a (qid 200)");
    TEST_EXPECT_EQ(sts[1].qid_path, 300ull, "hop 1 -> b (qid 300)");
    TEST_EXPECT_EQ(g_larder.dentry_hits, 2ull, "two positive hops counted");
}

// A chain with an uncached intermediate hop bails to the RPC (a partial prefix
// serve cannot skip the whole-run RPC).
void test_larder_dentry_partial_chain_bails(void) {
    larder_init(&g_larder);
    put_pos(100, "a", 200, 040755);   // hop 0 cached; (200,"b") is NOT
    const char *nm[] = {"a", "b"};
    size_t      ln[] = {1, 1};
    struct t_stat sts[4];
    int nres = -1; bool miss = true;
    TEST_ASSERT(!larder_walk_serve(&g_larder, 100, nm, ln, 2, sts, &nres, &miss),
                "a chain with an uncached hop bails to the RPC");
}

// A positive dentry whose child attr is NOT cached bails (a served qid must
// carry a coherent attr from the attr sub-cache).
void test_larder_dentry_attr_miss_bails(void) {
    larder_init(&g_larder);
    larder_dentry_install(&g_larder, larder_gen_snapshot(&g_larder), 100,
                          "foo", 3, 200, /*negative=*/false);   // no attr(200)
    const char *nm[] = {"foo"};
    size_t      ln[] = {3};
    struct t_stat sts[4];
    int nres = -1; bool miss = true;
    TEST_ASSERT(!larder_walk_serve(&g_larder, 100, nm, ln, 1, sts, &nres, &miss),
                "a positive dentry without a cached child attr bails to the RPC");
}

// THE GROUND-TRUTH CORE: own-write invalidation is the SOLE dentry coherence
// mechanism. A create in a directory (which does NOT bump the parent's si_cvers
// in Stratum) must drop the parent's cached dentries -- else a stale NEGATIVE
// entry would serve ENOENT for the now-existing file. Non-vacuous (fails if
// invalidate-parent does not drop the negative).
void test_larder_dentry_invalidate_parent(void) {
    larder_init(&g_larder);
    larder_dentry_install(&g_larder, larder_gen_snapshot(&g_larder), 100,
                          "foo", 3, 0, /*negative=*/true);   // "foo" absent in 100
    put_pos(100, "bar", 200, 0644);                          // "bar" -> 200 in 100

    const char *nf[] = {"foo"}; size_t lf[] = {3};
    const char *nb[] = {"bar"}; size_t lb[] = {3};
    struct t_stat sts[4]; int nres; bool miss;

    TEST_ASSERT(larder_walk_serve(&g_larder, 100, nf, lf, 1, sts, &nres, &miss) &&
                miss && nres == 0, "negative 'foo' serves the miss before the create");
    TEST_ASSERT(larder_walk_serve(&g_larder, 100, nb, lb, 1, sts, &nres, &miss) &&
                !miss && nres == 1, "positive 'bar' serves the hit before the create");

    // A create in dir 100 (own-write): drop ALL of 100's cached dentries.
    larder_dentry_invalidate_parent(&g_larder, 100);

    TEST_ASSERT(!larder_walk_serve(&g_larder, 100, nf, lf, 1, sts, &nres, &miss),
                "invalidate-parent drops the stale NEGATIVE dentry (no ENOENT for a new file)");
    TEST_ASSERT(!larder_walk_serve(&g_larder, 100, nb, lb, 1, sts, &nres, &miss),
                "invalidate-parent drops the POSITIVE dentry too");
    // The child's attr survives -- invalidate-parent drops only dentries.
    struct t_stat out; u64 s0 = 0;
    TEST_ASSERT(larder_attr_serve(&g_larder, 200, &out, &s0),
                "invalidate-parent leaves the child's attr (dentry-only drop)");
    TEST_ASSERT(g_larder.dentry_invalidations >= 2ull, "both dentries dropped");
}

// The gen guard covers dentry populates: an install whose RPC raced an
// invalidate-parent (which bumps the shared gen) is skipped.
void test_larder_dentry_gen_guard(void) {
    larder_init(&g_larder);
    u64 seq = larder_gen_snapshot(&g_larder);               // "before the RPC"
    larder_dentry_invalidate_parent(&g_larder, 999);        // a concurrent own-write bumps gen
    larder_dentry_install(&g_larder, seq, 100, "foo", 3, 200, false);  // stale install

    const char *nf[] = {"foo"}; size_t lf[] = {3};
    struct t_stat sts[4]; int nres; bool miss;
    TEST_ASSERT(!larder_walk_serve(&g_larder, 100, nf, lf, 1, sts, &nres, &miss),
                "gen-guard skips a dentry install that raced an invalidate");
    TEST_EXPECT_EQ(g_larder.dentry_install_skips, 1ull, "one dentry guard skip counted");
}

// A component longer than LARDER_DENTRY_NAME_MAX is not cached (fail-safe to the
// RPC) -- neither installed nor matched.
void test_larder_dentry_name_too_long(void) {
    larder_init(&g_larder);
    char longname[LARDER_DENTRY_NAME_MAX + 8];
    for (size_t i = 0; i < sizeof(longname); i++) longname[i] = 'x';
    larder_dentry_install(&g_larder, larder_gen_snapshot(&g_larder), 100,
                          longname, LARDER_DENTRY_NAME_MAX + 1, 200, false);
    u32 cnt = 0;
    for (u32 i = 0; i < LARDER_DENTRY_ENTRIES; i++)
        if (g_larder.dentry[i].valid) cnt++;
    TEST_EXPECT_EQ(cnt, 0u, "a too-long component is not cached");
}

// Bounded (I-32): filling past capacity evicts LRU victims; never exceeds
// LARDER_DENTRY_ENTRIES valid entries.
void test_larder_dentry_bounded(void) {
    larder_init(&g_larder);
    for (u64 k = 0; k < (u64)LARDER_DENTRY_ENTRIES + 8u; k++)
        larder_dentry_install(&g_larder, larder_gen_snapshot(&g_larder),
                              /*parent=*/k, "x", 1, /*child=*/k + 1, false);
    u32 valid = 0;
    for (u32 i = 0; i < LARDER_DENTRY_ENTRIES; i++)
        if (g_larder.dentry[i].valid) valid++;
    TEST_EXPECT_EQ(valid, (u32)LARDER_DENTRY_ENTRIES, "dentry cache bounded at capacity");
    TEST_ASSERT(g_larder.dentry_evictions >= 8ull, "evictions fired on overflow");
}
