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
void test_srvconn_server_send_blocks_then_drain_wakes(void);

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

    struct SrvConn *cn = srvconn_create(want_stripes, want_pid, false, 0, SRVCONN_MSIZE);
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
    struct SrvConn *cn = srvconn_create(0x1111u, 11, false, 0, SRVCONN_MSIZE);
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
    struct SrvConn *cn = srvconn_create(0x2222u, 22, false, 0, SRVCONN_MSIZE);
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
static volatile bool     g_sc_exited;   // #109: terminal-park reap handshake

static void sc_recv_consumer(void) {
    g_sc_ran++;                                          // → 1: pre-recv
    g_sc_ret = srvconn_client_recv(g_sc_conn, g_sc_buf, sizeof g_sc_buf);
    g_sc_ran++;                                          // → 2: post-recv
    test_kthread_park_terminal(&g_sc_exited);            // #109: EXITING park
}

// ---------------------------------------------------------------------------
// srvconn.recv_blocks_then_wakes
// ---------------------------------------------------------------------------

void test_srvconn_recv_blocks_then_wakes(void) {
    struct SrvConn *cn = srvconn_create(0x3333u, 33, false, 0, SRVCONN_MSIZE);
    TEST_ASSERT(cn != NULL, "srvconn_create");

    g_sc_conn = cn;
    g_sc_ran  = 0;
    g_sc_ret  = -999;
    g_sc_exited = false;

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

    test_kthread_join_free(consumer, &g_sc_exited);
    srvconn_unref(cn);
}

// ---------------------------------------------------------------------------
// srvconn.server_send_blocks_then_drain_wakes (#348)
// ---------------------------------------------------------------------------
//
// The s2c twin of recv_blocks_then_wakes. A blocking SERVER send into a
// FULL s2c ring must PARK on wrendez (the new #348 behavior) -- never
// return 0, which a 9P-server Proc's write_full treats as EPIPE and closes
// the kernel-attached mount on (the go-build EIO cascade). The kernel
// client's drain (srvconn_client_recv) must then WAKE the producer to
// deliver the whole payload. Non-vacuous: pre-fix srvconn_server_send
// returned 0 on a full ring (no wrendez, no park).

static struct SrvConn   *g_ss_conn;
static volatile u32      g_ss_ran;       // producer: pre-send → 1, post-send → 2
static volatile long     g_ss_ret;       // server_send_blocking's return value
static volatile bool     g_ss_exited;    // #109 terminal-park reap handshake
static u8                g_ss_payload[256];

static void ss_send_producer(void) {
    g_ss_ran++;                                          // → 1: pre-send
    g_ss_ret = srvconn_server_send_blocking(g_ss_conn, g_ss_payload,
                                            (long)sizeof g_ss_payload);
    g_ss_ran++;                                          // → 2: post-send
    test_kthread_park_terminal(&g_ss_exited);            // #109: EXITING park
}

