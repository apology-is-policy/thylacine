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
void test_larder_dentry_invalidate_name(void);
void test_larder_dentry_gen_guard(void);
void test_larder_dentry_name_too_long(void);
void test_larder_dentry_bounded(void);
// L1e page sub-cache. Maps to fs_cache.tla's Read + Open + OwnWrite on content
// tokens keyed (qid.path, page_index): serve = Read (cvers-gated), install =
// Open/Refetch (gen-guarded), invalidate = OwnWrite (drop + bump gen). Each test
// that installs pages calls larder_destroy at the end to free the lazily-allocated
// buffers (larder_init on the next test zeroes the slot pointers, so a prior
// buffer must be freed first -- production never re-inits a used larder).
void test_larder_page_serve(void);
void test_larder_page_serve_miss(void);
void test_larder_page_offset(void);
void test_larder_page_cvers_mismatch(void);
void test_larder_page_partial(void);
void test_larder_page_invalidate(void);
void test_larder_page_invalidate_multifile(void);
void test_larder_page_gen_guard(void);
void test_larder_pages_snapshot_gen_witness(void);
void test_larder_page_overwrite(void);
void test_larder_page_bounded(void);
void test_larder_page_destroy_frees(void);

// struct larder is now small (pointers + counters): all three sub-cache arrays are
// HEAP + lazy (attr 4096 / dentry 4096 / page 32768 entries; the page BUFFERS too),
// allocated on first install -- file scope, not stack.
static struct larder g_larder;

// Destroy-then-init between tests: all three sub-cache arrays are heap + lazy
// (the FID-LIFECYCLE re-size), so a bare re-init would leak the prior test's
// arrays. Destroy guards NULL, so the first call (boot-zeroed g_larder) no-ops.
static void larder_reset(void) {
    larder_destroy(&g_larder);
    larder_init(&g_larder);
}
// Page-sized scratch (the 16 KiB kstack won't hold two 4 KiB frames per test).
static u8 g_pgsrc[LARDER_PAGE_SIZE];
static u8 g_pgout[LARDER_PAGE_SIZE];

static void fill_pattern(u8 *b, u32 n, u8 seed) {
    for (u32 i = 0; i < n; i++) b[i] = (u8)((u32)seed + i);
}
static bool bytes_eq(const u8 *a, const u8 *b, u32 n) {
    for (u32 i = 0; i < n; i++) if (a[i] != b[i]) return false;
    return true;
}

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
    larder_reset();
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
    larder_reset();
    struct t_stat out;
    u64 seq0 = 999;   // must be overwritten by the miss
    bool hit = larder_attr_serve(&g_larder, 42, &out, &seq0);
    TEST_ASSERT(!hit, "empty cache misses");
    TEST_EXPECT_EQ(seq0, 0ull, "miss returns the current gen snapshot (0 here)");
    TEST_EXPECT_EQ(g_larder.attr_misses, 1ull, "one miss counted");
}

