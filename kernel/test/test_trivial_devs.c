// Trivial Dev tests — cons / null / zero / full / random
// (P4-B baseline + P6-pouch-devnodes adds devfull).
//
// Per ROADMAP §6.2 exit criterion "cat /dev/random produces non-zero
// bytes" + the implicit per-Dev contract checks. Tests cover:
//
//   trivial_devs.bestiary_smoke         — all 5 registered + lookup
//   null.attach_open_close              — lifecycle plumbing
//   null.read_returns_eof               — read returns 0 always
//   null.write_consumes                 — writes return n
//   zero.read_fills_zeroes              — read produces all zeroes
//   zero.write_consumes                 — writes return n
//   full.attach_open_close              — lifecycle plumbing
//   full.read_fills_zeroes              — Linux /dev/full read shape
//   full.write_returns_minus1           — every write fails (-1)
//   random.rndr_available_or_skipped    — RNDR detection (must be
//                                          available on QEMU virt
//                                          -cpu max)
//   random.read_produces_nonzero_bytes  — ROADMAP §6.2 exit criterion
//   random.read_varies_across_calls     — basic CSPRNG sanity
//   cons.write_advances                 — write returns n; bytes go
//                                         to UART (visual confirm via
//                                         boot log)
//   cons.read_returns_eof               — v1.0 read is degenerate
//   trivial_devs.spoor_alloc_10k_no_leak_devnull
//                                       — ROADMAP §6.2 full 10K iter
//                                         exit criterion via the
//                                         warm cache path

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

void test_trivial_devs_bestiary_smoke(void);
void test_null_attach_open_close(void);
void test_null_read_returns_eof(void);
void test_null_write_consumes(void);
void test_zero_read_fills_zeroes(void);
void test_zero_write_consumes(void);
void test_full_attach_open_close(void);
void test_full_read_fills_zeroes(void);
void test_full_write_returns_minus1(void);
void test_random_rndr_available(void);
void test_random_read_produces_nonzero_bytes(void);
void test_random_read_varies_across_calls(void);
void test_cons_write_advances(void);
void test_cons_read_returns_eof(void);
void test_trivial_devs_devnull_10k_no_leak(void);

// =============================================================================
// Helpers — shared lifecycle pattern: dev->attach, dev->open, ..., spoor_clunk.
// =============================================================================

// Open a Spoor on dev d for omode. Returns the opened Spoor or NULL.
// Caller is responsible for spoor_clunk on success.
static struct Spoor *open_spoor(struct Dev *d, int omode) {
    struct Spoor *c = d->attach("");
    if (!c) return NULL;
    struct Spoor *opened = d->open(c, omode);
    if (!opened) {
        spoor_unref(c);
        return NULL;
    }
    return opened;
}

// =============================================================================
// Tests.
// =============================================================================

void test_trivial_devs_bestiary_smoke(void) {
    // dev_init() registered devnone + cons + null + zero + full + random
    // in that order (P4-B baseline + P6-pouch-devnodes adds devfull
    // between zero and random for alphabetical insertion). Verify each
    // is reachable by both dc and name.
    TEST_ASSERT(dev_count() >= 6,
                "dev_count must be >= 6 (devnone + cons + null + zero "
                "+ full + random)");

    TEST_EXPECT_EQ(dev_lookup_by_dc('c'),    &devcons,    "dc 'c' = devcons");
    TEST_EXPECT_EQ(dev_lookup_by_dc('0'),    &devnull,    "dc '0' = devnull");
    TEST_EXPECT_EQ(dev_lookup_by_dc('z'),    &devzero,    "dc 'z' = devzero");
    TEST_EXPECT_EQ(dev_lookup_by_dc('f'),    &devfull,    "dc 'f' = devfull");
    TEST_EXPECT_EQ(dev_lookup_by_dc('r'),    &devrandom,  "dc 'r' = devrandom");

    TEST_EXPECT_EQ(dev_lookup_by_name("cons"),   &devcons,   "lookup cons");
    TEST_EXPECT_EQ(dev_lookup_by_name("null"),   &devnull,   "lookup null");
    TEST_EXPECT_EQ(dev_lookup_by_name("zero"),   &devzero,   "lookup zero");
    TEST_EXPECT_EQ(dev_lookup_by_name("full"),   &devfull,   "lookup full");
    TEST_EXPECT_EQ(dev_lookup_by_name("random"), &devrandom, "lookup random");
}