void test_srvconn_server_send_blocks_then_drain_wakes(void) {
    struct SrvConn *cn = srvconn_create(0x6666u, 66, false, 0, SRVCONN_MSIZE);
    TEST_ASSERT(cn != NULL, "srvconn_create");

    g_ss_conn   = cn;
    g_ss_ran    = 0;
    g_ss_ret    = -999;
    g_ss_exited = false;
    fill_pattern(g_ss_payload, sizeof g_ss_payload, 0x55);

    // Fill s2c to capacity with a continuous 0xAA ramp via the non-blocking
    // server_send (≤1 KiB chunks — no SRVCONN_RING_CAP stack buffer). The
    // producer's first chan_produce then writes nothing and parks.
    long filled = 0;
    while (filled < (long)SRVCONN_RING_CAP) {
        long want = (long)SRVCONN_RING_CAP - filled;
        if (want > (long)sizeof g_sc_chunk) want = (long)sizeof g_sc_chunk;
        for (long j = 0; j < want; j++)
            g_sc_chunk[j] = (u8)(0xAA + (u8)(filled + j));
        TEST_EXPECT_EQ(srvconn_server_send(cn, g_sc_chunk, want), want,
            "s2c prefill chunk accepted");
        filled += want;
    }
    TEST_EXPECT_EQ(cn->s2c.count, SRVCONN_RING_CAP, "s2c filled to capacity");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty at test entry");

    struct Thread *producer = thread_create(kproc(), ss_send_producer);
    TEST_ASSERT(producer != NULL, "thread_create(producer)");
    ready(producer);

    // Yield: producer runs, increments to 1, writes nothing (s2c is full),
    // and PARKS on wrendez -- the load-bearing #348 behavior. Pre-fix it
    // would have returned 0 here and incremented to 2.
    sched();
    TEST_EXPECT_EQ(g_ss_ran, 1u, "producer ran once before blocking");
    TEST_EXPECT_EQ(producer->state, THREAD_SLEEPING,
        "producer is SLEEPING inside server_send_blocking");
    TEST_EXPECT_EQ(cn->s2c.wrendez.waiter, producer,
        "producer parked on the s2c WRENDEZ (the new #348 mechanism)");
    TEST_ASSERT(cn->s2c.rendez.waiter == NULL,
        "the reader rendez has NO waiter -- separate from wrendez");
    TEST_ASSERT(cn->s2c.writing == true,
        "the single-writer busy-guard is held across the park");
    TEST_EXPECT_EQ(g_ss_ret, -999L, "producer has NOT returned (still blocked)");

    // The kernel client drains s2c -> the drain-wake (wakeup wrendez)
    // releases the producer. One 256-byte drain frees enough room.
    u8 in[256];
    TEST_EXPECT_EQ(srvconn_client_recv(cn, in, sizeof in), 256L,
        "client_recv drains a chunk of the prefill");

    sched();
    TEST_EXPECT_EQ(g_ss_ran, 2u, "producer resumed past the blocking send");
    TEST_EXPECT_EQ(g_ss_ret, (long)sizeof g_ss_payload,
        "server_send_blocking delivered the WHOLE payload");
    TEST_ASSERT(cn->s2c.writing == false,
        "the busy-guard is released after the send completes");

    // Verify the payload landed intact at the tail. After the 256-byte
    // drain + the producer's 256-byte write, s2c holds (CAP-256) prefill
    // (0xAA ramp from offset 256) then the 256-byte payload (0x55 ramp).
    long off = 256;
    long remain = (long)SRVCONN_RING_CAP - 256;
    while (remain > 0) {
        long want = remain;
        if (want > (long)sizeof in) want = (long)sizeof in;
        TEST_EXPECT_EQ(srvconn_client_recv(cn, in, want), want,
            "drain remaining prefill");
        for (long j = 0; j < want; j++)
            TEST_ASSERT(in[j] == (u8)(0xAA + (u8)(off + j)),
                "prefill byte intact");
        off += want;
        remain -= want;
    }
    TEST_EXPECT_EQ(srvconn_client_recv(cn, in, 256), 256L, "drain payload");
    TEST_ASSERT(check_pattern(in, 256, 0x55),
        "the producer's payload landed intact at the stream tail");

    test_kthread_join_free(producer, &g_ss_exited);
    srvconn_unref(cn);
}

// ---------------------------------------------------------------------------
// srvconn.recv_deadline_timeout
// ---------------------------------------------------------------------------

void test_srvconn_recv_deadline_timeout(void) {
    struct SrvConn *cn = srvconn_create(0x4444u, 44, false, 0, SRVCONN_MSIZE);
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
    struct SrvConn *cn = srvconn_create(0x5555u, 55, false, 0, SRVCONN_MSIZE);
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
    struct SrvConn *cn = srvconn_create(0x6666u, 66, false, 0, SRVCONN_MSIZE);
    TEST_ASSERT(cn != NULL, "srvconn_create");

    g_sc_conn = cn;
    g_sc_ran  = 0;
    g_sc_ret  = -999;
    g_sc_exited = false;

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

    test_kthread_join_free(consumer, &g_sc_exited);
    srvconn_unref(cn);
}

// ---------------------------------------------------------------------------
// srvconn.bulk_ring_class (CF-3 B)
// ---------------------------------------------------------------------------
//
// A DMSRVBULK-class conn carries rings of 2 x SRVCONN_BULK_MSIZE and
// reports its msize; anything outside the two-point class policy is
// rejected. The >64 KiB stream round-trip is the load-bearing part:
// pre-CF-3-B the ring capacity was a compile-time 64 KiB, so buffering
// 96 KiB without a drain was impossible -- this proves the dynamic cap
// (and the word-wise chan_copy) end to end.

