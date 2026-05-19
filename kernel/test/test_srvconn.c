// P5-corvus-srv-impl-a3a — kernel-internal tests for the SrvConn
// connection object and its bidirectional tsleep-bounded transport.
//
// Coverage:
//
//   srvconn.create_destroy
//     srvconn_create stamps the peer identity BY VALUE (CORVUS-DESIGN
//     §6.3); the connection is born LIVE with refcount 1; the stamped
//     identity survives the peer Proc being freed (no use-after-free);
//     the last srvconn_unref frees it.
//
//   srvconn.roundtrip
//     Bytes written by the kernel client (c2s) are read back by the
//     server side and vice versa (s2c); a server recv on an empty live
//     ring reads 0 (poll again); a client recv with data already
//     buffered returns at once without blocking.
//
//   srvconn.ring_capacity
//     A ring holds SRVCONN_RING_CAP bytes; a write into a full ring is
//     refused (0 accepted, never blocks); the ring wraps correctly
//     across a second fill/drain cycle.
//
//   srvconn.recv_blocks_then_wakes
//     A client recv on an empty ring blocks (THREAD_SLEEPING, the
//     connection's rendez waiter); a server send wakes it and it reads
//     the bytes.
//
//   srvconn.recv_deadline_timeout
//     A client recv past its deadline returns -1 with the timed-out
//     signal set; setting a fresh deadline clears the signal.
//
//   srvconn.teardown_eofs
//     After srvconn_teardown the connection is not live, residual bytes
//     still drain, then client recv reads EOF (0), server recv reads
//     EOF (-1), and every send is refused.
//
//   srvconn.teardown_wakes_blocked
//     srvconn_teardown wakes a client recv that is blocked on an empty
//     ring; it returns EOF (0) — a corvus crash never wedges a client.

#include "test.h"

#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>
#include <thylacine/srvconn.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

void test_srvconn_create_destroy(void);
void test_srvconn_roundtrip(void);
void test_srvconn_ring_capacity(void);
void test_srvconn_recv_blocks_then_wakes(void);
void test_srvconn_recv_deadline_timeout(void);
void test_srvconn_teardown_eofs(void);
void test_srvconn_teardown_wakes_blocked(void);

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

static void fill_pattern(u8 *buf, long n, u8 seed) {
    for (long i = 0; i < n; i++) buf[i] = (u8)(seed + (u8)i);
}

static bool check_pattern(const u8 *buf, long n, u8 seed) {
    for (long i = 0; i < n; i++) {
        if (buf[i] != (u8)(seed + (u8)i)) return false;
    }
    return true;
}