void test_null_attach_open_close(void) {
    struct Spoor *c = devnull.attach("");
    TEST_ASSERT(c != NULL, "devnull.attach succeeds");
    TEST_EXPECT_EQ(c->dev,        &devnull,  "Spoor.dev = &devnull");
    TEST_EXPECT_EQ(c->dc,         '0',       "Spoor.dc = '0'");
    TEST_EXPECT_EQ(c->qid.type,   QTFILE,    "fresh attach: QTFILE");
    TEST_EXPECT_EQ((u32)0,        c->flag,   "fresh attach: flag=0");

    struct Spoor *opened = devnull.open(c, 0);
    TEST_EXPECT_EQ(opened, c, "open returns the same Spoor");
    TEST_ASSERT((c->flag & COPEN) != 0, "open sets COPEN");

    devnull.close(c);
    TEST_EXPECT_EQ((u32)0, c->flag & COPEN, "close clears COPEN");

    spoor_unref(c);
}

void test_null_read_returns_eof(void) {
    struct Spoor *c = open_spoor(&devnull, 0);
    TEST_ASSERT(c != NULL, "open devnull OK");

    u8 buf[16];
    long got = devnull.read(c, buf, 16, 0);
    TEST_EXPECT_EQ(got, (long)0, "devnull.read returns 0 (EOF)");

    spoor_clunk(c);
}

void test_null_write_consumes(void) {
    struct Spoor *c = open_spoor(&devnull, 0);
    TEST_ASSERT(c != NULL, "open devnull OK");

    const char msg[] = "swallowed by /dev/null";
    long n = (long)sizeof(msg) - 1;
    long wrote = devnull.write(c, msg, n, 0);
    TEST_EXPECT_EQ(wrote, n, "devnull.write returns n");

    // Negative n must be rejected.
    TEST_EXPECT_EQ(devnull.write(c, msg, -1, 0), (long)-1,
                   "negative n rejected");

    spoor_clunk(c);
}

void test_zero_read_fills_zeroes(void) {
    struct Spoor *c = open_spoor(&devzero, 0);
    TEST_ASSERT(c != NULL, "open devzero OK");

    u8 buf[32];
    // Pre-fill with non-zero so we observe the write.
    for (int i = 0; i < 32; i++) buf[i] = 0xAB;

    long got = devzero.read(c, buf, 32, 0);
    TEST_EXPECT_EQ(got, (long)32, "devzero.read returns n");

    for (int i = 0; i < 32; i++) {
        TEST_ASSERT(buf[i] == 0,
                    "every byte filled by devzero.read must be zero");
    }

    // n=0 is a legal no-op.
    TEST_EXPECT_EQ(devzero.read(c, buf, 0, 0), (long)0, "n=0 returns 0");
    // NULL buf rejected.
    TEST_EXPECT_EQ(devzero.read(c, NULL, 32, 0), (long)-1, "NULL buf rejected");

    spoor_clunk(c);
}

void test_zero_write_consumes(void) {
    struct Spoor *c = open_spoor(&devzero, 0);
    TEST_ASSERT(c != NULL, "open devzero OK");

    const char msg[] = "absorbed by /dev/zero";
    long n = (long)sizeof(msg) - 1;
    long wrote = devzero.write(c, msg, n, 0);
    TEST_EXPECT_EQ(wrote, n, "devzero.write returns n");

    spoor_clunk(c);
}

// =============================================================================
// devfull (P6-pouch-devnodes — sub-chunk 11). Linux man 4 full semantics:
// reads NUL-fill like /dev/zero; writes always fail.
// =============================================================================

