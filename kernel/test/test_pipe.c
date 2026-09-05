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
#include <thylacine/errno.h>
#include <thylacine/9p_spoor_transport.h>
#include <thylacine/9p_transport.h>
#include <thylacine/9p_wire.h>
#include <thylacine/dev.h>
#include <thylacine/pipe.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>   // #96: struct t_stat + T_S_IFIFO
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
    // which sets read_eof so write fails immediately.
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "create");

    // Close read end first → read_eof = true.
    spoor_clunk(rd);

    // #100 (ER-3): -T_E_PIPE, regardless of buffer state. This assertion
    // used to read `-1L` under the message "returns -1 (EPIPE)" -- the name
    // stated the intent while the value pinned the defect, so the test
    // passed for years on a pipe that could not produce EPIPE at all.
    u8 extra = 0xAB;
    TEST_EXPECT_EQ(dev_write(wr, &extra, 1L), (long)(-T_E_PIPE),
        "write with read_eof returns -T_E_PIPE");

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

    // Ask to write n > PIPE_BUF; only the 10 free bytes fit and the write
    // returns short. Since the 2026-09-02 PIPE_BUF-atomicity fix, ONLY a write
    // of n > PIPE_BUF partials -- a blocking write of n <= PIPE_BUF that does
    // not wholly fit now WAITS for the whole fit (POSIX atomicity), so the old
    // 100-byte form would sleep forever on this single-threaded boot path.
    static u8 more[PIPE_BUF_SIZE + 50];
    for (size_t i = 0; i < sizeof(more); i++) more[i] = 0xDD;
    TEST_EXPECT_EQ(dev_write(wr, more, (long)sizeof(more)), 10L,
        "short write (n > PIPE_BUF) returns space-available");

    spoor_clunk(rd);
    spoor_clunk(wr);
}

void test_pipe_nonblock_returns_eagain(void) {
    // CNONBLOCK (per-Spoor POSIX O_NONBLOCK, the git-stash fill): a would-block
    // read/write returns -T_E_AGAIN instead of sleeping. The guard is a
    // pre-sleep early return placed AFTER the data / EOF / space checks, so this
    // test pins BOTH that the would-block case converts AND that a ready op,
    // EPIPE, and EOF are all untouched -- the placement is the load-bearing
    // half (a guard before the write_eof check would spin a reader forever at
    // EOF). The blocking path never registers on the rendez for an EAGAIN
    // caller, so I-9 (pipe.tla NoStuckReader/NoStuckWriter) is byte-unchanged.
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "create");
    rd->flag |= CNONBLOCK;
    wr->flag |= CNONBLOCK;

    // (1) empty + both ends open: a blocking read would sleep -> EAGAIN.
    static u8 got[PIPE_BUF_SIZE];
    TEST_EXPECT_EQ(dev_read(rd, got, 64L), (long)(-T_E_AGAIN),
        "non-blocking read on empty (not EOF) returns -T_E_AGAIN");

    // (2) data present: the guard sits AFTER the drain, so a ready read serves.
    const u8 payload[] = { 0xA1, 0xB2, 0xC3, 0xD4 };
    TEST_EXPECT_EQ(dev_write(wr, payload, (long)sizeof(payload)),
                   (long)sizeof(payload), "write payload (space available)");
    TEST_EXPECT_EQ(dev_read(rd, got, 64L), (long)sizeof(payload),
        "non-blocking read still drains available data");
    for (size_t i = 0; i < sizeof(payload); i++)
        TEST_ASSERT(got[i] == payload[i], "FIFO order preserved");

    // (3) completely full: a blocking write would sleep -> EAGAIN.
    static u8 fill[PIPE_BUF_SIZE];
    for (size_t i = 0; i < PIPE_BUF_SIZE; i++) fill[i] = 0xEE;
    TEST_EXPECT_EQ(dev_write(wr, fill, (long)PIPE_BUF_SIZE),
                   (long)PIPE_BUF_SIZE, "fill the ring completely");
    u8 one = 0x5A;
    TEST_EXPECT_EQ(dev_write(wr, &one, 1L), (long)(-T_E_AGAIN),
        "non-blocking write on full returns -T_E_AGAIN");

    // (4) not stranded: draining frees space; the next write serves.
    static u8 drain[PIPE_BUF_SIZE];
    TEST_EXPECT_EQ(dev_read(rd, drain, 100L), 100L, "drain 100 bytes");
    TEST_EXPECT_EQ(dev_write(wr, &one, 1L), 1L,
        "non-blocking write serves once space frees");

    // (5) the guard sits AFTER the write_eof check: a non-blocking read at EOF
    // returns 0, NOT EAGAIN (else a reader would spin forever at EOF).
    (void)dev_read(rd, drain, (long)PIPE_BUF_SIZE);   // fully drain the ring
    spoor_clunk(wr);                                  // write_eof = true
    TEST_EXPECT_EQ(dev_read(rd, drain, (long)PIPE_BUF_SIZE), 0L,
        "non-blocking read at EOF returns 0 (not EAGAIN)");

    spoor_clunk(rd);
}