void test_larder_invalidate(void) {
    larder_reset();
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
    larder_reset();
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
    larder_reset();
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
    larder_reset();
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
    larder_reset();
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
    larder_reset();
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
    larder_reset();
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
    larder_reset();
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
    larder_reset();
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
    larder_reset();
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
    larder_reset();
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
void test_larder_dentry_invalidate_name(void) {
    larder_reset();
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

    // A create of "foo" in dir 100 (own-write on exactly (100,"foo")): drop ONLY
    // that binding. The "bar" sibling MUST survive -- the whole point of the L1d
    // narrowing (a whole-parent drop would evict it and force a needless re-walk,
    // the cold-band wga thrash). This assertion FAILS on the old whole-parent code.
    larder_dentry_invalidate_name(&g_larder, 100, "foo", 3);

    TEST_ASSERT(!larder_walk_serve(&g_larder, 100, nf, lf, 1, sts, &nres, &miss),
                "invalidate-name drops the stale NEGATIVE 'foo' (no ENOENT for a new file)");
    TEST_ASSERT(larder_walk_serve(&g_larder, 100, nb, lb, 1, sts, &nres, &miss) &&
                !miss && nres == 1,
                "invalidate-name PRESERVES the 'bar' sibling (the narrowing)");
    // The child's attr survives -- invalidate-name drops only the named dentry.
    struct t_stat out; u64 s0 = 0;
    TEST_ASSERT(larder_attr_serve(&g_larder, 200, &out, &s0),
                "invalidate-name leaves the child's attr (dentry-only drop)");
    TEST_ASSERT(g_larder.dentry_invalidations >= 1ull, "the named dentry dropped");
}

// The gen guard covers dentry populates: an install whose RPC raced an
// invalidate-parent (which bumps the shared gen) is skipped.
void test_larder_dentry_gen_guard(void) {
    larder_reset();
    u64 seq = larder_gen_snapshot(&g_larder);               // "before the RPC"
    larder_dentry_invalidate_name(&g_larder, 999, "x", 1);  // a concurrent own-write bumps gen
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
    larder_reset();
    char longname[LARDER_DENTRY_NAME_MAX + 8];
    for (size_t i = 0; i < sizeof(longname); i++) longname[i] = 'x';
    larder_dentry_install(&g_larder, larder_gen_snapshot(&g_larder), 100,
                          longname, LARDER_DENTRY_NAME_MAX + 1, 200, false);
    u32 cnt = 0;
    if (g_larder.dentry)   // heap + lazy: a declined install allocates nothing
        for (u32 i = 0; i < LARDER_DENTRY_ENTRIES; i++)
            if (g_larder.dentry[i].valid) cnt++;
    TEST_EXPECT_EQ(cnt, 0u, "a too-long component is not cached");
}

// Bounded (I-32): filling past capacity evicts LRU victims; never exceeds
// LARDER_DENTRY_ENTRIES valid entries.
void test_larder_dentry_bounded(void) {
    larder_reset();
    for (u64 k = 0; k < (u64)LARDER_DENTRY_ENTRIES + 8u; k++)
        larder_dentry_install(&g_larder, larder_gen_snapshot(&g_larder),
                              /*parent=*/k, "x", 1, /*child=*/k + 1, false);
    u32 valid = 0;
    for (u32 i = 0; i < LARDER_DENTRY_ENTRIES; i++)
        if (g_larder.dentry[i].valid) valid++;
    TEST_EXPECT_EQ(valid, (u32)LARDER_DENTRY_ENTRIES, "dentry cache bounded at capacity");
    TEST_ASSERT(g_larder.dentry_evictions >= 8ull, "evictions fired on overflow");
}

// -- L1e page sub-cache ---------------------------------------------------------

// Install a full page, serve it whole -- the bytes round-trip (fs_cache.tla Read).
void test_larder_page_serve(void) {
    larder_reset();
    fill_pattern(g_pgsrc, LARDER_PAGE_SIZE, 0x11);
    u64 seq = larder_gen_snapshot(&g_larder);
    larder_page_install(&g_larder, seq, /*qid=*/7, /*page=*/3, /*cvers=*/5,
                        g_pgsrc, LARDER_PAGE_SIZE);
    u64 s0 = 0;
    u32 got = larder_page_serve(&g_larder, 7, 3, /*page_off=*/0, LARDER_PAGE_SIZE,
                                /*want_cvers=*/5, g_pgout, &s0);
    TEST_EXPECT_EQ(got, (u32)LARDER_PAGE_SIZE, "page serve returns the full page");
    TEST_ASSERT(bytes_eq(g_pgout, g_pgsrc, LARDER_PAGE_SIZE),
                "page serve returns the cached bytes verbatim");
    TEST_EXPECT_EQ(g_larder.page_hits, 1ull, "one page hit counted");
    larder_destroy(&g_larder);
}

// A miss returns 0 and hands back the gen snapshot for the populate guard.
void test_larder_page_serve_miss(void) {
    larder_reset();
    u64 s0 = 999;
    u32 got = larder_page_serve(&g_larder, 7, 3, 0, LARDER_PAGE_SIZE, 5, g_pgout, &s0);
    TEST_EXPECT_EQ(got, 0u, "an empty page cache misses");
    TEST_EXPECT_EQ(s0, g_larder.gen, "miss returns the current gen for the guard");
    TEST_EXPECT_EQ(g_larder.page_misses, 1ull, "one page miss counted");
}

// A serve within a page honors page_off + `want`: offset 100, want 200 -> the 200
// bytes starting at src[100].
void test_larder_page_offset(void) {
    larder_reset();
    fill_pattern(g_pgsrc, LARDER_PAGE_SIZE, 0x40);
    larder_page_install(&g_larder, larder_gen_snapshot(&g_larder), 7, 3, 5,
                        g_pgsrc, LARDER_PAGE_SIZE);
    u64 s0 = 0;
    u32 got = larder_page_serve(&g_larder, 7, 3, /*page_off=*/100, /*want=*/200,
                                5, g_pgout, &s0);
    TEST_EXPECT_EQ(got, 200u, "an offset serve returns `want` bytes");
    TEST_ASSERT(bytes_eq(g_pgout, g_pgsrc + 100, 200),
                "an offset serve returns bytes from page_off");
    larder_destroy(&g_larder);
}

// The cvers gate (the close-to-open Open discipline): a page fetched at cvers 5 is
// NOT served to a reader whose fid version is 6 (a cross-open external write); it
// IS served at 5. (Within one client, own-write invalidation is the primary gate;
// cvers catches the cross-open change.)
void test_larder_page_cvers_mismatch(void) {
    larder_reset();
    fill_pattern(g_pgsrc, LARDER_PAGE_SIZE, 0x22);
    larder_page_install(&g_larder, larder_gen_snapshot(&g_larder), 7, 3,
                        /*cvers=*/5, g_pgsrc, LARDER_PAGE_SIZE);
    u64 s0 = 0;
    u32 stale = larder_page_serve(&g_larder, 7, 3, 0, LARDER_PAGE_SIZE,
                                  /*want_cvers=*/6, g_pgout, &s0);
    TEST_EXPECT_EQ(stale, 0u, "a cvers mismatch misses (cross-open write)");
    u32 fresh = larder_page_serve(&g_larder, 7, 3, 0, LARDER_PAGE_SIZE,
                                  /*want_cvers=*/5, g_pgout, &s0);
    TEST_EXPECT_EQ(fresh, (u32)LARDER_PAGE_SIZE, "the matching cvers serves");
    larder_destroy(&g_larder);
}

// A partial (small-file / EOF) page: valid_len 100. A serve within [0,100) returns
// up to valid_len (clamped); a serve AT/PAST valid_len misses (the caller refetches
// -- sound without an EOF determination). No hole is ever served.
void test_larder_page_partial(void) {
    larder_reset();
    fill_pattern(g_pgsrc, LARDER_PAGE_SIZE, 0x55);
    larder_page_install(&g_larder, larder_gen_snapshot(&g_larder), 7, 0, 5,
                        g_pgsrc, /*len=*/100);
    u64 s0 = 0;
    // off 50, want 200 -> only 50 bytes (valid_len - page_off), from src[50].
    u32 mid = larder_page_serve(&g_larder, 7, 0, /*page_off=*/50, /*want=*/200,
                                5, g_pgout, &s0);
    TEST_EXPECT_EQ(mid, 50u, "a partial page serves only up to valid_len");
    TEST_ASSERT(bytes_eq(g_pgout, g_pgsrc + 50, 50), "partial serve bytes correct");
    // off 0, want the whole page -> clamped to valid_len (100).
    u32 whole = larder_page_serve(&g_larder, 7, 0, 0, LARDER_PAGE_SIZE, 5, g_pgout, &s0);
    TEST_EXPECT_EQ(whole, 100u, "a whole-page want clamps to valid_len");
    // off AT valid_len -> miss (past the known content).
    u32 past = larder_page_serve(&g_larder, 7, 0, /*page_off=*/100, 8, 5, g_pgout, &s0);
    TEST_EXPECT_EQ(past, 0u, "a serve at/past valid_len misses");
    larder_destroy(&g_larder);
}

// OwnWrite: invalidate(qid_path) drops every page of the file + bumps gen.
void test_larder_page_invalidate(void) {
    larder_reset();
    fill_pattern(g_pgsrc, LARDER_PAGE_SIZE, 0x33);
    larder_page_install(&g_larder, larder_gen_snapshot(&g_larder), 7, 0, 5,
                        g_pgsrc, LARDER_PAGE_SIZE);
    larder_page_install(&g_larder, larder_gen_snapshot(&g_larder), 7, 1, 5,
                        g_pgsrc, LARDER_PAGE_SIZE);
    u64 gen_before = g_larder.gen;
    larder_page_invalidate(&g_larder, /*qid=*/7);
    TEST_ASSERT(g_larder.gen > gen_before, "invalidate bumps gen");
    u64 s0 = 0;
    TEST_EXPECT_EQ(larder_page_serve(&g_larder, 7, 0, 0, LARDER_PAGE_SIZE, 5, g_pgout, &s0),
                   0u, "page 0 dropped by own-write invalidate");
    TEST_EXPECT_EQ(larder_page_serve(&g_larder, 7, 1, 0, LARDER_PAGE_SIZE, 5, g_pgout, &s0),
                   0u, "page 1 dropped by own-write invalidate");
    larder_destroy(&g_larder);
}

// The F3 O(pages-of-file) invalidate (task #29): the page_qhash secondary index drops
// EVERY page of the written file and NO page of any OTHER file, and re-uses the freed
// slots cleanly (the qhash unlink kept the "in qhash IFF in page_hash" invariant).
void test_larder_page_invalidate_multifile(void) {
    larder_reset();
    fill_pattern(g_pgsrc, LARDER_PAGE_SIZE, 0x55);
    // File A (qid 7): pages 0,1,2.  File B (qid 8): pages 0,1.  Interleaved installs so
    // A's pages are NOT contiguous in the entry array (exercises the qbucket chain walk).
    larder_page_install(&g_larder, larder_gen_snapshot(&g_larder), 7, 0, 5, g_pgsrc, LARDER_PAGE_SIZE);
    larder_page_install(&g_larder, larder_gen_snapshot(&g_larder), 8, 0, 5, g_pgsrc, LARDER_PAGE_SIZE);
    larder_page_install(&g_larder, larder_gen_snapshot(&g_larder), 7, 1, 5, g_pgsrc, LARDER_PAGE_SIZE);
    larder_page_install(&g_larder, larder_gen_snapshot(&g_larder), 8, 1, 5, g_pgsrc, LARDER_PAGE_SIZE);
    larder_page_install(&g_larder, larder_gen_snapshot(&g_larder), 7, 2, 5, g_pgsrc, LARDER_PAGE_SIZE);
    u64 inv_before = g_larder.page_invalidations;
    larder_page_invalidate(&g_larder, /*qid=*/7);
    // Exactly file A's 3 pages dropped -- the qbucket walk found ALL of A and ONLY A.
    TEST_EXPECT_EQ(g_larder.page_invalidations - inv_before, 3ull,
                   "invalidate dropped exactly file A's 3 pages");
    u64 s0 = 0;
    TEST_EXPECT_EQ(larder_page_serve(&g_larder, 7, 0, 0, LARDER_PAGE_SIZE, 5, g_pgout, &s0), 0u, "A page 0 dropped");
    TEST_EXPECT_EQ(larder_page_serve(&g_larder, 7, 1, 0, LARDER_PAGE_SIZE, 5, g_pgout, &s0), 0u, "A page 1 dropped");
    TEST_EXPECT_EQ(larder_page_serve(&g_larder, 7, 2, 0, LARDER_PAGE_SIZE, 5, g_pgout, &s0), 0u, "A page 2 dropped");
    // File B UNTOUCHED -- the qid_path discrimination (the correctness the qbucket must keep).
    TEST_EXPECT_EQ(larder_page_serve(&g_larder, 8, 0, 0, LARDER_PAGE_SIZE, 5, g_pgout, &s0),
                   LARDER_PAGE_SIZE, "B page 0 survives A's invalidate");
    TEST_EXPECT_EQ(larder_page_serve(&g_larder, 8, 1, 0, LARDER_PAGE_SIZE, 5, g_pgout, &s0),
                   LARDER_PAGE_SIZE, "B page 1 survives A's invalidate");
    // A's freed slots re-install cleanly (proves the qhash unlink kept the invariant so a
    // CLOCK/free-cursor reuse of an invalidated slot re-links both indexes correctly).
    larder_page_install(&g_larder, larder_gen_snapshot(&g_larder), 7, 0, 6, g_pgsrc, LARDER_PAGE_SIZE);
    TEST_EXPECT_EQ(larder_page_serve(&g_larder, 7, 0, 0, LARDER_PAGE_SIZE, 6, g_pgout, &s0),
                   LARDER_PAGE_SIZE, "A page 0 re-installs cleanly after invalidate");
    // ...and the re-installed page is reachable through the QBUCKET (not just page_hash):
    // invalidate again must find + drop it (a re-link into page_hash but NOT page_qhash
    // would pass the serve above yet leak here -- the qhash-relink coverage, audit F3).
    u64 inv2 = g_larder.page_invalidations;
    larder_page_invalidate(&g_larder, /*qid=*/7);
    TEST_EXPECT_EQ(g_larder.page_invalidations - inv2, 1ull,
                   "the re-installed page is in the qbucket (invalidate drops it)");
    TEST_EXPECT_EQ(larder_page_serve(&g_larder, 7, 0, 0, LARDER_PAGE_SIZE, 6, g_pgout, &s0),
                   0u, "re-installed A page 0 dropped by the second invalidate");
    larder_destroy(&g_larder);
}

// The gen guard (the atomic-Open realization): an invalidate that raced the read
// (gen changed since the seq0 snapshot) skips the install -- the resurrection close.
void test_larder_page_gen_guard(void) {
    larder_reset();
    fill_pattern(g_pgsrc, LARDER_PAGE_SIZE, 0x44);
    u64 seq = larder_gen_snapshot(&g_larder);        // snapshot BEFORE the "read"
    larder_page_invalidate(&g_larder, /*other qid=*/99);  // races: bumps gen
    larder_page_install(&g_larder, seq, 7, 0, 5, g_pgsrc, LARDER_PAGE_SIZE);
    u64 s0 = 0;
    TEST_EXPECT_EQ(larder_page_serve(&g_larder, 7, 0, 0, LARDER_PAGE_SIZE, 5, g_pgout, &s0),
                   0u, "the raced install is skipped (gen guard)");
    TEST_EXPECT_EQ(g_larder.page_install_skips, 1ull, "one page install skip counted");
    larder_destroy(&g_larder);
}

// B1-audit F1 regression: the pages-snapshot GEN WITNESS. A stale-fid
// repopulate between a caller's coverage decision and its snapshot copy
// re-satisfies coverage at the OLD cvers with post-write bytes; the witness
// (seq0 captured before the decision, checked under the snapshot's lock
// hold) fails the snapshot closed on ANY intervening invalidate.
void test_larder_pages_snapshot_gen_witness(void) {
    larder_reset();
    fill_pattern(g_pgsrc, LARDER_PAGE_SIZE, 0x5A);
    // A fully-covered one-page file (qid 7, cvers 5).
    larder_page_install(&g_larder, larder_gen_snapshot(&g_larder), 7, 0, 5,
                        g_pgsrc, LARDER_PAGE_SIZE);
    static u8 snap[LARDER_PAGE_SIZE];

    // Fresh witness -> the snapshot serves.
    u64 seq_ok = larder_gen_snapshot(&g_larder);
    TEST_ASSERT(larder_pages_snapshot(&g_larder, 7, 5, LARDER_PAGE_SIZE, snap,
                                      seq_ok),
                "fresh witness -> snapshot serves");
    TEST_ASSERT(bytes_eq(snap, g_pgsrc, LARDER_PAGE_SIZE), "snapshot bytes");

    // The F1 interleave: capture the witness, then an own-write invalidate
    // (ANY file) + a stale-tagged repopulate land before the copy. The
    // coverage alone would pass (the page is back at cvers 5); the witness
    // must fail it.
    u64 seq_stale = larder_gen_snapshot(&g_larder);
    larder_page_invalidate(&g_larder, 7);                 // the own-write drop
    larder_page_install(&g_larder, larder_gen_snapshot(&g_larder), 7, 0, 5,
                        g_pgsrc, LARDER_PAGE_SIZE);       // the stale-fid repopulate
    TEST_ASSERT(larder_pages_cover(&g_larder, 7, 5, LARDER_PAGE_SIZE),
                "coverage alone re-satisfied (the F1 hole)");
    TEST_ASSERT(!larder_pages_snapshot(&g_larder, 7, 5, LARDER_PAGE_SIZE, snap,
                                       seq_stale),
                "stale witness -> snapshot FAILS closed (the F1 fix)");

    // A fresh re-capture serves again (the fallback path's next attempt).
    TEST_ASSERT(larder_pages_snapshot(&g_larder, 7, 5, LARDER_PAGE_SIZE, snap,
                                      larder_gen_snapshot(&g_larder)),
                "fresh re-capture serves");
    larder_destroy(&g_larder);
}

// Overwrite (revalidate-by-overwrite): re-installing (qid,page) replaces the bytes
// in place (the buffer is reused, no leak).
void test_larder_page_overwrite(void) {
    larder_reset();
    fill_pattern(g_pgsrc, LARDER_PAGE_SIZE, 0xA0);
    larder_page_install(&g_larder, larder_gen_snapshot(&g_larder), 7, 3, 5,
                        g_pgsrc, LARDER_PAGE_SIZE);
    fill_pattern(g_pgsrc, LARDER_PAGE_SIZE, 0xB0);   // new content, same key
    larder_page_install(&g_larder, larder_gen_snapshot(&g_larder), 7, 3, 5,
                        g_pgsrc, LARDER_PAGE_SIZE);
    u64 s0 = 0;
    u32 got = larder_page_serve(&g_larder, 7, 3, 0, LARDER_PAGE_SIZE, 5, g_pgout, &s0);
    TEST_EXPECT_EQ(got, (u32)LARDER_PAGE_SIZE, "overwrite serves");
    TEST_ASSERT(bytes_eq(g_pgout, g_pgsrc, LARDER_PAGE_SIZE),
                "overwrite replaces the cached bytes");
    larder_destroy(&g_larder);
}

// Bounded (I-32): filling past LARDER_PAGE_ENTRIES evicts LRU victims; the last
// page installed is present, an early one evicted; valid slots never exceed
// capacity. Then destroy frees the buffers.
void test_larder_page_bounded(void) {
    larder_reset();
    fill_pattern(g_pgsrc, LARDER_PAGE_SIZE, 0x01);
    u32 over = LARDER_PAGE_ENTRIES + 50u;
    for (u32 k = 0; k < over; k++)
        larder_page_install(&g_larder, larder_gen_snapshot(&g_larder),
                            /*qid=*/7, /*page=*/k, /*cvers=*/5, g_pgsrc, 64);
    u32 valid = 0;
    for (u32 i = 0; i < LARDER_PAGE_ENTRIES; i++)
        if (g_larder.page[i].valid) valid++;
    TEST_EXPECT_EQ(valid, (u32)LARDER_PAGE_ENTRIES, "page cache bounded at capacity");
    TEST_ASSERT(g_larder.page_evictions >= 50ull, "evictions fired on overflow");
    u64 s0 = 0;
    TEST_ASSERT(larder_page_serve(&g_larder, 7, over - 1, 0, 64, 5, g_pgout, &s0) == 64u,
                "the most-recent page is present");
    TEST_EXPECT_EQ(larder_page_serve(&g_larder, 7, 0, 0, 64, 5, g_pgout, &s0), 0u,
                   "an early page was evicted (LRU)");
    larder_destroy(&g_larder);
}

// destroy frees the buffers + drops the entries: a serve after destroy misses.
void test_larder_page_destroy_frees(void) {
    larder_reset();
    fill_pattern(g_pgsrc, LARDER_PAGE_SIZE, 0x77);
    larder_page_install(&g_larder, larder_gen_snapshot(&g_larder), 7, 0, 5,
                        g_pgsrc, LARDER_PAGE_SIZE);
    larder_destroy(&g_larder);
    u64 s0 = 0;
    TEST_EXPECT_EQ(larder_page_serve(&g_larder, 7, 0, 0, LARDER_PAGE_SIZE, 5, g_pgout, &s0),
                   0u, "destroy drops the page (serve misses)");
    // A fresh init + install after destroy still works (buffers re-alloc lazily).
    larder_reset();
    larder_page_install(&g_larder, larder_gen_snapshot(&g_larder), 7, 0, 5,
                        g_pgsrc, LARDER_PAGE_SIZE);
    TEST_EXPECT_EQ(larder_page_serve(&g_larder, 7, 0, 0, LARDER_PAGE_SIZE, 5, g_pgout, &s0),
                   (u32)LARDER_PAGE_SIZE, "re-init + install after destroy works");
    larder_destroy(&g_larder);
}