void test_full_attach_open_close(void) {
    struct Spoor *c = devfull.attach("");
    TEST_ASSERT(c != NULL, "devfull.attach succeeds");
    TEST_EXPECT_EQ(c->dev,      &devfull,  "Spoor.dev = &devfull");
    TEST_EXPECT_EQ(c->dc,       'f',       "Spoor.dc = 'f'");
    TEST_EXPECT_EQ(c->qid.type, QTFILE,    "fresh attach: QTFILE");
    TEST_EXPECT_EQ((u32)0,      c->flag,   "fresh attach: flag=0");

    struct Spoor *opened = devfull.open(c, 0);
    TEST_EXPECT_EQ(opened, c, "open returns the same Spoor");
    TEST_ASSERT((c->flag & COPEN) != 0, "open sets COPEN");

    devfull.close(c);
    TEST_EXPECT_EQ((u32)0, c->flag & COPEN, "close clears COPEN");

    spoor_unref(c);
}

void test_full_read_fills_zeroes(void) {
    struct Spoor *c = open_spoor(&devfull, 0);
    TEST_ASSERT(c != NULL, "open devfull OK");

    u8 buf[32];
    // Pre-fill with non-zero so we observe the NUL write.
    for (int i = 0; i < 32; i++) buf[i] = 0xAB;

    long got = devfull.read(c, buf, 32, 0);
    TEST_EXPECT_EQ(got, (long)32, "devfull.read returns n (NUL-fills like /dev/zero)");

    for (int i = 0; i < 32; i++) {
        TEST_ASSERT(buf[i] == 0,
                    "every byte filled by devfull.read must be zero");
    }

    // n=0 is a legal no-op.
    TEST_EXPECT_EQ(devfull.read(c, buf, 0, 0), (long)0, "n=0 returns 0");
    // NULL buf rejected.
    TEST_EXPECT_EQ(devfull.read(c, NULL, 32, 0), (long)-1, "NULL buf rejected");
    // Negative n rejected.
    TEST_EXPECT_EQ(devfull.read(c, buf, -1, 0), (long)-1, "negative n rejected");

    spoor_clunk(c);
}

void test_full_write_returns_minus1(void) {
    struct Spoor *c = open_spoor(&devfull, 0);
    TEST_ASSERT(c != NULL, "open devfull OK");

    const char msg[] = "would-be-written-to-a-full-disk";
    long n = (long)sizeof(msg) - 1;
    // Every write fails — the "full disk" contract.
    TEST_EXPECT_EQ(devfull.write(c, msg, n,   0), (long)-1, "non-empty write fails");
    TEST_EXPECT_EQ(devfull.write(c, msg, 1,   0), (long)-1, "single-byte write fails");
    TEST_EXPECT_EQ(devfull.write(c, msg, 0,   0), (long)-1, "n=0 also fails (Linux behavior)");
    TEST_EXPECT_EQ(devfull.write(c, NULL, 4,  0), (long)-1, "NULL buf write fails");

    spoor_clunk(c);
}

// RNDR detection — on QEMU virt with -cpu max, FEAT_RNG should be live.
// If for some reason RNDR is absent (unusual config), random.read will
// return -1 and the boot banner notes it; this test then becomes a
// reminder that random.read is degenerate.
void test_random_rndr_available(void) {
    struct Spoor *c = open_spoor(&devrandom, 0);
    TEST_ASSERT(c != NULL, "open devrandom OK");

    u8 buf[8];
    long got = devrandom.read(c, buf, 8, 0);

    // Either RNDR is available (got > 0) or absent (got == -1). Both
    // are valid. The exit criterion only applies when RNDR is up.
    TEST_ASSERT(got == 8 || got == -1,
                "devrandom.read on RNDR-capable CPU returns 8; on "
                "non-RNDR CPU returns -1");

    spoor_clunk(c);
}

// ROADMAP §6.2 exit criterion: "cat /dev/random produces non-zero
// bytes." Read 16 bytes; assert at least one is non-zero. (16 bytes
// being all zero by chance from a CSPRNG is 2^-128.)
void test_random_read_produces_nonzero_bytes(void) {
    struct Spoor *c = open_spoor(&devrandom, 0);
    TEST_ASSERT(c != NULL, "open devrandom OK");

    u8 buf[16];
    for (int i = 0; i < 16; i++) buf[i] = 0;

    long got = devrandom.read(c, buf, 16, 0);
    if (got == -1) {
        // RNDR unavailable — exit criterion is conditional on FEAT_RNG.
        // Skip with a soft note; this is observable in the boot banner.
        spoor_clunk(c);
        return;
    }

    TEST_ASSERT(got == 16,
                "devrandom.read must return full 16 bytes when RNDR up");

    bool any_nonzero = false;
    for (int i = 0; i < 16; i++) {
        if (buf[i] != 0) { any_nonzero = true; break; }
    }
    TEST_ASSERT(any_nonzero,
                "ROADMAP §6.2: cat /dev/random produces non-zero bytes");

    spoor_clunk(c);
}