// CNBFRAME (the byte-pipe 9P transport's tx end): frame-atomic + non-blocking.
// The round-B F1 regression: a 9P frame is n <= msize <= PIPE_BUF, and since
// the 2026-09-02 PIPE_BUF-atomicity fix a NON-CNBFRAME blocking write of
// n <= PIPE_BUF that does not wholly fit BLOCKS (sleeps) until it does -- not a
// partial. Taken under the 9P client's held c->lock, that sleep is the #360
// lock-across-sleep extinction. (Even before the atomicity fix it was fatal:
// then the non-fitting frame partial-wrote and stranded a fragment, desyncing
// the shared stream, #349.) With CNBFRAME a write commits the WHOLE frame or
// returns -T_E_AGAIN having written NOTHING, and never sleeps -- so the client
// drops c->lock and retries. The contrast test
// test_pipe_write_short_when_partially_full now uses n > PIPE_BUF (the only
// size that still partials); a frame-sized non-fitting write there would block,
// which is exactly the hazard this flag exists to avoid.
void test_pipe_cnbframe_atomic_nonblocking(void) {
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "create");
    wr->flag |= CNBFRAME;

    static u8 frame[1000];
    for (size_t i = 0; i < sizeof(frame); i++) frame[i] = 0xA5;

    // (1) A frame FITS in an empty pipe -> the whole frame commits.
    TEST_EXPECT_EQ(dev_write(wr, frame, 1000L), 1000L,
        "CNBFRAME: a fitting frame commits whole");
    // (2) Three more -> 4000 buffered, 96 free (< 1000).
    for (int k = 0; k < 3; k++)
        TEST_EXPECT_EQ(dev_write(wr, frame, 1000L), 1000L,
            "CNBFRAME: fitting frames commit whole");

    // (3) The frame no longer fits (96 free < 1000) -> -T_E_AGAIN, ATOMIC.
    //     The non-CNBFRAME path would BLOCK on a frame this size (n <= PIPE_BUF
    //     waits for the whole fit since the 2026-09-02 atomicity fix; before it
    //     it partial-wrote the 96 free bytes); CNBFRAME writes NOTHING and
    //     never sleeps -- that is the whole point of the flag.
    TEST_EXPECT_EQ(dev_write(wr, frame, 1000L), (long)(-T_E_AGAIN),
        "CNBFRAME: a non-fitting frame -> -T_E_AGAIN, nothing written");

    // (4) Prove atomicity: exactly 4000 readable, not 4096 (no partial byte).
    static u8 drain[PIPE_BUF_SIZE];
    TEST_EXPECT_EQ(dev_read(rd, drain, (long)PIPE_BUF_SIZE), 4000L,
        "CNBFRAME: the rejected frame left the ring at 4000, no partial byte");

    // (5) Drained -> the frame fits again (progress is guaranteed since a
    //     frame <= PIPE_BUF_SIZE always fits an empty pipe).
    TEST_EXPECT_EQ(dev_write(wr, frame, 1000L), 1000L,
        "CNBFRAME: fits again once drained");

    // (6) A read-closed pipe still yields -T_E_PIPE under CNBFRAME (unchanged).
    spoor_clunk(rd);   // close the read end -> read_eof
    TEST_EXPECT_EQ(dev_write(wr, frame, 1000L), (long)(-T_E_PIPE),
        "CNBFRAME: a read-closed pipe -> -T_E_PIPE");

    spoor_clunk(wr);
}

