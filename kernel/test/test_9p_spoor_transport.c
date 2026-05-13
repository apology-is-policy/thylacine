// Spoor-pair transport adapter tests (P5-spoor-transport).
//
// Tests:
//
//   spoor_transport.init_destroy
//     Init populates magic + spoors + owns flag; is_open returns true.
//     Destroy clobbers magic; is_open returns false.
//
//   spoor_transport.init_null_rejected
//     init with NULL adapter / tx / rx returns -1 + no state mutation.
//
//   spoor_transport.send_routes_to_tx_dev_write
//     Adapter.send writes the bytes through tx Spoor's dev->write; the
//     bytes land in tx's backing buffer.
//
//   spoor_transport.recv_routes_to_rx_dev_read
//     Bytes pre-written to rx's backing buffer are returned by
//     adapter.recv.
//
//   spoor_transport.recv_empty_returns_zero
//     Empty rx buffer: adapter.recv returns 0 (EOF — transport core
//     surfaces this).
//
//   spoor_transport.close_clunks_when_owned
//     owns=true: close clunks both Spoors. Verified by spoor_total_freed
//     counter incrementing.
//
//   spoor_transport.close_preserves_when_unowned
//     owns=false: close leaves Spoors alive. Caller cleans up after.
//
//   spoor_transport.transport_core_round_trip
//     Wrap adapter in `struct p9_transport`; round-trip a raw frame
//     through transport_send + transport_recv with an inline responder
//     that drains tx and synthesizes a reply into rx.
//
//   spoor_transport.end_to_end_handshake
//     Full p9_session + p9_transport + spoor-transport composition:
//     run Tversion + Tattach via p9_session_send_* through the Spoor
//     pair; inline responder builds canonical responses.

#include "test.h"

#include <thylacine/9p_session.h>
#include <thylacine/9p_spoor_transport.h>
#include <thylacine/9p_transport.h>
#include <thylacine/9p_wire.h>
#include <thylacine/dev.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

// =============================================================================
// Test-only byte-pipe Dev.
//
// One buffer per Spoor (in aux), 8 KiB cap, FIFO semantics. write
// appends; read drains from the head. Not registered in the bestiary;
// used only by these tests.
// =============================================================================

#define TEST_PIPE_CAP  8192u

struct test_pipe {
    u8     buf[TEST_PIPE_CAP];
    size_t write_pos;    // total bytes ever written
    size_t read_pos;     // total bytes ever read; read_pos <= write_pos
};

static void test_pipe_init(struct test_pipe *p) {
    p->write_pos = 0;
    p->read_pos  = 0;
    for (size_t i = 0; i < TEST_PIPE_CAP; i++) p->buf[i] = 0;
}

static long test_pipe_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)off;
    if (!c || !c->aux || n < 0) return -1;
    struct test_pipe *p = (struct test_pipe *)c->aux;
    size_t avail = TEST_PIPE_CAP - p->write_pos;
    size_t to_write = ((size_t)n < avail) ? (size_t)n : avail;
    const u8 *src = (const u8 *)buf;
    for (size_t i = 0; i < to_write; i++) {
        p->buf[p->write_pos + i] = src[i];
    }
    p->write_pos += to_write;
    return (long)to_write;
}

static long test_pipe_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)off;
    if (!c || !c->aux || n < 0) return -1;
    struct test_pipe *p = (struct test_pipe *)c->aux;
    size_t avail = p->write_pos - p->read_pos;
    if (avail == 0) return 0;     // EOF / empty
    size_t to_read = ((size_t)n < avail) ? (size_t)n : avail;
    u8 *dst = (u8 *)buf;
    for (size_t i = 0; i < to_read; i++) {
        dst[i] = p->buf[p->read_pos + i];
    }
    p->read_pos += to_read;
    return (long)to_read;
}