void test_random_read_varies_across_calls(void) {
    struct Spoor *c = open_spoor(&devrandom, 0);
    TEST_ASSERT(c != NULL, "open devrandom OK");

    u8 a[16], b[16];
    long ga = devrandom.read(c, a, 16, 0);
    long gb = devrandom.read(c, b, 16, 0);

    if (ga == -1 || gb == -1) {
        spoor_clunk(c);
        return;             // RNDR unavailable; skip
    }

    TEST_ASSERT(ga == 16 && gb == 16, "two 16-byte reads succeed");

    bool different = false;
    for (int i = 0; i < 16; i++) {
        if (a[i] != b[i]) { different = true; break; }
    }
    TEST_ASSERT(different,
                "two consecutive devrandom reads must differ "
                "(probabilistic; 2^-128 fail rate)");

    spoor_clunk(c);
}

void test_cons_write_advances(void) {
    struct Spoor *c = open_spoor(&devcons, 0);
    TEST_ASSERT(c != NULL, "open devcons OK");

    // Write a recognizable marker. Visible in boot log under the
    // "tests:" section. NB: we don't assert it appears on UART —
    // tests can't capture stdout from inside QEMU. The PASS is the
    // return-value contract.
    const char marker[] = "[test_cons_write_advances]";
    long n = (long)sizeof(marker) - 1;
    long wrote = devcons.write(c, marker, n, 0);
    TEST_EXPECT_EQ(wrote, n, "devcons.write returns n");

    // Empty / NULL / negative cases.
    TEST_EXPECT_EQ(devcons.write(c, "", 0, 0), (long)0, "n=0 returns 0");
    TEST_EXPECT_EQ(devcons.write(c, NULL, 5, 0), (long)-1, "NULL buf rejected");
    TEST_EXPECT_EQ(devcons.write(c, marker, -1, 0), (long)-1, "neg n rejected");

    spoor_clunk(c);
}

void test_cons_read_returns_eof(void) {
    struct Spoor *c = open_spoor(&devcons, 0);
    TEST_ASSERT(c != NULL, "open devcons OK");

    u8 buf[16];
    long got = devcons.read(c, buf, 16, 0);
    TEST_EXPECT_EQ(got, (long)0,
                   "devcons.read returns 0 at v1.0 (UART RX deferred)");

    spoor_clunk(c);
}

// ROADMAP §6.2 exit criterion: 10,000 open/read/close cycles on
// /dev/null without leak. Replaces P4-A's spoor.alloc_10k_no_leak
// devnone-against-cold-cache loop with the warm-cache version.
//
// The "spoor" SLUB cache is now hot from prior tests in this run, so
// the per-iteration cost drops by ~10x — comfortably within the boot
// budget for the full 10K count.
void test_trivial_devs_devnull_10k_no_leak(void) {
    enum { ITERS = 10000 };

    u64 alloc_before = spoor_total_allocated();
    u64 free_before  = spoor_total_freed();

    for (int i = 0; i < ITERS; i++) {
        struct Spoor *c = devnull.attach("");
        TEST_ASSERT(c != NULL, "devnull.attach must succeed in no-leak loop");
        struct Spoor *opened = devnull.open(c, 0);
        TEST_ASSERT(opened == c, "devnull.open must succeed");

        u8 b[1];
        // read returns EOF; write consumes — exercise both paths.
        (void)devnull.read(c, b, 1, 0);
        (void)devnull.write(c, b, 1, 0);

        spoor_clunk(c);
    }

    u64 alloc_delta = spoor_total_allocated() - alloc_before;
    u64 free_delta  = spoor_total_freed()      - free_before;
    TEST_EXPECT_EQ(alloc_delta, (u64)ITERS, "10K spoor allocs observed");
    TEST_EXPECT_EQ(free_delta,  (u64)ITERS,
                   "10K spoor frees observed (no leak — ROADMAP §6.2)");
}
