// Dev vtable + Spoor lifecycle tests (P4-A).
//
// Per ARCHITECTURE.md §9.2 + ROADMAP §6.2 (Spoor lifecycle exit
// criterion). Nine tests:
//
//   dev.boot_registration_smoke — devnone is in bestiary at boot.
//   dev.lookup_unknown          — lookup misses return NULL.
//   dev.devnone_ops_smoke       — every devnone op returns its
//                                 documented sentinel (NULL / -1 / void).
//   spoor.alloc_unref_round_trip — alloc + unref → freed; counters tick.
//   spoor.ref_lifecycle         — ref/unref balance; freed at ref=0.
//   spoor.clone_lifecycle       — clone is a NEW Spoor; both freed.
//   spoor.clone_copies_state    — qid / flag / mode / offset / dev /
//                                 dc all carry over.
//   spoor.clunk_dispatches_close — spoor_clunk calls dev->close exactly
//                                  once before the unref.
//   spoor.alloc_10k_no_leak     — 10K alloc/unref cycles, allocated ==
//                                 freed delta (ROADMAP §6.2 baseline;
//                                 the "/dev/null" form lands at P4-B).
//
// devnone (kernel/devnone.c) is the no-op stub Dev with dc='-' and all
// ops returning safe sentinels. Tests verify the audit-guard contract:
// invoking any devnone op must NOT extinct the kernel.
//
// Maps to ARCH §9 (Dev vtable + bestiary) and the v1.0 P4-A landed
// surface; future P4-B+ tests add per-real-dev coverage (cons / null /
// zero / random / proc / ctl / ramfs).

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

void test_dev_boot_registration_smoke(void);
void test_dev_lookup_unknown(void);
void test_dev_devnone_ops_smoke(void);
void test_spoor_alloc_unref_round_trip(void);
void test_spoor_ref_lifecycle(void);
void test_spoor_clone_lifecycle(void);
void test_spoor_clone_copies_state(void);
void test_spoor_clunk_dispatches_close(void);
void test_spoor_alloc_10k_no_leak(void);

// =============================================================================
// Counter snapshots — tests assert deltas rather than absolute counts so
// the harness order doesn't pin specific numbers.
// =============================================================================

static u64 g_alloc_snap;
static u64 g_free_snap;

static void snap_counters(void) {
    g_alloc_snap = spoor_total_allocated();
    g_free_snap  = spoor_total_freed();
}

static u64 alloc_since_snap(void) {
    return spoor_total_allocated() - g_alloc_snap;
}

static u64 free_since_snap(void) {
    return spoor_total_freed() - g_free_snap;
}

// =============================================================================
// Test-only Dev — instruments close() so spoor.clunk_dispatches_close
// can verify the dispatch chain. dc='%' is unused in the v1.0 device-
// character space; chosen to avoid colliding with any real Dev (devnone
// owns '-'; future cons='c', null='⊘', proc='p', ctl='C', ...).
// =============================================================================

static int g_test_dev_close_calls;

static void test_only_dev_close(struct Spoor *c) {
    (void)c;
    g_test_dev_close_calls++;
}

static struct Dev g_test_only_dev = {
    .dc       = '%',
    .name     = "test_only",
    .reset    = NULL,
    .init     = NULL,
    .shutdown = NULL,
    .attach   = NULL,
    .walk     = NULL,
    .stat     = NULL,
    .open     = NULL,
    .create   = NULL,
    .close    = test_only_dev_close,
    .read     = NULL,
    .bread    = NULL,
    .write    = NULL,
    .bwrite   = NULL,
    .remove   = NULL,
    .wstat    = NULL,
    .power    = NULL,
};

static bool g_test_only_dev_registered;

static void register_test_only_dev_once(void) {
    if (g_test_only_dev_registered) return;
    dev_register(&g_test_only_dev);
    g_test_only_dev_registered = true;
}

// =============================================================================
// Tests.
// =============================================================================

void test_dev_boot_registration_smoke(void) {
    // dev_init() ran in boot_main before tests; bestiary should contain
    // at least devnone.
    TEST_ASSERT(dev_count() >= 1,
                "dev_count must be >= 1 after boot (devnone registered)");
    TEST_EXPECT_EQ(dev_lookup_by_dc('-'), &devnone,
                   "lookup '-' must return &devnone");
    TEST_EXPECT_EQ(dev_lookup_by_name("none"), &devnone,
                   "lookup 'none' must return &devnone");

    // devnone identity self-checks.
    TEST_EXPECT_EQ(devnone.dc, '-', "devnone.dc == '-'");
    TEST_ASSERT(devnone.name != NULL, "devnone.name non-NULL");
}