void test_srvconn_bulk_ring_class(void) {
    // The two-point policy: anything else is refused.
    TEST_ASSERT(srvconn_create(1, 1, false, 0, 12345u) == NULL,
        "an arbitrary ring msize is rejected");
    TEST_ASSERT(srvconn_create(1, 1, false, 0, 0u) == NULL,
        "a zero ring msize is rejected");

    struct SrvConn *cn = srvconn_create(0x7777u, 77, false, 0,
                                        SRVCONN_BULK_MSIZE);
    TEST_ASSERT(cn != NULL, "srvconn_create(bulk)");
    TEST_EXPECT_EQ(srvconn_msize(cn), SRVCONN_BULK_MSIZE,
        "bulk conn reports the bulk msize");
    TEST_EXPECT_EQ(cn->c2s.cap, 2u * SRVCONN_BULK_MSIZE,
        "c2s ring sized 2x the bulk msize");
    TEST_EXPECT_EQ(cn->s2c.cap, 2u * SRVCONN_BULK_MSIZE,
        "s2c ring sized 2x the bulk msize");

    // Buffer 96 KiB into c2s WITHOUT a drain (impossible at the old
    // 64 KiB cap), then drain + byte-verify the ramp.
    const long total = 96 * 1024;
    long put = 0;
    while (put < total) {
        long want = total - put;
        if (want > (long)sizeof g_sc_chunk) want = (long)sizeof g_sc_chunk;
        for (long j = 0; j < want; j++)
            g_sc_chunk[j] = (u8)(0x30 + (u8)(put + j));
        TEST_EXPECT_EQ(srvconn_client_send(cn, g_sc_chunk, want), want,
            "bulk c2s accepts the chunk whole (past the old 64 KiB cap)");
        put += want;
    }
    TEST_EXPECT_EQ(cn->c2s.count, (u32)total, "96 KiB buffered undrained");

    long got = 0;
    while (got < total) {
        long want = total - got;
        if (want > (long)sizeof g_sc_chunk) want = (long)sizeof g_sc_chunk;
        TEST_EXPECT_EQ(srvconn_server_recv(cn, g_sc_chunk, want), want,
            "bulk c2s drains the chunk whole");
        for (long j = 0; j < want; j++)
            TEST_ASSERT(g_sc_chunk[j] == (u8)(0x30 + (u8)(got + j)),
                "bulk stream byte intact (word-wise chan_copy)");
        got += want;
    }
    TEST_EXPECT_EQ(cn->c2s.count, 0u, "bulk ring fully drained");

    // The kernel client's ALL-OR-NOTHING frame send must accept a
    // bulk-class frame whole on the empty bulk ring -- the boot-wedge
    // regression: a stale default-cap `freeb` bound in
    // srvconn_client_send_frame made a 128 KiB Twrite frame "not fit"
    // (65536 - count vs n), so client_send_flow EAGAIN-spun forever on
    // the FIRST bulk write (the fsbench hang). 96 KiB > the old 64 KiB
    // cap; must be accepted whole, frame-atomically, in ONE call.
    {
        static u8 bigframe[96 * 1024];
        for (long j = 0; j < (long)sizeof bigframe; j++)
            bigframe[j] = (u8)(0x51 + (u8)j);
        TEST_EXPECT_EQ(srvconn_client_send_frame(cn, bigframe,
                                                 (long)sizeof bigframe),
                       (long)sizeof bigframe,
            "a >default-cap frame is accepted WHOLE on the bulk ring "
            "(the fsbench-wedge regression)");
        long drained = 0;
        while (drained < (long)sizeof bigframe) {
            long want = (long)sizeof bigframe - drained;
            if (want > (long)sizeof g_sc_chunk) want = (long)sizeof g_sc_chunk;
            TEST_EXPECT_EQ(srvconn_server_recv(cn, g_sc_chunk, want), want,
                "drain the big frame");
            for (long j = 0; j < want; j++)
                TEST_ASSERT(g_sc_chunk[j] == (u8)(0x51 + (u8)(drained + j)),
                    "big-frame byte intact");
            drained += want;
        }
    }

    // A default conn still reports the default class.
    struct SrvConn *dn = srvconn_create(0x7778u, 78, false, 0, SRVCONN_MSIZE);
    TEST_ASSERT(dn != NULL, "srvconn_create(default)");
    TEST_EXPECT_EQ(srvconn_msize(dn), SRVCONN_MSIZE,
        "default conn reports the default msize");
    TEST_EXPECT_EQ(dn->s2c.cap, SRVCONN_RING_CAP,
        "default ring cap unchanged (2x 32 KiB)");
    srvconn_unref(dn);
    srvconn_unref(cn);
}