static void drop_test_proc(struct Proc *p) {
    if (!p) return;
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// srvconn.create_destroy
// ---------------------------------------------------------------------------

void test_srvconn_create_destroy(void) {
    u64 created0 = srvconn_total_created();
    u64 freed0   = srvconn_total_freed();

    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u64 want_stripes = proc_stripes(p);
    int want_pid     = p->pid;

    struct SrvConn *cn = srvconn_create(want_stripes, want_pid, false);
    TEST_ASSERT(cn != NULL, "srvconn_create");
    TEST_EXPECT_EQ(srvconn_total_created(), created0 + 1,
        "create bumps the created counter");
    TEST_ASSERT(srvconn_is_live(cn), "a fresh connection is LIVE");
    TEST_EXPECT_EQ(cn->peer_stripes, want_stripes,
        "peer stripes stamped by value at create");
    TEST_EXPECT_EQ(cn->peer_pid, want_pid,
        "peer pid stamped by value at create");
    TEST_ASSERT(cn->peer_console == false, "peer console bit stamped");
    TEST_EXPECT_EQ(cn->ref, 1, "a fresh connection has refcount 1");
    TEST_ASSERT(srvconn_client(cn) != NULL,
        "the dedicated kernel 9P client is present");

    // The peer Proc is freed — the by-value identity must survive it
    // (CORVUS-DESIGN §6.3: the SrvConn holds no raw Proc*).
    drop_test_proc(p);
    TEST_EXPECT_EQ(cn->peer_stripes, want_stripes,
        "stamped stripes survive the peer Proc being freed (no UAF)");

    srvconn_unref(cn);
    TEST_EXPECT_EQ(srvconn_total_freed(), freed0 + 1,
        "the last unref frees the connection");
}

// ---------------------------------------------------------------------------
// srvconn.roundtrip
// ---------------------------------------------------------------------------

void test_srvconn_roundtrip(void) {
    struct SrvConn *cn = srvconn_create(0x1111u, 11, false);
    TEST_ASSERT(cn != NULL, "srvconn_create");

    u8 out[64];
    u8 in[64];

    // c2s: the kernel client writes, the server reads it back.
    fill_pattern(out, 40, 0x20);
    TEST_EXPECT_EQ(srvconn_client_send(cn, out, 40), 40L,
        "client_send accepts all 40 bytes");
    TEST_EXPECT_EQ(srvconn_server_recv(cn, in, sizeof in), 40L,
        "server_recv drains all 40 bytes");
    TEST_ASSERT(check_pattern(in, 40, 0x20), "c2s bytes round-trip intact");
    TEST_EXPECT_EQ(srvconn_server_recv(cn, in, sizeof in), 0L,
        "server_recv on an empty live ring reads 0 (poll again)");

    // s2c: the server writes, the kernel client reads it — data is
    // already buffered, so the recv returns at once without blocking.
    fill_pattern(out, 33, 0x80);
    TEST_EXPECT_EQ(srvconn_server_send(cn, out, 33), 33L,
        "server_send accepts all 33 bytes");
    TEST_EXPECT_EQ(srvconn_client_recv(cn, in, sizeof in), 33L,
        "client_recv returns buffered bytes without blocking");
    TEST_ASSERT(check_pattern(in, 33, 0x80), "s2c bytes round-trip intact");

    srvconn_unref(cn);
}

// ---------------------------------------------------------------------------
// srvconn.ring_capacity
// ---------------------------------------------------------------------------
//
// A 1 KiB scratch reused for chunked send/recv so the test never holds
// a SRVCONN_RING_CAP-sized buffer (which would bloat the kernel image's
// .bss against the 2 MiB L3-block ceiling).

static u8 g_sc_chunk[1024];

// sc_send_chunked — client-side send of `total` pattern bytes, ≤1 KiB
// per call. Returns true iff every chunk was fully accepted.
static bool sc_send_chunked(struct SrvConn *cn, long total, u8 seed) {
    for (long off = 0; off < total; ) {
        long want = total - off;
        if (want > (long)sizeof g_sc_chunk) want = (long)sizeof g_sc_chunk;
        for (long j = 0; j < want; j++) {
            g_sc_chunk[j] = (u8)(seed + (u8)(off + j));
        }
        if (srvconn_client_send(cn, g_sc_chunk, want) != want) return false;
        off += want;
    }
    return true;
}

// sc_recv_verify_chunked — server-side drain of `total` bytes, ≤1 KiB
// per call, verifying each against the (seed + index) pattern.
static bool sc_recv_verify_chunked(struct SrvConn *cn, long total, u8 seed) {
    for (long off = 0; off < total; ) {
        long want = total - off;
        if (want > (long)sizeof g_sc_chunk) want = (long)sizeof g_sc_chunk;
        if (srvconn_server_recv(cn, g_sc_chunk, want) != want) return false;
        for (long j = 0; j < want; j++) {
            if (g_sc_chunk[j] != (u8)(seed + (u8)(off + j))) return false;
        }
        off += want;
    }
    return true;
}

void test_srvconn_ring_capacity(void) {
    struct SrvConn *cn = srvconn_create(0x2222u, 22, false);
    TEST_ASSERT(cn != NULL, "srvconn_create");

    // Fill c2s to capacity; one further byte is refused (0 accepted —
    // a full ring never blocks the writer).
    TEST_ASSERT(sc_send_chunked(cn, SRVCONN_RING_CAP, 0x01),
        "client_send fills the ring to capacity");
    u8 one = 0xee;
    TEST_EXPECT_EQ(srvconn_client_send(cn, &one, 1), 0L,
        "a write into a full ring is refused (0), never blocked");
    TEST_ASSERT(sc_recv_verify_chunked(cn, SRVCONN_RING_CAP, 0x01),
        "server_recv drains the full ring, bytes intact");

    // The first 5 KiB fill/drain positions head + tail mid-ring; the
    // second then crosses the ring's end — exercising the two-segment
    // wrap in both chan_ring_write and chan_ring_read.
    TEST_ASSERT(sc_send_chunked(cn, 5000, 0x40), "pre-wrap fill accepted");
    TEST_ASSERT(sc_recv_verify_chunked(cn, 5000, 0x40),
        "pre-wrap drain intact");
    TEST_ASSERT(sc_send_chunked(cn, 5000, 0x90),
        "wrapping fill accepted");
    TEST_ASSERT(sc_recv_verify_chunked(cn, 5000, 0x90),
        "wrapping drain returns all bytes across the ring's end");

    srvconn_unref(cn);
}

// ---------------------------------------------------------------------------
// Threaded recv handoff — shared state.
// ---------------------------------------------------------------------------
//
//   g_sc_conn  — the connection under test.
//   g_sc_ran   — consumer increments: pre-recv → 1, post-recv → 2.
//   g_sc_ret   — the value srvconn_client_recv returned in the consumer.
//   g_sc_buf   — the consumer's recv destination.

static struct SrvConn   *g_sc_conn;
static volatile u32      g_sc_ran;
static volatile long     g_sc_ret;
static u8                g_sc_buf[64];

static void sc_recv_consumer(void) {
    g_sc_ran++;                                          // → 1: pre-recv
    g_sc_ret = srvconn_client_recv(g_sc_conn, g_sc_buf, sizeof g_sc_buf);
    g_sc_ran++;                                          // → 2: post-recv
    for (;;) sched();                                    // park safely
}

// ---------------------------------------------------------------------------
// srvconn.recv_blocks_then_wakes
// ---------------------------------------------------------------------------

void test_srvconn_recv_blocks_then_wakes(void) {
    struct SrvConn *cn = srvconn_create(0x3333u, 33, false);
    TEST_ASSERT(cn != NULL, "srvconn_create");

    g_sc_conn = cn;
    g_sc_ran  = 0;
    g_sc_ret  = -999;

    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree must be empty at test entry");

    struct Thread *consumer = thread_create(kproc(), sc_recv_consumer);
    TEST_ASSERT(consumer != NULL, "thread_create(consumer)");
    ready(consumer);

    // Yield: consumer runs, increments to 1, blocks in client_recv on
    // the empty s2c ring (no deadline) → SLEEPING.
    sched();
    TEST_EXPECT_EQ(g_sc_ran, 1u, "consumer ran once before blocking");
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer is SLEEPING inside client_recv");
    TEST_EXPECT_EQ(cn->s2c.rendez.waiter, consumer,
        "consumer is the s2c rendez waiter");

    // The server writes — the wakeup releases the blocked consumer.
    u8 out[16];
    fill_pattern(out, 16, 0x55);
    TEST_EXPECT_EQ(srvconn_server_send(cn, out, 16), 16L,
        "server_send accepts the bytes");

    sched();
    TEST_EXPECT_EQ(g_sc_ran, 2u, "consumer resumed past the recv");
    TEST_EXPECT_EQ(g_sc_ret, 16L, "client_recv returned the 16 bytes");
    TEST_ASSERT(check_pattern(g_sc_buf, 16, 0x55),
        "the woken consumer read the server's bytes intact");

    thread_free(consumer);
    srvconn_unref(cn);
}

// ---------------------------------------------------------------------------
// srvconn.recv_deadline_timeout
// ---------------------------------------------------------------------------

void test_srvconn_recv_deadline_timeout(void) {
    struct SrvConn *cn = srvconn_create(0x4444u, 44, false);
    TEST_ASSERT(cn != NULL, "srvconn_create");

    // deadline_ns == 1 — a timestamp long in the past. The s2c ring is
    // empty, so client_recv must time out at once rather than block.
    srvconn_set_client_deadline(cn, 1);
    TEST_ASSERT(srvconn_client_timed_out(cn) == false,
        "setting a deadline clears the timed-out signal");

    u8 in[16];
    TEST_EXPECT_EQ(srvconn_client_recv(cn, in, sizeof in), -1L,
        "client_recv past its deadline returns -1");
    TEST_ASSERT(srvconn_client_timed_out(cn) == true,
        "the timed-out signal is set after a deadline expiry");

    // A fresh deadline clears the signal again.
    srvconn_set_client_deadline(cn, 0);
    TEST_ASSERT(srvconn_client_timed_out(cn) == false,
        "a fresh deadline clears the timed-out signal");

    srvconn_unref(cn);
}

// ---------------------------------------------------------------------------
// srvconn.teardown_eofs
// ---------------------------------------------------------------------------

void test_srvconn_teardown_eofs(void) {
    struct SrvConn *cn = srvconn_create(0x5555u, 55, false);
    TEST_ASSERT(cn != NULL, "srvconn_create");

    // Buffer residual bytes in BOTH directions, then tear down. The
    // residual must still drain before EOF surfaces.
    u8 out[16];
    u8 in[16];
    fill_pattern(out, 8, 0x10);
    TEST_EXPECT_EQ(srvconn_client_send(cn, out, 8), 8L, "c2s residual queued");
    fill_pattern(out, 8, 0x90);
    TEST_EXPECT_EQ(srvconn_server_send(cn, out, 8), 8L, "s2c residual queued");

    srvconn_teardown(cn);
    TEST_ASSERT(srvconn_is_live(cn) == false,
        "the connection is no longer live after teardown");

    // Residual drains first.
    TEST_EXPECT_EQ(srvconn_server_recv(cn, in, sizeof in), 8L,
        "server_recv drains the c2s residual after teardown");
    TEST_ASSERT(check_pattern(in, 8, 0x10), "c2s residual intact");
    TEST_EXPECT_EQ(srvconn_client_recv(cn, in, sizeof in), 8L,
        "client_recv drains the s2c residual after teardown");
    TEST_ASSERT(check_pattern(in, 8, 0x90), "s2c residual intact");

    // Drained — now EOF surfaces on each side.
    TEST_EXPECT_EQ(srvconn_client_recv(cn, in, sizeof in), 0L,
        "client_recv reads EOF (0) once the ring is drained");
    TEST_EXPECT_EQ(srvconn_server_recv(cn, in, sizeof in), -1L,
        "server_recv reads EOF (-1) once the ring is drained");

    // Every send is refused after teardown.
    TEST_EXPECT_EQ(srvconn_client_send(cn, out, 8), -1L,
        "client_send is refused after teardown");
    TEST_EXPECT_EQ(srvconn_server_send(cn, out, 8), -1L,
        "server_send is refused after teardown");

    // Teardown is idempotent.
    srvconn_teardown(cn);
    TEST_ASSERT(srvconn_is_live(cn) == false, "teardown is idempotent");

    srvconn_unref(cn);
}

// ---------------------------------------------------------------------------
// srvconn.teardown_wakes_blocked
// ---------------------------------------------------------------------------

void test_srvconn_teardown_wakes_blocked(void) {
    struct SrvConn *cn = srvconn_create(0x6666u, 66, false);
    TEST_ASSERT(cn != NULL, "srvconn_create");

    g_sc_conn = cn;
    g_sc_ran  = 0;
    g_sc_ret  = -999;

    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree must be empty at test entry");

    struct Thread *consumer = thread_create(kproc(), sc_recv_consumer);
    TEST_ASSERT(consumer != NULL, "thread_create(consumer)");
    ready(consumer);

    // Yield: consumer blocks in client_recv on the empty s2c ring.
    sched();
    TEST_EXPECT_EQ(g_sc_ran, 1u, "consumer ran once before blocking");
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer is SLEEPING inside client_recv");

    // Teardown — modeling a corvus crash — must wake the blocked
    // consumer with EOF, never wedge it.
    srvconn_teardown(cn);

    sched();
    TEST_EXPECT_EQ(g_sc_ran, 2u, "teardown released the blocked consumer");
    TEST_EXPECT_EQ(g_sc_ret, 0L, "the woken consumer read EOF (0)");

    thread_free(consumer);
    srvconn_unref(cn);
}