static struct Dev test_pipe_dev = {
    .dc    = '?',          // unused; not registered in bestiary
    .name  = "test_pipe",
    .write = test_pipe_write,
    .read  = test_pipe_read,
    // All other slots NULL — spoor_clunk's dev->close check is guarded.
};

// =============================================================================
// Fixture helpers.
// =============================================================================

static struct Spoor *make_test_spoor(struct test_pipe *p) {
    struct Spoor *c = spoor_alloc(&test_pipe_dev);
    if (!c) return NULL;
    test_pipe_init(p);
    c->aux = p;
    return c;
}

// Helper for the inline e2e responder: read one whole 9P frame from
// `src` (peek the 4-byte size field first), then return a pointer to
// the start of the frame within the source buffer.
//
// Returns the frame length on success, 0 if not enough data.
static u32 peek_frame_size(struct test_pipe *p) {
    if (p->write_pos - p->read_pos < 7) return 0;
    u32 size = (u32)p->buf[p->read_pos]
             | ((u32)p->buf[p->read_pos + 1] << 8)
             | ((u32)p->buf[p->read_pos + 2] << 16)
             | ((u32)p->buf[p->read_pos + 3] << 24);
    if (p->write_pos - p->read_pos < size) return 0;
    return size;
}

// =============================================================================
// Forward decls (mirrored in test.c registry).
// =============================================================================

void test_spoor_transport_init_destroy(void);
void test_spoor_transport_init_null_rejected(void);
void test_spoor_transport_send_routes_to_tx_dev_write(void);
void test_spoor_transport_recv_routes_to_rx_dev_read(void);
void test_spoor_transport_recv_empty_returns_zero(void);
void test_spoor_transport_close_clunks_when_owned(void);
void test_spoor_transport_close_preserves_when_unowned(void);
void test_spoor_transport_transport_core_round_trip(void);
void test_spoor_transport_end_to_end_handshake(void);

// =============================================================================
// Tests.
// =============================================================================

void test_spoor_transport_init_destroy(void) {
    static struct test_pipe tx_pipe, rx_pipe;
    struct Spoor *tx = make_test_spoor(&tx_pipe);
    struct Spoor *rx = make_test_spoor(&rx_pipe);
    TEST_ASSERT(tx && rx, "Spoor alloc");

    struct p9_spoor_transport st;
    TEST_EXPECT_EQ(p9_spoor_transport_init(&st, tx, rx, false), 0,
        "init returns 0");
    TEST_ASSERT(p9_spoor_transport_is_open(&st),
        "is_open after init");
    TEST_EXPECT_EQ((u32)st.magic, P9_SPOOR_TRANSPORT_MAGIC,
        "magic set");
    TEST_ASSERT(st.tx_spoor == tx && st.rx_spoor == rx,
        "tx/rx pointers stored");

    p9_spoor_transport_destroy(&st);
    TEST_ASSERT(!p9_spoor_transport_is_open(&st),
        "is_open false after destroy");
    TEST_EXPECT_EQ((u32)st.magic, 0u,
        "magic cleared by destroy");

    spoor_clunk(tx);
    spoor_clunk(rx);
}

void test_spoor_transport_init_null_rejected(void) {
    static struct test_pipe tx_pipe, rx_pipe;
    struct Spoor *tx = make_test_spoor(&tx_pipe);
    struct Spoor *rx = make_test_spoor(&rx_pipe);

    struct p9_spoor_transport st = { 0 };
    TEST_EXPECT_EQ(p9_spoor_transport_init(NULL, tx, rx, false), -1,
        "NULL adapter rejected");
    TEST_EXPECT_EQ(p9_spoor_transport_init(&st, NULL, rx, false), -1,
        "NULL tx rejected");
    TEST_EXPECT_EQ(p9_spoor_transport_init(&st, tx, NULL, false), -1,
        "NULL rx rejected");
    TEST_EXPECT_EQ((u32)st.magic, 0u,
        "rejected init leaves magic untouched");

    spoor_clunk(tx);
    spoor_clunk(rx);
}