// ---------------------------------------------------------------------------
// srvconn.role_park_second_writer (#354)
// ---------------------------------------------------------------------------
//
// TWO concurrent blocking producers on one s2c: the second must PARK on
// the role list until the first finishes, then deliver -- with the two
// payloads call-atomic (A's bytes fully precede B's). Pre-#354 the
// second writer was refused -1 the instant it contended, which a POSIX
// server's write_full treats as EPIPE -> mount close (the #348-audit F1
// latent, live once stratumd went threaded at CF-2). Non-vacuous: on
// pre-fix code g_rp_ret_b reads -1 after the first sched().

static struct SrvConn   *g_rp_conn;
static volatile u32      g_rp_ran_a, g_rp_ran_b;
static volatile long     g_rp_ret_a, g_rp_ret_b;
static volatile bool     g_rp_exited_a, g_rp_exited_b;
static u8                g_rp_payload_a[256];
static u8                g_rp_payload_b[256];

static void rp_send_producer_a(void) {
    g_rp_ran_a++;
    g_rp_ret_a = srvconn_server_send_blocking(g_rp_conn, g_rp_payload_a,
                                              (long)sizeof g_rp_payload_a);
    g_rp_ran_a++;
    test_kthread_park_terminal(&g_rp_exited_a);
}

static void rp_send_producer_b(void) {
    g_rp_ran_b++;
    g_rp_ret_b = srvconn_server_send_blocking(g_rp_conn, g_rp_payload_b,
                                              (long)sizeof g_rp_payload_b);
    g_rp_ran_b++;
    test_kthread_park_terminal(&g_rp_exited_b);
}

