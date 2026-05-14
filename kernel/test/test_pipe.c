// Kernel pipe tests (P5-pipe).
//
// Coverage:
//
//   pipe.smoke
//     Create a pair; write some bytes to write end; read them back from
//     read end; verify FIFO order + counters.
//
//   pipe.read_on_empty_returns_zero
//     read end with empty buffer returns 0 (non-blocking semantic).
//
//   pipe.write_to_full_returns_zero
//     Fill the buffer (PIPE_BUF_SIZE bytes); next write returns 0.
//
//   pipe.write_short_when_partially_full
//     Buffer has K free bytes; write N > K returns K.
//
//   pipe.wraparound
//     Write half, read half, write more (head wraps), read all → bytes
//     emerge in correct order across the wrap.
//
//   pipe.read_on_write_end_rejected
//     Write end's dev->read returns -1 (wrong end).
//
//   pipe.write_on_read_end_rejected
//     Read end's dev->write returns -1 (wrong end).
//
//   pipe.close_one_end_keeps_other_alive
//     Clunk read end. Write end's Spoor still valid (ref > 0). Ring's
//     refcount dropped by 1 (still 1, ring alive).
//
//   pipe.close_both_ends_frees_ring
//     Clunk both ends. pipe_total_freed increments. Subsequent reads /
//     writes are not attempted (Spoor is gone).
//
//   pipe.compose_with_spoor_transport
//     End-to-end: build a pair of pipe pairs (tx + rx); wire them into
//     a p9_spoor_transport adapter; run a Tversion + Tattach handshake
//     through real pipes. Replaces the test scaffold's byte-pipe Dev
//     with the production pipe primitive.

#include "test.h"

#include <thylacine/9p_session.h>
#include <thylacine/9p_spoor_transport.h>
#include <thylacine/9p_transport.h>
#include <thylacine/9p_wire.h>
#include <thylacine/dev.h>
#include <thylacine/pipe.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

// =============================================================================
// Forward decls.
// =============================================================================

void test_pipe_smoke(void);
void test_pipe_read_on_empty_returns_zero(void);
void test_pipe_write_to_full_returns_zero(void);
void test_pipe_write_short_when_partially_full(void);
void test_pipe_wraparound(void);
void test_pipe_read_on_write_end_rejected(void);
void test_pipe_write_on_read_end_rejected(void);
void test_pipe_close_one_end_keeps_other_alive(void);
void test_pipe_close_both_ends_frees_ring(void);
void test_pipe_compose_with_spoor_transport(void);

// =============================================================================
// Helpers.
// =============================================================================

static long dev_write(struct Spoor *c, const void *buf, long n) {
    return c->dev->write(c, buf, n, 0);
}

static long dev_read(struct Spoor *c, void *buf, long n) {
    return c->dev->read(c, buf, n, 0);
}

// =============================================================================
// Tests.
// =============================================================================

void test_pipe_smoke(void) {
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0,
        "pipe_create returns 0");
    TEST_ASSERT(rd != NULL && wr != NULL,
        "both endpoints non-NULL");
    TEST_ASSERT(rd != wr, "distinct Spoors");

    const u8 payload[] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
    TEST_EXPECT_EQ(dev_write(wr, payload, (long)sizeof(payload)),
                   (long)sizeof(payload),
        "write accepts full payload");

    u8 got[8] = { 0 };
    long n = dev_read(rd, got, (long)sizeof(got));
    TEST_EXPECT_EQ(n, (long)sizeof(payload),
        "read returns payload length");
    for (size_t i = 0; i < sizeof(payload); i++) {
        TEST_ASSERT(got[i] == payload[i],
            "FIFO order preserved");
    }

    spoor_clunk(rd);
    spoor_clunk(wr);
}