void test_spoor_transport_send_routes_to_tx_dev_write(void) {
    static struct test_pipe tx_pipe, rx_pipe;
    struct Spoor *tx = make_test_spoor(&tx_pipe);
    struct Spoor *rx = make_test_spoor(&rx_pipe);

    struct p9_spoor_transport st;
    p9_spoor_transport_init(&st, tx, rx, false);
    struct p9_transport_ops ops = p9_spoor_transport_ops(&st);

    const u8 payload[] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
    int sent = ops.send(ops.ctx, payload, sizeof(payload));
    TEST_EXPECT_EQ(sent, (int)sizeof(payload),
        "send returns full length");
    TEST_EXPECT_EQ(tx_pipe.write_pos, sizeof(payload),
        "tx buffer advanced by payload length");
    for (size_t i = 0; i < sizeof(payload); i++) {
        TEST_ASSERT(tx_pipe.buf[i] == payload[i],
            "tx buffer byte mismatch");
    }
    TEST_EXPECT_EQ(rx_pipe.write_pos, 0u,
        "rx buffer untouched by send");

    p9_spoor_transport_destroy(&st);
    spoor_clunk(tx);
    spoor_clunk(rx);
}

void test_spoor_transport_recv_routes_to_rx_dev_read(void) {
    static struct test_pipe tx_pipe, rx_pipe;
    struct Spoor *tx = make_test_spoor(&tx_pipe);
    struct Spoor *rx = make_test_spoor(&rx_pipe);

    // Pre-load rx buffer with a payload.
    const u8 payload[] = { 0xAA, 0xBB, 0xCC, 0xDD };
    for (size_t i = 0; i < sizeof(payload); i++) {
        rx_pipe.buf[i] = payload[i];
    }
    rx_pipe.write_pos = sizeof(payload);

    struct p9_spoor_transport st;
    p9_spoor_transport_init(&st, tx, rx, false);
    struct p9_transport_ops ops = p9_spoor_transport_ops(&st);

    u8 got[16] = { 0 };
    int n = ops.recv(ops.ctx, got, sizeof(got));
    TEST_EXPECT_EQ(n, (int)sizeof(payload),
        "recv returns payload length");
    for (size_t i = 0; i < sizeof(payload); i++) {
        TEST_ASSERT(got[i] == payload[i],
            "received bytes match payload");
    }
    TEST_EXPECT_EQ(rx_pipe.read_pos, sizeof(payload),
        "rx read cursor advanced");

    p9_spoor_transport_destroy(&st);
    spoor_clunk(tx);
    spoor_clunk(rx);
}

void test_spoor_transport_recv_empty_returns_zero(void) {
    static struct test_pipe tx_pipe, rx_pipe;
    struct Spoor *tx = make_test_spoor(&tx_pipe);
    struct Spoor *rx = make_test_spoor(&rx_pipe);

    struct p9_spoor_transport st;
    p9_spoor_transport_init(&st, tx, rx, false);
    struct p9_transport_ops ops = p9_spoor_transport_ops(&st);

    u8 got[8];
    int n = ops.recv(ops.ctx, got, sizeof(got));
    TEST_EXPECT_EQ(n, 0,
        "recv on empty rx returns 0 (EOF)");

    p9_spoor_transport_destroy(&st);
    spoor_clunk(tx);
    spoor_clunk(rx);
}