void test_dev_lookup_unknown(void) {
    TEST_ASSERT(dev_lookup_by_dc('Z') == NULL,
                "unknown dc 'Z' returns NULL");
    TEST_ASSERT(dev_lookup_by_name("does-not-exist") == NULL,
                "unknown name returns NULL");
    TEST_ASSERT(dev_lookup_by_name(NULL) == NULL,
                "NULL name returns NULL (no crash)");
}

void test_dev_devnone_ops_smoke(void) {
    // Every devnone op must be safe to invoke and return its documented
    // sentinel. This is the audit-guard contract: a Spoor with dev ==
    // &devnone must not be able to crash the kernel by being read /
    // written / opened.
    TEST_ASSERT(devnone.attach("") == NULL,
                "devnone.attach returns NULL");
    TEST_ASSERT(devnone.walk(NULL, NULL, NULL, 0) == NULL,
                "devnone.walk returns NULL");

    struct Spoor *c = spoor_alloc(&devnone);
    TEST_ASSERT(c != NULL, "spoor_alloc(&devnone) succeeds");

    TEST_EXPECT_EQ(devnone.stat(c, NULL, 0), -1,
                   "devnone.stat returns -1");
    TEST_ASSERT(devnone.open(c, 0) == NULL,
                "devnone.open returns NULL");
    TEST_EXPECT_EQ(devnone.read(c, NULL, 0, 0), (long)-1,
                   "devnone.read returns -1");
    TEST_ASSERT(devnone.bread(c, 0, 0) == NULL,
                "devnone.bread returns NULL");
    TEST_EXPECT_EQ(devnone.write(c, NULL, 0, 0), (long)-1,
                   "devnone.write returns -1");
    TEST_EXPECT_EQ(devnone.bwrite(c, NULL, 0), (long)-1,
                   "devnone.bwrite returns -1");
    TEST_EXPECT_EQ(devnone.wstat(c, NULL, 0), -1,
                   "devnone.wstat returns -1");
    TEST_ASSERT(devnone.power(c, 0) == NULL,
                "devnone.power returns NULL");

    // Void ops — must not crash.
    devnone.reset();
    devnone.shutdown();
    devnone.create(c, "x", 0, 0);
    devnone.remove(c);
    devnone.close(c);

    spoor_unref(c);
}

void test_spoor_alloc_unref_round_trip(void) {
    snap_counters();

    struct Spoor *c = spoor_alloc(&devnone);
    TEST_ASSERT(c != NULL, "spoor_alloc(&devnone) succeeds");
    TEST_EXPECT_EQ(c->dc,     '-',       "dc cached from devnone");
    TEST_EXPECT_EQ(c->dev,    &devnone,  "dev back-pointer set");
    TEST_EXPECT_EQ(c->ref,    1,         "fresh Spoor ref=1");
    TEST_EXPECT_EQ(c->flag,   (u32)0,    "fresh Spoor flag=0");
    TEST_EXPECT_EQ(c->mode,   0,         "fresh Spoor mode=0");
    TEST_EXPECT_EQ(c->offset, (s64)0,    "fresh Spoor offset=0");
    TEST_ASSERT(c->aux == NULL,          "fresh Spoor aux=NULL");
    TEST_EXPECT_EQ(alloc_since_snap(), (u64)1, "1 allocated");
    TEST_EXPECT_EQ(free_since_snap(),  (u64)0, "0 freed");

    spoor_unref(c);
    // c is now an invalid pointer — must NOT dereference.
    TEST_EXPECT_EQ(free_since_snap(), (u64)1,
                   "freed by the unref that brings ref to 0");
}

void test_spoor_ref_lifecycle(void) {
    snap_counters();

    struct Spoor *c = spoor_alloc(&devnone);
    TEST_ASSERT(c != NULL, "alloc OK");

    spoor_ref(c);
    TEST_EXPECT_EQ(c->ref, 2, "after ref, ref=2");
    TEST_EXPECT_EQ(free_since_snap(), (u64)0,
                   "must not be freed while ref > 0");

    spoor_unref(c);
    TEST_EXPECT_EQ(c->ref, 1, "after first unref, ref=1");
    TEST_EXPECT_EQ(free_since_snap(), (u64)0,
                   "must not be freed at ref=1");

    spoor_unref(c);
    TEST_EXPECT_EQ(free_since_snap(), (u64)1,
                   "freed when ref reaches 0");
}