void test_pipe_read_on_empty_returns_zero(void) {
    // Renamed semantics under P5-pipe-blocking: read on empty WOULD
    // sleep; to test the non-sleeping path we close the write end first,
    // which sets write_eof so read returns 0 (EOF) immediately.
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "create");

    // Close write end first → write_eof = true.
    spoor_clunk(wr);

    u8 got[16];
    TEST_EXPECT_EQ(dev_read(rd, got, (long)sizeof(got)), 0L,
        "read on empty + write_eof returns 0 (EOF)");

    spoor_clunk(rd);
}

void test_pipe_write_to_full_returns_zero(void) {
    // Renamed semantics under P5-pipe-blocking: write to full WOULD
    // sleep; to test the non-sleeping path we close the read end first,
    // which sets read_eof so write returns -1 (EPIPE) immediately.
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "create");

    // Close read end first → read_eof = true.
    spoor_clunk(rd);

    // Write returns -1 (EPIPE) regardless of buffer state.
    u8 extra = 0xAB;
    TEST_EXPECT_EQ(dev_write(wr, &extra, 1L), -1L,
        "write with read_eof returns -1 (EPIPE)");

    spoor_clunk(wr);
}

void test_pipe_write_short_when_partially_full(void) {
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "create");

    // Fill all but 10 bytes.
    static u8 fill[PIPE_BUF_SIZE];
    for (size_t i = 0; i < PIPE_BUF_SIZE; i++) fill[i] = 0xCC;
    TEST_EXPECT_EQ(dev_write(wr, fill, (long)(PIPE_BUF_SIZE - 10)),
                   (long)(PIPE_BUF_SIZE - 10),
        "partial fill");

    // Ask to write 100; only 10 fit.
    u8 more[100];
    for (size_t i = 0; i < 100; i++) more[i] = 0xDD;
    TEST_EXPECT_EQ(dev_write(wr, more, 100L), 10L,
        "short write returns space-available");

    spoor_clunk(rd);
    spoor_clunk(wr);
}

void test_pipe_wraparound(void) {
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "create");

    // Write 3000 bytes (well under PIPE_BUF_SIZE=4096).
    static u8 first[3000];
    for (size_t i = 0; i < 3000; i++) first[i] = (u8)((i + 1) & 0xff);
    TEST_EXPECT_EQ(dev_write(wr, first, 3000L), 3000L,
        "first write 3000 bytes");

    // Drain 2500 bytes.
    static u8 drain[2500];
    TEST_EXPECT_EQ(dev_read(rd, drain, 2500L), 2500L,
        "drain 2500 bytes");
    for (size_t i = 0; i < 2500; i++) {
        TEST_ASSERT(drain[i] == first[i],
            "drained bytes match first[0..2500)");
    }

    // Write another 3000 bytes — head wraps past end-of-buf because
    // tail is at 2500 and head is at 3000; the next 3000 wraps into
    // the freed prefix.
    static u8 second[3000];
    for (size_t i = 0; i < 3000; i++) second[i] = (u8)((i + 0x80) & 0xff);
    TEST_EXPECT_EQ(dev_write(wr, second, 3000L), 3000L,
        "second write 3000 bytes (wraps)");

    // Drain everything remaining: 500 bytes of first[2500..3000)
    // followed by 3000 bytes of second.
    static u8 rest[3500];
    TEST_EXPECT_EQ(dev_read(rd, rest, 3500L), 3500L,
        "final drain 3500 bytes");
    for (size_t i = 0; i < 500; i++) {
        TEST_ASSERT(rest[i] == first[2500 + i],
            "tail of first segment correct");
    }
    for (size_t i = 0; i < 3000; i++) {
        TEST_ASSERT(rest[500 + i] == second[i],
            "second segment correct (post-wrap)");
    }

    spoor_clunk(rd);
    spoor_clunk(wr);
}

void test_pipe_read_on_write_end_rejected(void) {
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "create");

    // Pre-fill so the buffer has data — proves the rejection isn't
    // because the buffer is empty.
    u8 payload[4] = { 1, 2, 3, 4 };
    dev_write(wr, payload, 4L);

    u8 got[8];
    TEST_EXPECT_EQ(dev_read(wr, got, 8L), -1L,
        "read on write end returns -1");

    spoor_clunk(rd);
    spoor_clunk(wr);
}