void test_spoor_transport_close_clunks_when_owned(void) {
    static struct test_pipe tx_pipe, rx_pipe;
    struct Spoor *tx = make_test_spoor(&tx_pipe);
    struct Spoor *rx = make_test_spoor(&rx_pipe);
    TEST_ASSERT(tx && rx, "Spoor alloc");
    u64 freed_before = spoor_total_freed();

    struct p9_spoor_transport st;
    p9_spoor_transport_init(&st, tx, rx, true);    // owns
    struct p9_transport_ops ops = p9_spoor_transport_ops(&st);

    TEST_EXPECT_EQ(ops.close(ops.ctx), 0,
        "close returns 0");
    TEST_EXPECT_EQ(spoor_total_freed() - freed_before, 2ull,
        "owned close clunks both Spoors (count = 2)");
    TEST_ASSERT(st.tx_spoor == NULL && st.rx_spoor == NULL,
        "close clears tx/rx pointers");

    // Second close is a no-op (idempotent).
    TEST_EXPECT_EQ(ops.close(ops.ctx), 0,
        "second close is idempotent no-op");
    TEST_EXPECT_EQ(spoor_total_freed() - freed_before, 2ull,
        "second close does not double-free");

    p9_spoor_transport_destroy(&st);
}

void test_spoor_transport_close_preserves_when_unowned(void) {
    static struct test_pipe tx_pipe, rx_pipe;
    struct Spoor *tx = make_test_spoor(&tx_pipe);
    struct Spoor *rx = make_test_spoor(&rx_pipe);
    u64 freed_before = spoor_total_freed();

    struct p9_spoor_transport st;
    p9_spoor_transport_init(&st, tx, rx, false);   // unowned
    struct p9_transport_ops ops = p9_spoor_transport_ops(&st);

    TEST_EXPECT_EQ(ops.close(ops.ctx), 0,
        "close returns 0");
    TEST_EXPECT_EQ(spoor_total_freed() - freed_before, 0ull,
        "unowned close does NOT clunk the Spoors");
    TEST_ASSERT(st.tx_spoor == tx && st.rx_spoor == rx,
        "unowned close leaves pointers (caller owns)");

    p9_spoor_transport_destroy(&st);
    spoor_clunk(tx);
    spoor_clunk(rx);
}

void test_spoor_transport_transport_core_round_trip(void) {
    static struct test_pipe tx_pipe, rx_pipe;
    static u8 recv_buf[1024];
    struct Spoor *tx = make_test_spoor(&tx_pipe);
    struct Spoor *rx = make_test_spoor(&rx_pipe);

    struct p9_spoor_transport st;
    p9_spoor_transport_init(&st, tx, rx, false);
    struct p9_transport_ops ops = p9_spoor_transport_ops(&st);

    struct p9_transport t;
    TEST_EXPECT_EQ(p9_transport_init(&t, ops, recv_buf, sizeof(recv_buf)), 0,
        "transport init");

    // Build a fake Tmsg frame: 11-byte Tversion-ish. Size = 11; type
    // = TVERSION (100); tag = NOTAG; msize = 4096; version = "9P2000.L"
    // (8 bytes). We construct it by hand to keep the test independent
    // of the wire builders' interpretation.
    u8 req[64];
    size_t off = 0;
    u32 msize_val = 4096;
    const char *version = "9P2000.L";
    size_t vlen = 8;
    u32 frame_size = 4 + 1 + 2 + 4 + 2 + (u32)vlen;
    req[off++] = (u8)(frame_size & 0xff);
    req[off++] = (u8)((frame_size >> 8) & 0xff);
    req[off++] = (u8)((frame_size >> 16) & 0xff);
    req[off++] = (u8)((frame_size >> 24) & 0xff);
    req[off++] = 100;                          // TVERSION
    req[off++] = 0xff; req[off++] = 0xff;      // tag=NOTAG
    req[off++] = (u8)(msize_val & 0xff);
    req[off++] = (u8)((msize_val >> 8) & 0xff);
    req[off++] = (u8)((msize_val >> 16) & 0xff);
    req[off++] = (u8)((msize_val >> 24) & 0xff);
    req[off++] = (u8)(vlen & 0xff);
    req[off++] = (u8)((vlen >> 8) & 0xff);
    for (size_t i = 0; i < vlen; i++) req[off++] = (u8)version[i];

    TEST_EXPECT_EQ(p9_transport_send(&t, req, off), 0,
        "transport send through Spoor adapter");
    TEST_EXPECT_EQ(tx_pipe.write_pos, off,
        "tx Spoor accumulated full frame");

    // Inline responder: synthesize Rversion (type=101) with the same
    // msize + version. The transport's recv will pick this up.
    u8 resp[64];
    size_t roff = 0;
    u32 resp_size = 4 + 1 + 2 + 4 + 2 + (u32)vlen;
    resp[roff++] = (u8)(resp_size & 0xff);
    resp[roff++] = (u8)((resp_size >> 8) & 0xff);
    resp[roff++] = (u8)((resp_size >> 16) & 0xff);
    resp[roff++] = (u8)((resp_size >> 24) & 0xff);
    resp[roff++] = 101;                        // RVERSION
    resp[roff++] = 0xff; resp[roff++] = 0xff;  // tag=NOTAG
    resp[roff++] = (u8)(msize_val & 0xff);
    resp[roff++] = (u8)((msize_val >> 8) & 0xff);
    resp[roff++] = (u8)((msize_val >> 16) & 0xff);
    resp[roff++] = (u8)((msize_val >> 24) & 0xff);
    resp[roff++] = (u8)(vlen & 0xff);
    resp[roff++] = (u8)((vlen >> 8) & 0xff);
    for (size_t i = 0; i < vlen; i++) resp[roff++] = (u8)version[i];
    for (size_t i = 0; i < roff; i++) rx_pipe.buf[i] = resp[i];
    rx_pipe.write_pos = roff;

    int got_len = p9_transport_recv(&t);
    TEST_EXPECT_EQ(got_len, (int)resp_size,
        "transport recv length matches frame");
    TEST_EXPECT_EQ(recv_buf[4], 101u,
        "received type is RVERSION");

    p9_transport_destroy(&t);
    p9_spoor_transport_destroy(&st);
    spoor_clunk(tx);
    spoor_clunk(rx);
}