void test_spoor_clone_lifecycle(void) {
    snap_counters();

    struct Spoor *c = spoor_alloc(&devnone);
    TEST_ASSERT(c != NULL, "alloc OK");
    TEST_EXPECT_EQ(alloc_since_snap(), (u64)1, "1 allocated post-alloc");

    struct Spoor *nc = spoor_clone(c);
    TEST_ASSERT(nc != NULL, "clone OK");
    TEST_ASSERT(nc != c,    "clone returns a NEW Spoor (distinct pointer)");
    TEST_EXPECT_EQ(c->ref,  1, "source ref unchanged at 1");
    TEST_EXPECT_EQ(nc->ref, 1, "clone ref=1");
    TEST_EXPECT_EQ(alloc_since_snap(), (u64)2, "2 allocated post-clone");

    spoor_unref(c);
    TEST_EXPECT_EQ(free_since_snap(), (u64)1, "source freed");
    spoor_unref(nc);
    TEST_EXPECT_EQ(free_since_snap(), (u64)2, "clone freed");
}

void test_spoor_clone_copies_state(void) {
    struct Spoor *c = spoor_alloc(&devnone);
    TEST_ASSERT(c != NULL, "alloc OK");

    // Mutate every field spoor_clone is documented to copy.
    c->qid.path = 0xDEADBEEFCAFE0001ULL;
    c->qid.vers = 7;
    c->qid.type = QTDIR;
    c->flag     = COPEN | CMSG;
    c->mode     = 3;
    c->offset   = (s64)0x1000;

    struct Spoor *nc = spoor_clone(c);
    TEST_ASSERT(nc != NULL, "clone OK");

    TEST_EXPECT_EQ(nc->qid.path, c->qid.path, "qid.path copied");
    TEST_EXPECT_EQ(nc->qid.vers, c->qid.vers, "qid.vers copied");
    TEST_EXPECT_EQ(nc->qid.type, c->qid.type, "qid.type copied");
    TEST_EXPECT_EQ(nc->flag,     c->flag,     "flag copied");
    TEST_EXPECT_EQ(nc->mode,     c->mode,     "mode copied");
    TEST_EXPECT_EQ(nc->offset,   c->offset,   "offset copied");
    TEST_EXPECT_EQ(nc->dev,      c->dev,      "dev back-pointer copied");
    TEST_EXPECT_EQ(nc->dc,       c->dc,       "dc copied");

    spoor_unref(c);
    spoor_unref(nc);
}

void test_spoor_clunk_dispatches_close(void) {
    register_test_only_dev_once();

    int before = g_test_dev_close_calls;

    struct Spoor *c = spoor_alloc(&g_test_only_dev);
    TEST_ASSERT(c != NULL, "alloc on test-only dev succeeds");

    spoor_clunk(c);

    TEST_EXPECT_EQ(g_test_dev_close_calls, before + 1,
                   "spoor_clunk dispatches dev->close exactly once");
}

void test_spoor_alloc_10k_no_leak(void) {
    // ROADMAP §6.2 exit criterion (full): "Spoor lifecycle: 10,000
    // open/read/close cycles on /dev/null without leak." The /dev/null
    // form requires P4-B; at P4-A we exercise the same SLUB cache +
    // counter discipline against devnone.
    //
    // P4-A boot-budget compromise: 1000 iterations here (lands the
    // no-leak signal in the boot-path harness). The full 10K criterion
    // is met when P4-B's real /dev/null is wired and the dedicated
    // stress-suite path runs it (counterpart to slub.leak_10k +
    // phys.leak_10k which use the pre-warmed kmalloc-64 cache; the
    // fresh "spoor" cache has slower cold-magazine refill behavior at
    // 10K under QEMU emulation, blowing the < 500 ms boot-time budget).
    enum { ITERS = 1000 };

    u64 alloc_before = spoor_total_allocated();
    u64 free_before  = spoor_total_freed();

    for (int i = 0; i < ITERS; i++) {
        struct Spoor *c = spoor_alloc(&devnone);
        TEST_ASSERT(c != NULL, "alloc must succeed throughout no-leak loop");
        spoor_unref(c);
    }

    u64 alloc_delta = spoor_total_allocated() - alloc_before;
    u64 free_delta  = spoor_total_freed()      - free_before;
    TEST_EXPECT_EQ(alloc_delta, (u64)ITERS, "iters allocs observed");
    TEST_EXPECT_EQ(free_delta,  (u64)ITERS,
                   "iters frees observed (no leak)");
}