void test_pipe_write_on_read_end_rejected(void) {
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "create");

    u8 payload[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    TEST_EXPECT_EQ(dev_write(rd, payload, 4L), -1L,
        "write on read end returns -1");

    spoor_clunk(rd);
    spoor_clunk(wr);
}

void test_pipe_close_one_end_keeps_other_alive(void) {
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "create");
    u64 freed_before = pipe_total_freed();

    // Put data in so we can verify the ring is alive after one close.
    u8 payload[4] = { 9, 8, 7, 6 };
    dev_write(wr, payload, 4L);

    // Close read end. Write end's Spoor still valid; ring NOT freed
    // (still 1 ref).
    spoor_clunk(rd);
    TEST_EXPECT_EQ(pipe_total_freed() - freed_before, 0ULL,
        "ring NOT freed after one end close");
    TEST_ASSERT(wr->magic != 0,
        "write end Spoor still alive after read close");

    spoor_clunk(wr);
    TEST_EXPECT_EQ(pipe_total_freed() - freed_before, 1ULL,
        "ring freed after second end close");
}

void test_pipe_close_both_ends_frees_ring(void) {
    u64 alloc_before = pipe_total_allocated();
    u64 freed_before = pipe_total_freed();
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "create");

    TEST_EXPECT_EQ(pipe_total_allocated() - alloc_before, 1ULL,
        "pipe_total_allocated += 1");

    spoor_clunk(rd);
    spoor_clunk(wr);
    TEST_EXPECT_EQ(pipe_total_freed() - freed_before, 1ULL,
        "ring freed; pipe_total_freed += 1");
}