void test_spoor_transport_end_to_end_handshake(void) {
    static struct test_pipe tx_pipe, rx_pipe;
    static u8 recv_buf[2048];
    struct Spoor *tx = make_test_spoor(&tx_pipe);
    struct Spoor *rx = make_test_spoor(&rx_pipe);

    struct p9_spoor_transport st;
    p9_spoor_transport_init(&st, tx, rx, false);
    struct p9_transport_ops ops = p9_spoor_transport_ops(&st);

    struct p9_transport t;
    TEST_EXPECT_EQ(p9_transport_init(&t, ops, recv_buf, sizeof(recv_buf)), 0,
        "transport init");

    struct p9_session s;
    TEST_EXPECT_EQ(p9_session_init(&s, /*root_fid=*/1, /*msize=*/4096), 0,
        "session init");

    // Tversion. session.send_version produces the frame; transport
    // sends it through tx; inline responder peeks tx + writes the
    // canonical Rversion to rx; transport recv aggregates; session
    // dispatch transitions INIT → VERSIONED.
    u8 out_buf[256];
    int out_rc = p9_session_send_version(&s,
                                         out_buf, sizeof(out_buf),
                                         (const u8 *)"9P2000.L", 8);
    TEST_ASSERT(out_rc > 0, "send_version builds Tversion");
    size_t out_len = (size_t)out_rc;
    TEST_EXPECT_EQ(p9_transport_send(&t, out_buf, out_len), 0,
        "transport send Tversion");
    u32 frame_in = peek_frame_size(&tx_pipe);
    TEST_ASSERT(frame_in == out_len,
        "tx Spoor has full Tversion frame");
    // Drain the request from tx (test responder reads it).
    tx_pipe.read_pos = tx_pipe.write_pos;

    // Synthesize Rversion: type=101, tag=NOTAG, msize=4096, version.
    u8 resp[64];
    size_t roff = 0;
    const char *version = "9P2000.L";
    size_t vlen = 8;
    u32 resp_size = 4 + 1 + 2 + 4 + 2 + (u32)vlen;
    resp[roff++] = (u8)(resp_size & 0xff);
    resp[roff++] = (u8)((resp_size >> 8) & 0xff);
    resp[roff++] = (u8)((resp_size >> 16) & 0xff);
    resp[roff++] = (u8)((resp_size >> 24) & 0xff);
    resp[roff++] = 101;
    resp[roff++] = 0xff; resp[roff++] = 0xff;
    resp[roff++] = 0x00; resp[roff++] = 0x10; resp[roff++] = 0; resp[roff++] = 0;  // 4096
    resp[roff++] = (u8)(vlen & 0xff);
    resp[roff++] = (u8)((vlen >> 8) & 0xff);
    for (size_t i = 0; i < vlen; i++) resp[roff++] = (u8)version[i];
    for (size_t i = 0; i < roff; i++) rx_pipe.buf[rx_pipe.write_pos + i] = resp[i];
    rx_pipe.write_pos += roff;

    struct p9_dispatch_result r;
    TEST_EXPECT_EQ(p9_transport_recv(&t), (int)resp_size,
        "transport recv Rversion");
    TEST_EXPECT_EQ(p9_session_dispatch_rmsg(&s, recv_buf, resp_size, &r), 0,
        "session dispatch Rversion");
    TEST_EXPECT_EQ((int)s.state, (int)P9_SESS_VERSIONED,
        "session transitioned to VERSIONED");
    TEST_EXPECT_EQ(s.negotiated_msize, 4096u,
        "msize negotiated");

    // Tattach. send_attach produces the frame; same loop.
    out_rc = p9_session_send_attach(&s,
                                    out_buf, sizeof(out_buf),
                                    (const u8 *)"none", 4,
                                    (const u8 *)"", 0,
                                    /*n_uname=*/0);
    TEST_ASSERT(out_rc > 0, "send_attach builds Tattach");
    out_len = (size_t)out_rc;
    TEST_EXPECT_EQ(p9_transport_send(&t, out_buf, out_len), 0,
        "transport send Tattach");
    tx_pipe.read_pos = tx_pipe.write_pos;     // drain tx

    // Synthesize Rattach: type=105, tag=<allocated tag>, qid (13 bytes).
    // tag is allocated by session; peek it from out_buf header byte 5.
    u8 tag_lo = out_buf[5];
    u8 tag_hi = out_buf[6];
    u8 rattach[32];
    size_t aoff = 0;
    u32 rsize = 4 + 1 + 2 + 13;       // header + qid
    rattach[aoff++] = (u8)(rsize & 0xff);
    rattach[aoff++] = (u8)((rsize >> 8) & 0xff);
    rattach[aoff++] = (u8)((rsize >> 16) & 0xff);
    rattach[aoff++] = (u8)((rsize >> 24) & 0xff);
    rattach[aoff++] = 105;
    rattach[aoff++] = tag_lo; rattach[aoff++] = tag_hi;
    // qid: type=QTDIR, vers=0, path=0
    rattach[aoff++] = 0x80;
    for (int i = 0; i < 4 + 8; i++) rattach[aoff++] = 0;
    for (size_t i = 0; i < aoff; i++) rx_pipe.buf[rx_pipe.write_pos + i] = rattach[i];
    rx_pipe.write_pos += aoff;

    TEST_EXPECT_EQ(p9_transport_recv(&t), (int)rsize,
        "transport recv Rattach");
    TEST_EXPECT_EQ(p9_session_dispatch_rmsg(&s, recv_buf, rsize, &r), 0,
        "session dispatch Rattach");
    TEST_EXPECT_EQ((int)s.state, (int)P9_SESS_OPEN,
        "session transitioned to OPEN");
    TEST_ASSERT(p9_session_fid_bound(&s, 1),
        "root fid bound by Tattach");

    p9_session_destroy(&s);
    p9_transport_destroy(&t);
    p9_spoor_transport_destroy(&st);
    spoor_clunk(tx);
    spoor_clunk(rx);
}