void test_srvconn_role_park_second_writer(void) {
    struct SrvConn *cn = srvconn_create(0x8888u, 88, false, 0, SRVCONN_MSIZE);
    TEST_ASSERT(cn != NULL, "srvconn_create");

    g_rp_conn = cn;
    g_rp_ran_a = g_rp_ran_b = 0;
    g_rp_ret_a = g_rp_ret_b = -999;
    g_rp_exited_a = g_rp_exited_b = false;
    fill_pattern(g_rp_payload_a, sizeof g_rp_payload_a, 0x55);
    fill_pattern(g_rp_payload_b, sizeof g_rp_payload_b, 0x66);

    // Fill s2c to capacity so producer A parks mid-delivery holding the
    // writing role, and producer B then contends the ROLE itself.
    long filled = 0;
    while (filled < (long)cn->s2c.cap) {
        long want = (long)cn->s2c.cap - filled;
        if (want > (long)sizeof g_sc_chunk) want = (long)sizeof g_sc_chunk;
        for (long j = 0; j < want; j++)
            g_sc_chunk[j] = (u8)(0xAA + (u8)(filled + j));
        TEST_EXPECT_EQ(srvconn_server_send(cn, g_sc_chunk, want), want,
            "s2c prefill chunk accepted");
        filled += want;
    }
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty at test entry");

    struct Thread *pa = thread_create(kproc(), rp_send_producer_a);
    struct Thread *pb = thread_create(kproc(), rp_send_producer_b);
    TEST_ASSERT(pa != NULL && pb != NULL, "thread_create x2");
    ready(pa);
    sched();
    TEST_EXPECT_EQ(g_rp_ran_a, 1u, "producer A parked mid-send (ring full)");
    TEST_ASSERT(cn->s2c.writing == true, "A holds the writing role");

    ready(pb);
    sched();
    // THE #354 ASSERTION: B contended the held role and PARKED -- it did
    // NOT return -1. Pre-fix this reads g_rp_ret_b == -1 with ran_b == 2.
    TEST_EXPECT_EQ(g_rp_ran_b, 1u, "producer B parked on the ROLE");
    TEST_EXPECT_EQ(g_rp_ret_b, -999L,
        "producer B was NOT refused -1 (the pre-#354 behavior)");
    TEST_EXPECT_EQ(pb->state, THREAD_SLEEPING,
        "producer B is SLEEPING in the role wait");

    // Drain the whole prefill; A completes + releases the role; B then
    // acquires, delivers, and both payloads sit in order at the tail.
    u8 in[256];
    long remain = (long)cn->s2c.cap;
    long off = 0;
    while (remain > 0) {
        long want = remain > (long)sizeof in ? (long)sizeof in : remain;
        TEST_EXPECT_EQ(srvconn_client_recv(cn, in, want), want,
            "drain prefill");
        for (long j = 0; j < want; j++)
            TEST_ASSERT(in[j] == (u8)(0xAA + (u8)(off + j)),
                "prefill byte intact");
        off += want;
        remain -= want;
        sched();   // let A (then B) resume as room frees
    }
    sched();
    TEST_EXPECT_EQ(g_rp_ran_a, 2u, "producer A completed");
    TEST_EXPECT_EQ(g_rp_ran_b, 2u, "producer B completed after the role park");
    TEST_EXPECT_EQ(g_rp_ret_a, (long)sizeof g_rp_payload_a, "A whole payload");
    TEST_EXPECT_EQ(g_rp_ret_b, (long)sizeof g_rp_payload_b, "B whole payload");
    TEST_ASSERT(cn->s2c.writing == false, "the role is released");

    // Call atomicity: A's 256 bytes fully precede B's (the role was held
    // across A's whole multi-chunk delivery).
    TEST_EXPECT_EQ(srvconn_client_recv(cn, in, 256), 256L, "drain A");
    TEST_ASSERT(check_pattern(in, 256, 0x55), "A's payload contiguous");
    TEST_EXPECT_EQ(srvconn_client_recv(cn, in, 256), 256L, "drain B");
    TEST_ASSERT(check_pattern(in, 256, 0x66), "B's payload after A's");

    test_kthread_join_free(pa, &g_rp_exited_a);
    test_kthread_join_free(pb, &g_rp_exited_b);
    srvconn_unref(cn);
}

// ---------------------------------------------------------------------------
// srvconn.role_park_second_reader (#354)
// ---------------------------------------------------------------------------
//
// TWO concurrent blocking consumers on one s2c: the second parks on the
// role list (pre-#354: refused -1 -- the RW-4 R2-F1 guard's contention
// shape) and reads once the first finishes.

static volatile long g_rr_ret_a, g_rr_ret_b;
static volatile u32  g_rr_ran_a, g_rr_ran_b;
static volatile bool g_rr_exited_a, g_rr_exited_b;
static u8            g_rr_buf_a[64];
static u8            g_rr_buf_b[64];

static void rr_recv_consumer_a(void) {
    g_rr_ran_a++;
    g_rr_ret_a = srvconn_client_recv(g_rp_conn, g_rr_buf_a,
                                     (long)sizeof g_rr_buf_a);
    g_rr_ran_a++;
    test_kthread_park_terminal(&g_rr_exited_a);
}

static void rr_recv_consumer_b(void) {
    g_rr_ran_b++;
    g_rr_ret_b = srvconn_client_recv(g_rp_conn, g_rr_buf_b,
                                     (long)sizeof g_rr_buf_b);
    g_rr_ran_b++;
    test_kthread_park_terminal(&g_rr_exited_b);
}