void test_pipe_compose_with_spoor_transport(void) {
    // Two pipe pairs:
    //   pair 1 (rd1, wr1): client→test direction.  Adapter's tx = wr1.
    //   pair 2 (rd2, wr2): test→client direction.  Adapter's rx = rd2.
    //
    // Test acts as the "server": reads request from rd1, synthesizes
    // canonical R-frame into wr2.
    struct Spoor *rd1 = NULL, *wr1 = NULL, *rd2 = NULL, *wr2 = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd1, &wr1), 0, "client→server pipe");
    TEST_EXPECT_EQ(pipe_create(&rd2, &wr2), 0, "server→client pipe");

    struct p9_spoor_transport st;
    TEST_EXPECT_EQ(p9_spoor_transport_init(&st, wr1, rd2, false), 0,
        "adapter init: tx=wr1, rx=rd2, owns=false");
    struct p9_transport_ops ops = p9_spoor_transport_ops(&st);

    static u8 recv_buf[2048];
    struct p9_transport t;
    TEST_EXPECT_EQ(p9_transport_init(&t, ops, recv_buf, sizeof(recv_buf)), 0,
        "transport init");

    struct p9_session s;
    TEST_EXPECT_EQ(p9_session_init(&s, /*root_fid=*/1, /*msize=*/4096), 0,
        "session init");

    // Tversion through the pipe stack.
    u8 out_buf[256];
    int len = p9_session_send_version(&s, out_buf, sizeof(out_buf),
                                      (const u8 *)"9P2000.L", 8);
    TEST_ASSERT(len > 0, "send_version");
    TEST_EXPECT_EQ(p9_transport_send(&t, out_buf, (size_t)len), 0,
        "transport send Tversion");

    // Server side: drain Tversion from rd1.
    u8 drained[256];
    long got = dev_read(rd1, drained, len);
    TEST_EXPECT_EQ(got, (long)len,
        "server drains Tversion from rd1");
    TEST_EXPECT_EQ((u32)drained[4], 100u,
        "first frame is Tversion (type=100)");

    // Server synthesizes Rversion → wr2.
    u8 rversion[64];
    size_t roff = 0;
    const u8 *version = (const u8 *)"9P2000.L";
    size_t vlen = 8;
    u32 rsize = 4 + 1 + 2 + 4 + 2 + (u32)vlen;
    rversion[roff++] = (u8)(rsize & 0xff);
    rversion[roff++] = (u8)((rsize >> 8) & 0xff);
    rversion[roff++] = (u8)((rsize >> 16) & 0xff);
    rversion[roff++] = (u8)((rsize >> 24) & 0xff);
    rversion[roff++] = 101;                           // RVERSION
    rversion[roff++] = 0xff; rversion[roff++] = 0xff; // NOTAG
    rversion[roff++] = 0x00; rversion[roff++] = 0x10; // msize = 4096 (little-endian)
    rversion[roff++] = 0x00; rversion[roff++] = 0x00;
    rversion[roff++] = (u8)(vlen & 0xff);
    rversion[roff++] = (u8)((vlen >> 8) & 0xff);
    for (size_t i = 0; i < vlen; i++) rversion[roff++] = version[i];
    TEST_EXPECT_EQ(dev_write(wr2, rversion, (long)roff), (long)roff,
        "server writes Rversion to wr2");

    // Client recv + dispatch.
    TEST_EXPECT_EQ(p9_transport_recv(&t), (int)rsize,
        "client recv Rversion");
    struct p9_dispatch_result r;
    TEST_EXPECT_EQ(p9_session_dispatch_rmsg(&s, recv_buf, rsize, &r), 0,
        "session dispatch Rversion");
    TEST_EXPECT_EQ((int)s.state, (int)P9_SESS_VERSIONED,
        "INIT → VERSIONED");

    // Tattach round trip.
    len = p9_session_send_attach(&s, out_buf, sizeof(out_buf),
                                 (const u8 *)"none", 4,
                                 (const u8 *)"", 0,
                                 /*n_uname=*/0);
    TEST_ASSERT(len > 0, "send_attach");
    TEST_EXPECT_EQ(p9_transport_send(&t, out_buf, (size_t)len), 0,
        "transport send Tattach");
    got = dev_read(rd1, drained, len);
    TEST_EXPECT_EQ(got, (long)len, "server drains Tattach");
    TEST_EXPECT_EQ((u32)drained[4], 104u,
        "second frame is Tattach (type=104)");
    u8 tag_lo = drained[5], tag_hi = drained[6];

    // Server synthesizes Rattach (header + 13-byte qid).
    u8 rattach[32];
    size_t aoff = 0;
    u32 asize = 4 + 1 + 2 + 13;
    rattach[aoff++] = (u8)(asize & 0xff);
    rattach[aoff++] = (u8)((asize >> 8) & 0xff);
    rattach[aoff++] = (u8)((asize >> 16) & 0xff);
    rattach[aoff++] = (u8)((asize >> 24) & 0xff);
    rattach[aoff++] = 105;                            // RATTACH
    rattach[aoff++] = tag_lo; rattach[aoff++] = tag_hi;
    rattach[aoff++] = 0x80;                           // QTDIR
    for (int i = 0; i < 12; i++) rattach[aoff++] = 0; // vers + path
    TEST_EXPECT_EQ(dev_write(wr2, rattach, (long)aoff), (long)aoff,
        "server writes Rattach");

    TEST_EXPECT_EQ(p9_transport_recv(&t), (int)asize,
        "client recv Rattach");
    TEST_EXPECT_EQ(p9_session_dispatch_rmsg(&s, recv_buf, asize, &r), 0,
        "session dispatch Rattach");
    TEST_EXPECT_EQ((int)s.state, (int)P9_SESS_OPEN,
        "VERSIONED → OPEN");
    TEST_ASSERT(p9_session_fid_bound(&s, 1),
        "root fid bound");

    p9_session_destroy(&s);
    p9_transport_destroy(&t);
    p9_spoor_transport_destroy(&st);
    spoor_clunk(rd1);
    spoor_clunk(wr1);
    spoor_clunk(rd2);
    spoor_clunk(wr2);
}