// The follow-up round's F1: SYS_ATTACH_9P admits pipe pairs ONLY. The spoor
// transport is sound solely over a NON-BLOCKING tx -- CNBFRAME is honored by
// devpipe alone, and a /srv byte-conn (devsrv_write tsleeps) or a dev9p file tx
// driven under the 9P client's held c->lock is the #360 lock-across-sleep
// extinction. This exercises the handler's ACTUAL gate predicate
// (sys_attach_9p_ends_are_pipes): a pipe pair passes, any non-pipe / NULL end is
// refused. p9_spoor_transport_init itself stays Dev-generic (the transport tests
// drive it over a non-blocking mock), so the pipe-only constraint lives at the
// EL0 boundary, which is what this checks.
void test_pipe_attach_9p_admits_pipes_only(void) {
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "pipe");
    struct Spoor *nonpipe = spoor_alloc(&devnull);
    TEST_ASSERT(nonpipe != NULL, "non-pipe Spoor (devnull)");

    TEST_ASSERT(sys_attach_9p_ends_are_pipes(wr, rd),
        "a pipe tx + pipe rx is admitted");
    TEST_ASSERT(!sys_attach_9p_ends_are_pipes(nonpipe, rd),
        "a non-pipe tx is refused -- the extinction vector");
    TEST_ASSERT(!sys_attach_9p_ends_are_pipes(wr, nonpipe),
        "a non-pipe rx is refused too");
    TEST_ASSERT(!sys_attach_9p_ends_are_pipes(NULL, rd),
        "a NULL tx is refused");
    TEST_ASSERT(!sys_attach_9p_ends_are_pipes(wr, NULL),
        "a NULL rx is refused");

    spoor_clunk(nonpipe);
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
    TEST_EXPECT_EQ(dev_read(wr, got, 8L), (long)(-T_E_BADF),
        "read on write end returns -T_E_BADF");

    spoor_clunk(rd);
    spoor_clunk(wr);
}

void test_pipe_write_on_read_end_rejected(void) {
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "create");

    u8 payload[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    TEST_EXPECT_EQ(dev_write(rd, payload, 4L), (long)(-T_E_BADF),
        "write on read end returns -T_E_BADF");

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

// #96 -- fstat(2) on a pipe must SUCCEED and report S_IFIFO.
//
// Found by the CL-5 build storm: GNU make hands every concurrent job but the
// first the read end of a broken pipe on fd 0, and clang's
// FixupStandardFileDescriptors treats a non-EBADF fstat failure on fd 0/1/2 as
// FATAL -- so `make -j4` had job 1 succeed and its siblings exit 1 with no
// diagnostic at all. devpipe simply had no .stat_native, and
// spoor_stat_native returns -1 when the slot is NULL.
//
// This drives spoor_stat_native -- the exact function SYS_FSTAT calls -- not
// the vtable slot directly, so a future refactor that reintroduces the NULL
// slot fails here.
void test_pipe_fstat_reports_fifo(void) {
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "pipe_create returns 0");

    struct t_stat st;
    // Poison, so a stat_native that returns 0 without filling the struct
    // cannot pass by leaving stale zeroes that happen to look right.
    for (size_t i = 0; i < sizeof(st); i++) ((u8 *)&st)[i] = 0xA5;
    TEST_EXPECT_EQ(spoor_stat_native(rd, &st), 0,
        "fstat on the READ end succeeds (pre-#96 this returned -1)");
    TEST_EXPECT_EQ((long)(st.mode & T_S_IFMT), (long)T_S_IFIFO,
        "read end reports S_IFIFO");
    TEST_EXPECT_EQ((long)(st.mode & 07777u), 0600L,
        "read end reports 0600");
    TEST_EXPECT_EQ((long)st.size, 0L, "pipe reports size 0");
    TEST_EXPECT_EQ((long)st.nlink, 1L, "pipe reports nlink 1");

    struct t_stat st_w;
    for (size_t i = 0; i < sizeof(st_w); i++) ((u8 *)&st_w)[i] = 0xA5;
    TEST_EXPECT_EQ(spoor_stat_native(wr, &st_w), 0,
        "fstat on the WRITE end succeeds");
    TEST_EXPECT_EQ((long)(st_w.mode & T_S_IFMT), (long)T_S_IFIFO,
        "write end reports S_IFIFO");
    // One pipe, one inode: the two ends are the same object.
    TEST_ASSERT(st.qid_path == st_w.qid_path,
        "both ends of ONE pipe share a qid_path");
    TEST_ASSERT(st.qid_path != 0,
        "qid_path is stamped (0 was the historical unset value)");

    // ...but two DIFFERENT pipes must not look like the same file, or any
    // caller doing file-identity comparison silently conflates them.
    struct Spoor *rd2 = NULL, *wr2 = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd2, &wr2), 0, "second pipe_create returns 0");
    struct t_stat st2;
    TEST_EXPECT_EQ(spoor_stat_native(rd2, &st2), 0, "fstat on pipe 2 succeeds");
    TEST_ASSERT(st2.qid_path != st.qid_path,
        "two DISTINCT pipes report distinct qid_paths");

    spoor_clunk(rd2);
    spoor_clunk(wr2);
    spoor_clunk(rd);
    spoor_clunk(wr);
}