void test_srvconn_role_park_second_reader(void) {
    struct SrvConn *cn = srvconn_create(0x9999u, 99, false, 0, SRVCONN_MSIZE);
    TEST_ASSERT(cn != NULL, "srvconn_create");

    g_rp_conn = cn;
    g_rr_ran_a = g_rr_ran_b = 0;
    g_rr_ret_a = g_rr_ret_b = -999;
    g_rr_exited_a = g_rr_exited_b = false;

    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty at test entry");

    struct Thread *ca = thread_create(kproc(), rr_recv_consumer_a);
    struct Thread *cb = thread_create(kproc(), rr_recv_consumer_b);
    TEST_ASSERT(ca != NULL && cb != NULL, "thread_create x2");
    ready(ca);
    sched();
    TEST_EXPECT_EQ(g_rr_ran_a, 1u, "consumer A parked on empty s2c");
    TEST_ASSERT(cn->s2c.reading == true, "A holds the reading role");

    ready(cb);
    sched();
    // THE #354 ASSERTION: B parked on the role instead of returning -1.
    TEST_EXPECT_EQ(g_rr_ran_b, 1u, "consumer B parked on the ROLE");
    TEST_EXPECT_EQ(g_rr_ret_b, -999L,
        "consumer B was NOT refused -1 (the pre-#354 behavior)");

    // First 64-byte delivery: A (the data-parked role holder) consumes,
    // releases the role; B acquires and parks on data.
    fill_pattern(g_sc_chunk, 64, 0x21);
    TEST_EXPECT_EQ(srvconn_server_send(cn, g_sc_chunk, 64), 64L, "send #1");
    sched();
    TEST_EXPECT_EQ(g_rr_ran_a, 2u, "consumer A completed");
    TEST_EXPECT_EQ(g_rr_ret_a, 64L, "A read the first chunk");
    TEST_ASSERT(check_pattern(g_rr_buf_a, 64, 0x21), "A's bytes intact");

    sched();   // B acquires the freed role + parks on data
    fill_pattern(g_sc_chunk, 64, 0x42);
    TEST_EXPECT_EQ(srvconn_server_send(cn, g_sc_chunk, 64), 64L, "send #2");
    sched();
    TEST_EXPECT_EQ(g_rr_ran_b, 2u, "consumer B completed after the role park");
    TEST_EXPECT_EQ(g_rr_ret_b, 64L, "B read the second chunk");
    TEST_ASSERT(check_pattern(g_rr_buf_b, 64, 0x42), "B's bytes intact");
    TEST_ASSERT(cn->s2c.reading == false, "the role is released");

    test_kthread_join_free(ca, &g_rr_exited_a);
    test_kthread_join_free(cb, &g_rr_exited_b);
    srvconn_unref(cn);
}

// ---------------------------------------------------------------------------
// srvconn.client_send_blocking_backpressure (CF-3 B)
// ---------------------------------------------------------------------------
//
// The c2s twin of srvconn.server_send_blocks_then_drain_wakes: a blocking
// CLIENT send into a FULL c2s ring parks on c2s.wrendez (never returns 0
// -- the per-user stratumd proxy's write_full would EPIPE-close its
// upstream mount on a 0); the SERVER's drain (srvconn_server_recv) wakes
// it to deliver the whole payload. Non-vacuous: pre-CF-3-B the client arm
// was the non-blocking srvconn_client_send, which returns a short/0 count
// on a full ring and never parks.

static volatile u32  g_cs_ran;
static volatile long g_cs_ret;
static volatile bool g_cs_exited;
static u8            g_cs_payload[256];

static void cs_send_producer(void) {
    g_cs_ran++;
    g_cs_ret = srvconn_client_send_blocking(g_rp_conn, g_cs_payload,
                                            (long)sizeof g_cs_payload);
    g_cs_ran++;
    test_kthread_park_terminal(&g_cs_exited);
}

void test_srvconn_client_send_blocking_backpressure(void) {
    struct SrvConn *cn = srvconn_create(0xAAAAu, 110, false, 0, SRVCONN_MSIZE);
    TEST_ASSERT(cn != NULL, "srvconn_create");

    g_rp_conn  = cn;
    g_cs_ran    = 0;
    g_cs_ret    = -999;
    g_cs_exited = false;
    fill_pattern(g_cs_payload, sizeof g_cs_payload, 0x77);

    // Fill c2s to capacity via the non-blocking client send.
    long filled = 0;
    while (filled < (long)cn->c2s.cap) {
        long want = (long)cn->c2s.cap - filled;
        if (want > (long)sizeof g_sc_chunk) want = (long)sizeof g_sc_chunk;
        for (long j = 0; j < want; j++)
            g_sc_chunk[j] = (u8)(0x11 + (u8)(filled + j));
        TEST_EXPECT_EQ(srvconn_client_send(cn, g_sc_chunk, want), want,
            "c2s prefill chunk accepted");
        filled += want;
    }
    TEST_EXPECT_EQ(cn->c2s.count, cn->c2s.cap, "c2s filled to capacity");
    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty at test entry");

    struct Thread *producer = thread_create(kproc(), cs_send_producer);
    TEST_ASSERT(producer != NULL, "thread_create(producer)");
    ready(producer);
    sched();
    TEST_EXPECT_EQ(g_cs_ran, 1u, "producer parked (c2s full)");
    TEST_EXPECT_EQ(producer->state, THREAD_SLEEPING,
        "producer is SLEEPING inside client_send_blocking");
    TEST_EXPECT_EQ(cn->c2s.wrendez.waiter, producer,
        "producer parked on the c2s WRENDEZ");
    TEST_ASSERT(cn->c2s.writing == true, "the writing role is held");
    TEST_EXPECT_EQ(g_cs_ret, -999L, "producer has NOT returned");

    // The server drains a chunk -> the drain-wake releases the producer.
    u8 in[256];
    TEST_EXPECT_EQ(srvconn_server_recv(cn, in, sizeof in), 256L,
        "server_recv drains a chunk of the prefill");
    sched();
    TEST_EXPECT_EQ(g_cs_ran, 2u, "producer resumed past the blocking send");
    TEST_EXPECT_EQ(g_cs_ret, (long)sizeof g_cs_payload,
        "client_send_blocking delivered the WHOLE payload");
    TEST_ASSERT(cn->c2s.writing == false, "the role is released");

    // Drain the rest + verify the payload landed at the stream tail.
    long off = 256;
    long remain = (long)cn->c2s.cap - 256;
    while (remain > 0) {
        long want = remain > (long)sizeof in ? (long)sizeof in : remain;
        TEST_EXPECT_EQ(srvconn_server_recv(cn, in, want), want,
            "drain remaining prefill");
        for (long j = 0; j < want; j++)
            TEST_ASSERT(in[j] == (u8)(0x11 + (u8)(off + j)),
                "prefill byte intact");
        off += want;
        remain -= want;
    }
    TEST_EXPECT_EQ(srvconn_server_recv(cn, in, 256), 256L, "drain payload");
    TEST_ASSERT(check_pattern(in, 256, 0x77),
        "the client payload landed intact at the stream tail");

    test_kthread_join_free(producer, &g_cs_exited);
    srvconn_unref(cn);
}

// ---------------------------------------------------------------------------
// srvconn.client_send_blocking_poll_edge (CF-3 B audit F1)
// ---------------------------------------------------------------------------
//
// A poll-then-read byte server parked on the conn's poll list MUST see
// the POLLIN edge from EVERY chunk a blocking client send delivers --
// not only from the send's completion. Pre-fix the blocking send woke
// cn->poll_list once at end-of-delivery, so a server polling an empty
// c2s + a client writing > ring cap was a CIRCULAR WAIT: the client
// parked on wrendez waiting for the drain, the server parked in poll()
// waiting for an edge that only fired after the delivery it was
// supposed to enable. Non-vacuous: on pre-fix code the poller below
// never wakes (g_pe_ran_p stays 1) while the producer is parked.

static struct SrvConn   *g_pe_conn;
static volatile u32      g_pe_ran_p, g_pe_ran_w;
static volatile long     g_pe_ret_w, g_pe_drained;
static volatile bool     g_pe_exited_p, g_pe_exited_w;
static u8                g_pe_payload[SRVCONN_RING_CAP + 256];

static int pe_cond_c2s_readable(void *arg) {
    struct SrvConn *cn = (struct SrvConn *)arg;
    return cn->c2s.count > 0 || cn->c2s.eof;
}

static void pe_poller(void) {
    g_pe_ran_p++;
    // The poll-then-read server shape: register the hook + sample under
    // the chan locks (srvconn_poll), then park on the hook's OWN rendez
    // until a producer's poll_waiter_list_wake fires (the 0015/0005
    // pouch poll path in miniature).
    struct Rendez      pr;
    struct poll_waiter pw;
    rendez_init(&pr);
    poll_waiter_init(&pw, &pr);
    short rv = srvconn_poll(g_pe_conn, POLLIN, &pw);
    if ((rv & POLLIN) == 0) {
        (void)sleep(&pr, pe_cond_c2s_readable, g_pe_conn);
    }
    poll_waiter_list_unregister(&pw);
    g_pe_ran_p++;                     // -> 2: the POLLIN edge arrived
    // ONE drain pass (everything buffered right now -- the full ring the
    // parked writer filled; its 256-byte tail lands only after this
    // drain frees room + the scheduler runs it, and is drained by the
    // main test thread). Each server_recv fires the drain-wake toward
    // the parked writer.
    static u8 in[1024];
    long total = 0;
    for (;;) {
        long got = srvconn_server_recv(g_pe_conn, in, sizeof in);
        if (got <= 0) break;
        total += got;
    }
    g_pe_drained = total;
    g_pe_ran_p++;                     // -> 3: drained
    test_kthread_park_terminal(&g_pe_exited_p);
}

static void pe_writer(void) {
    g_pe_ran_w++;
    g_pe_ret_w = srvconn_client_send_blocking(g_pe_conn, g_pe_payload,
                                              (long)sizeof g_pe_payload);
    g_pe_ran_w++;
    test_kthread_park_terminal(&g_pe_exited_w);
}

void test_srvconn_client_send_blocking_poll_edge(void) {
    struct SrvConn *cn = srvconn_create(0xBBBBu, 111, false, 0, SRVCONN_MSIZE);
    TEST_ASSERT(cn != NULL, "srvconn_create");

    g_pe_conn = cn;
    g_pe_ran_p = g_pe_ran_w = 0;
    g_pe_ret_w = -999;
    g_pe_drained = 0;
    g_pe_exited_p = g_pe_exited_w = false;
    fill_pattern(g_pe_payload, sizeof g_pe_payload, 0x3C);

    TEST_EXPECT_EQ(sched_runnable_count(), 0u, "run tree empty at test entry");

    struct Thread *tp = thread_create(kproc(), pe_poller);
    struct Thread *tw = thread_create(kproc(), pe_writer);
    TEST_ASSERT(tp != NULL && tw != NULL, "thread_create x2");

    // The poller registers + parks on the EMPTY c2s first.
    ready(tp);
    sched();
    TEST_EXPECT_EQ(g_pe_ran_p, 1u, "poller parked on the empty ring");
    TEST_EXPECT_EQ(tp->state, THREAD_SLEEPING, "poller is SLEEPING in poll");

    // The writer fills the ring (payload > cap) and parks mid-delivery.
    // THE F1 ASSERTION: the first accepted chunk must fire the poll
    // edge -- the poller wakes, drains, and the writer completes. On
    // pre-fix code the poller stays SLEEPING here (ran_p == 1) and the
    // writer stays parked: the circular wait.
    ready(tw);
    sched();
    TEST_ASSERT(g_pe_ran_p >= 2u,
        "the poller saw the POLLIN edge WHILE the send was in flight "
        "(pre-F1-fix: no wake until end-of-delivery -> circular wait)");

    // Let the drain + the writer's completion run out.
    for (int i = 0; i < 8; i++) sched();
    TEST_EXPECT_EQ(g_pe_ran_w, 2u, "writer completed past the park");
    TEST_EXPECT_EQ(g_pe_ret_w, (long)sizeof g_pe_payload,
        "client_send_blocking delivered the WHOLE >cap payload");
    TEST_EXPECT_EQ(g_pe_ran_p, 3u, "poller drained after the edge");
    TEST_EXPECT_EQ(g_pe_drained, (long)cn->c2s.cap,
        "the poller's pass drained the full ring the writer had filled");

    // The writer's 256-byte tail landed after the poller's pass; drain +
    // byte-verify it here (offset cap into the 0x3C ramp).
    {
        u8 tail[256];
        long got = srvconn_server_recv(cn, tail, sizeof tail);
        TEST_EXPECT_EQ(got, (long)sizeof tail, "the tail chunk drained");
        long base = (long)cn->c2s.cap;
        for (long j = 0; j < got; j++)
            TEST_ASSERT(tail[j] == (u8)(0x3C + (u8)(base + j)),
                "tail byte intact at the stream offset");
    }

    test_kthread_join_free(tp, &g_pe_exited_p);
    test_kthread_join_free(tw, &g_pe_exited_w);
    srvconn_unref(cn);
}
