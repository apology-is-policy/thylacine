// The closer pool (docs/FID-LIFECYCLE-DESIGN.md section 9;
// dec-2026-09-28-tclunk-closer): a Tclunk that a dying thread could not send
// is sent by a closer thread; a server that never answers holds only its own
// session's closer; a session that died first drops its entries; the closer
// that takes work spawns a spare, and retired closers are reaped.
//
// The pool is the boot's and outlives every test, so each test here leaves it
// as it found it: one closer, idle and asleep, nothing queued. A later test
// that counts kproc's threads or the runnable threads would otherwise see a
// closer come or go.

#include "test.h"

#include <thylacine/9p_attach.h>
#include <thylacine/9p_client.h>
#include <thylacine/9p_session.h>
#include <thylacine/9p_transport.h>
#include <thylacine/9p_transport_mq.h>
#include <thylacine/9p_wire.h>
#include <thylacine/dev.h>
#include <thylacine/dev9p.h>
#include <thylacine/errno.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

void test_p9_closer_dying_close_delivers_tclunk(void);
void test_p9_closer_stalled_session_holds_one_closer(void);
void test_p9_closer_flushed_walk_fid_clunked(void);
void test_p9_closer_failed_spawn_retried_by_hand_off(void);
void test_p9_closer_hand_off_inside_failed_spawn_retried(void);
void test_p9_closer_hand_off_inside_spawn_no_duplicate(void);
void test_p9_closer_clunk_killed_while_self_pumping(void);
void test_p9_closer_orphan_oom_on_dead_session_quiet(void);

// test_9p_client.c: the shared canned 9P2000.L replies.
int canonical_responder(void *ctx, const u8 *req, size_t req_len,
                        u8 *resp, size_t resp_cap);

static struct p9_closer_stats closer_now(void) {
    struct p9_closer_stats st;
    p9_closer_stats(&st);
    return st;
}

static bool closer_quiet(void) {
    struct p9_closer_stats st = closer_now();
    return st.threads == 1 && st.idle == 1 && st.idle_parked == 1 &&
           st.retired == 0 && st.runq == 0 && st.pending == 0;
}

// What one server saw. Written by whichever thread sends -- a closer, often
// on another CPU -- under the transport's lock, so the counts are RELEASEd.
#define SRV_FIDS  8u
struct srv_rec {
    u32 nclunk;
    u32 clunk_fid[SRV_FIDS];
    u32 nwalk;
    u32 nflush;
};

static int rec_responder(void *ctx, const u8 *req, size_t req_len,
                         u8 *resp, size_t resp_cap) {
    struct srv_rec *r = (struct srv_rec *)ctx;
    u32 size; u8 type; u16 tag;
    if (r && p9_peek_header(req, req_len, &size, &type, &tag) == 0) {
        if (type == P9_TCLUNK && req_len >= P9_HDR_LEN + 4) {
            u32 n = r->nclunk;
            if (n < SRV_FIDS)
                r->clunk_fid[n] = (u32)req[7] | (u32)req[8] << 8 |
                                  (u32)req[9] << 16 | (u32)req[10] << 24;
            __atomic_store_n(&r->nclunk, n + 1, __ATOMIC_RELEASE);
        } else if (type == P9_TWALK) {
            __atomic_store_n(&r->nwalk, r->nwalk + 1, __ATOMIC_RELEASE);
        } else if (type == P9_TFLUSH) {
            __atomic_store_n(&r->nflush, r->nflush + 1, __ATOMIC_RELEASE);
        }
    }
    return canonical_responder(ctx, req, req_len, resp, resp_cap);
}

static bool srv_clunked(struct srv_rec *r, u32 fid) {
    u32 n = __atomic_load_n(&r->nclunk, __ATOMIC_ACQUIRE);
    for (u32 i = 0; i < n && i < SRV_FIDS; i++)
        if (r->clunk_fid[i] == fid) return true;
    return false;
}

static struct p9_attached *closer_session(struct p9_transport_ops ops) {
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    return p9_attached_create(ops, /*recv_cap=*/4096, /*root_fid=*/0,
                              /*msize=*/8192, uname, sizeof(uname),
                              aname, sizeof(aname), 0, NULL);
}

static void hold_reader(struct p9_client *c, bool held) {
    spin_lock(&c->lock);
    c->reader_active = held;
    spin_unlock(&c->lock);
}

static struct p9_mq_loopback g_mq_a;
static struct p9_mq_loopback g_mq_b;
static struct srv_rec        g_rec_a;
static struct srv_rec        g_rec_b;

// =============================================================================
// A thread whose Proc is dying drops the last reference to a walked Spoor.
// dev9p_close's Tclunk cannot be sent from it, so it goes to a closer, which
// sends it. Before the closer the server kept the fid until the session
// ended: gopls's kill of a `go` child still in its spawn thunk did this three
// times a boot.
// =============================================================================

static struct Spoor      *g_t1_spoor;
static struct test_dying  g_t1_dying;

static void t1_drop(void *arg) {
    (void)arg;
    spoor_clunk(g_t1_spoor);
}

void test_p9_closer_dying_close_delivers_tclunk(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    g_rec_a = (struct srv_rec){0};
    TEST_EXPECT_EQ(p9_mq_loopback_init(&g_mq_a, rec_responder, &g_rec_a), 0, "mq");
    struct p9_attached *a = closer_session(p9_mq_loopback_ops_for(&g_mq_a));
    TEST_ASSERT(a != NULL, "session");
    struct Spoor *root = p9_attached_root_spoor(a);
    TEST_ASSERT(root != NULL, "root Spoor");
    // The attach syscall's stamp: the root, and every Spoor walked from it,
    // holds a session reference.
    struct dev9p_priv *rp = (struct dev9p_priv *)root->aux;
    rp->attached_owner = a;
    p9_attached_ref(a);

    struct Spoor *walked = spoor_clone(root);
    TEST_ASSERT(walked != NULL, "spoor_clone");
    const char *name = "victim";
    struct Walkqid *w = dev9p.walk(root, walked, &name, 1);
    TEST_ASSERT(w != NULL, "walk");
    walkqid_free(w);
    u32 fid = ((struct dev9p_priv *)walked->aux)->fid;
    TEST_ASSERT(p9_session_fid_bound(&a->client->session, fid), "the walk bound its fid");

    g_t1_spoor = walked;
    TEST_ASSERT(test_dying_start(&g_t1_dying, t1_drop, NULL, /*dead_now=*/true),
                "a dying thread");
    TEST_YIELD_UNTIL(test_dying_done(&g_t1_dying));
    test_dying_reap(&g_t1_dying);

    TEST_YIELD_UNTIL(srv_clunked(&g_rec_a, fid));
    TEST_YIELD_UNTIL(closer_quiet());
    struct p9_closer_stats st = closer_now();
    TEST_EXPECT_EQ(st.sent, base.sent + 1, "a closer sent the Tclunk");
    TEST_EXPECT_EQ(st.live_refusals, base.live_refusals, "no refusal line");
    TEST_EXPECT_EQ(st.spawned - base.spawned, st.reaped - base.reaped,
                   "every spare was reaped");
    TEST_ASSERT(!p9_session_fid_bound(&a->client->session, fid), "the fid is clunked");

    spoor_clunk(root);
    p9_attached_unref(a);                        // the construction reference, last
    p9_mq_loopback_destroy(&g_mq_a);
}

// =============================================================================
// A server that stops answering holds only its own session's closer. The
// closer that took its work waits in its recv; the spare that closer spawned
// sends another session's Tclunk meanwhile. When the stalled server dies, its
// session's entry is dropped quietly -- its fids died with it -- and the pool
// shrinks back to one idle closer, every spare reaped.
// =============================================================================

// A server that stops answering: while `stalled`, every send meets a full
// c2s ring, and a recv waits until the test lets it go, then reports EOF --
// the server died. Otherwise it is the mq loopback.
struct stall_tp {
    struct p9_mq_loopback   mq;
    struct p9_transport_ops inner;
    bool                    stalled;
    bool                    released;
    bool                    blocked;             // a recv is waiting
    bool                    inited;
    struct Rendez           r;
};

static struct stall_tp g_stall;

static int stall_send(void *ctx, const u8 *buf, size_t len) {
    struct stall_tp *st = (struct stall_tp *)ctx;
    if (__atomic_load_n(&st->stalled, __ATOMIC_ACQUIRE)) return P9_TRANSPORT_EAGAIN;
    return st->inner.send(st->inner.ctx, buf, len);
}

static int stall_released(void *arg) {
    struct stall_tp *st = (struct stall_tp *)arg;
    return __atomic_load_n(&st->released, __ATOMIC_ACQUIRE) ? 1 : 0;
}

static int stall_recv(void *ctx, u8 *buf, size_t cap) {
    struct stall_tp *st = (struct stall_tp *)ctx;
    if (__atomic_load_n(&st->stalled, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&st->blocked, true, __ATOMIC_RELEASE);
        (void)sleep(&st->r, stall_released, st);
        return 0;
    }
    return st->inner.recv(st->inner.ctx, buf, cap);
}

static int stall_close(void *ctx) {
    struct stall_tp *st = (struct stall_tp *)ctx;
    return st->inner.close(st->inner.ctx);
}

static void stall_set_recv_deadline(void *ctx, u64 deadline_ns) {
    struct stall_tp *st = (struct stall_tp *)ctx;
    st->inner.set_recv_deadline(st->inner.ctx, deadline_ns);
}

static bool stall_recv_timed_out(void *ctx) {
    struct stall_tp *st = (struct stall_tp *)ctx;
    return st->inner.recv_timed_out(st->inner.ctx);
}

static struct p9_transport_ops stall_init(struct stall_tp *st, struct srv_rec *rec) {
    st->stalled  = false;
    st->released = false;
    st->blocked  = false;
    rendez_init(&st->r);
    st->inited   = true;
    (void)p9_mq_loopback_init(&st->mq, rec_responder, rec);
    st->inner = p9_mq_loopback_ops_for(&st->mq);
    struct p9_transport_ops ops = st->inner;
    ops.send              = stall_send;
    ops.recv              = stall_recv;
    ops.close             = stall_close;
    ops.set_recv_deadline = stall_set_recv_deadline;
    ops.recv_timed_out    = stall_recv_timed_out;
    ops.ctx               = st;
    return ops;
}

static void stall_release(struct stall_tp *st) {
    __atomic_store_n(&st->released, true, __ATOMIC_RELEASE);
    (void)wakeup(&st->r);
}

static struct p9_attached *g_st_a;
static struct p9_attached *g_st_b;

// PLAIN: the pool as it runs. FAIL: the spare that A's closer spawns fails to
// start, so no closer is idle when B's fid is handed over, and the hand-off
// spawns the spare. HOLD_FAIL and HOLD_OK: B's fid is handed over while that
// spawn is still running, and fails or succeeds.
enum stall_mode { STALL_PLAIN, STALL_FAIL, STALL_HOLD_FAIL, STALL_HOLD_OK };

static void stalled_body(enum stall_mode mode) {
    bool fail = mode == STALL_FAIL || mode == STALL_HOLD_FAIL;
    bool hold = mode == STALL_HOLD_FAIL || mode == STALL_HOLD_OK;
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    g_rec_a = (struct srv_rec){0};
    g_rec_b = (struct srv_rec){0};
    struct p9_attached *a = g_st_a = closer_session(stall_init(&g_stall, &g_rec_a));
    TEST_ASSERT(a != NULL, "session A");
    TEST_EXPECT_EQ(p9_mq_loopback_init(&g_mq_b, rec_responder, &g_rec_b), 0, "mq B");
    struct p9_attached *b = g_st_b = closer_session(p9_mq_loopback_ops_for(&g_mq_b));
    TEST_ASSERT(b != NULL, "session B");
    TEST_EXPECT_EQ(p9_client_walk_one(a->client, 0, 10, (const u8 *)"a", 1, NULL), 0,
                   "A binds fid 10");
    TEST_EXPECT_EQ(p9_client_walk_one(b->client, 0, 20, (const u8 *)"b", 1, NULL), 0,
                   "B binds fid 20");

    __atomic_store_n(&g_stall.stalled, true, __ATOMIC_RELEASE);
    if (fail) p9_closer_fail_spawns_for_test(1);
    if (hold) p9_closer_hold_spawn_for_test(true);
    TEST_EXPECT_EQ(p9_attached_defer_clunk(a, 10), 0, "A's fid to the pool");
    if (hold) {
        // A's closer is inside its spare's spawn. B's hand-off finds no closer
        // idle and a spare starting, so it leaves the spare to that spawn.
        TEST_YIELD_UNTIL(p9_closer_spawn_held_for_test());
        TEST_EXPECT_EQ(p9_attached_defer_clunk(b, 20), 0, "B's fid to the pool");
        struct p9_closer_stats h = closer_now();
        TEST_EXPECT_EQ((u64)h.runq, (u64)1, "B waits for a closer");
        TEST_EXPECT_EQ(h.spawned, base.spawned + (fail ? 0u : 1u),
                       "B's hand-off spawned nothing");
        p9_closer_hold_spawn_for_test(false);
        TEST_YIELD_UNTIL(__atomic_load_n(&g_stall.blocked, __ATOMIC_ACQUIRE));
    } else {
        TEST_YIELD_UNTIL(__atomic_load_n(&g_stall.blocked, __ATOMIC_ACQUIRE));
        if (fail) {
            // A's closer tried its spare before it began to send.
            struct p9_closer_stats f = closer_now();
            TEST_EXPECT_EQ(f.spawn_failed, base.spawn_failed + 1, "the spare failed to start");
            TEST_EXPECT_EQ((u64)f.threads, (u64)1, "A's closer is the only one");
            TEST_EXPECT_EQ((u64)f.idle, (u64)0, "and it is busy");
        }
        TEST_EXPECT_EQ(p9_attached_defer_clunk(b, 20), 0, "B's fid to the pool");
    }

    TEST_YIELD_UNTIL(srv_clunked(&g_rec_b, 20));
    TEST_YIELD_UNTIL(closer_now().sent == base.sent + 1);
    struct p9_closer_stats st = closer_now();
    TEST_EXPECT_EQ(st.pending, base.pending + 1, "A's entry still waits on its server");
    TEST_ASSERT(!srv_clunked(&g_rec_a, 10), "A's server saw no Tclunk");
    TEST_ASSERT(!a->client->dead, "A's session is live, only silent");

    stall_release(&g_stall);                     // A's server dies
    TEST_YIELD_UNTIL(closer_now().dropped == base.dropped + 1);
    TEST_YIELD_UNTIL(closer_quiet());
    st = closer_now();
    TEST_EXPECT_EQ(st.sent, base.sent + 1, "only B's Tclunk was sent");
    TEST_EXPECT_EQ(st.refused, base.refused, "nothing refused");
    TEST_EXPECT_EQ(st.live_refusals, base.live_refusals,
                   "no refusal line: A's fids died with its session");
    TEST_EXPECT_EQ(st.spawned - base.spawned, (u64)2,
                   "a spare for each closer that took work");
    TEST_EXPECT_EQ(st.reaped - base.reaped, (u64)2, "both spares' worth reaped");
    TEST_EXPECT_EQ(st.spawn_failed, base.spawn_failed + (fail ? 1u : 0u),
                   "no other spawn failed");
    TEST_ASSERT(a->client->dead, "A's session died with its server");
}

static void stalled_run(enum stall_mode mode) {
    g_st_a = NULL;
    g_st_b = NULL;
    stalled_body(mode);
    // The body may have stopped at a failed assertion with a spawn held, or a
    // closer still waiting in A's recv. Let both go whatever happened, or that
    // closer waits for the rest of the boot. No assertion here: a second
    // failure would overwrite the first one's message.
    p9_closer_hold_spawn_for_test(false);
    (void)p9_closer_fail_spawns_for_test(0);
    if (g_stall.inited) stall_release(&g_stall);
    u64 deadline = timer_now_ns() + TEST_YIELD_BUDGET_NS;
    while (!closer_quiet() && timer_now_ns() < deadline) sched();
    if (g_st_b) p9_attached_unref(g_st_b);
    if (g_st_a) p9_attached_unref(g_st_a);
    p9_mq_loopback_destroy(&g_mq_b);
    p9_mq_loopback_destroy(&g_stall.mq);
}

void test_p9_closer_stalled_session_holds_one_closer(void) {
    stalled_run(STALL_PLAIN);
}

// The spare that a busy closer spawns can fail to start (its stack is an
// allocation). The closer then serves a server that never answers, and no
// closer is idle, so the next hand-off spawns the spare itself. Before, every
// other session's Tclunks waited on that one server.
void test_p9_closer_failed_spawn_retried_by_hand_off(void) {
    stalled_run(STALL_FAIL);
}

// A hand-off made while that spawn is failing sees a spare starting and
// spawns none. The failing spawn tries again for it; before, it gave up, and
// the session waited behind a server that never answers.
void test_p9_closer_hand_off_inside_failed_spawn_retried(void) {
    stalled_run(STALL_HOLD_FAIL);
}

// A spawned closer counts as starting until its first loop top, not until
// its creation: a hand-off in between spawns no second spare.
void test_p9_closer_hand_off_inside_spawn_no_duplicate(void) {
    stalled_run(STALL_HOLD_OK);
}

// =============================================================================
// A sender with no reader to wait behind reads the replies itself. Killed in
// that read, it comes back with nothing read -- which is not a dead server --
// so the session stays live and its Tclunk is taken back whole, the fid bound
// again for the closer.
// =============================================================================

static struct test_dying   g_sp_dying;
static struct p9_attached *g_sp_a;
static int                 g_sp_rc;

static void sp_clunk(void *arg) {
    (void)arg;
    g_sp_rc = p9_client_clunk_async(g_sp_a->client, 10);
}

static void self_pump_body(void) {
    g_rec_a = (struct srv_rec){0};
    struct p9_attached *a = g_sp_a = closer_session(stall_init(&g_stall, &g_rec_a));
    TEST_ASSERT(a != NULL, "session");
    struct p9_client *c = a->client;
    TEST_EXPECT_EQ(p9_client_walk_one(c, 0, 10, (const u8 *)"a", 1, NULL), 0, "binds fid 10");

    __atomic_store_n(&g_stall.stalled, true, __ATOMIC_RELEASE);
    g_sp_rc = 0x7fffffff;
    TEST_ASSERT(test_dying_start(&g_sp_dying, sp_clunk, NULL, /*dead_now=*/false),
                "sender");
    TEST_YIELD_UNTIL(test_dying_parked(&g_sp_dying) &&
                     __atomic_load_n(&g_stall.blocked, __ATOMIC_ACQUIRE));
    TEST_ASSERT(!p9_session_fid_bound(&c->session, 10), "the build unbound the fid");
    test_dying_kill(&g_sp_dying);
    TEST_YIELD_UNTIL(test_dying_done(&g_sp_dying));
    test_dying_reap(&g_sp_dying);
    TEST_EXPECT_EQ((u64)(s64)g_sp_rc, (u64)(s64)-P9_E_AGAIN, "taken back: -P9_E_AGAIN");
    TEST_ASSERT(!c->dead, "a killed reader is not a dead server");
    TEST_ASSERT(!c->reader_active, "the reader role was let go");
    TEST_ASSERT(p9_session_fid_bound(&c->session, 10), "the fid is bound again");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&c->session), (u64)0, "the tag is free");
    TEST_EXPECT_EQ((u64)p9_session_n_reserved_slots(&c->session), (u64)0, "no slot held");

    __atomic_store_n(&g_stall.stalled, false, __ATOMIC_RELEASE);
    TEST_EXPECT_EQ(p9_client_clunk_async(c, 10), 0, "the closer's clunk");
    TEST_ASSERT(srv_clunked(&g_rec_a, 10), "reaches the server");
}

void test_p9_closer_clunk_killed_while_self_pumping(void) {
    g_sp_a = NULL;
    g_sp_dying.t = NULL;
    self_pump_body();
    // After a failed assertion the sender may still wait in A's recv: let it
    // go and reap it before its session goes. No assertion here (see above).
    if (g_stall.inited) stall_release(&g_stall);
    if (g_sp_dying.t) {
        test_dying_kill(&g_sp_dying);
        u64 deadline = timer_now_ns() + TEST_YIELD_BUDGET_NS;
        while (!__atomic_load_n(&g_sp_dying.exited, __ATOMIC_ACQUIRE) &&
               timer_now_ns() < deadline)
            sched();
        if (!__atomic_load_n(&g_sp_dying.exited, __ATOMIC_ACQUIRE)) return;   // leak, not UAF
        test_dying_reap(&g_sp_dying);
    }
    if (g_sp_a) p9_attached_unref(g_sp_a);
    p9_mq_loopback_destroy(&g_stall.mq);
}

// =============================================================================
// flush(5), end to end: a walk whose owner dies is flushed, its late Rwalk
// binds the new fid, and the session's orphan sink hands that fid to a closer,
// which clunks it. Before, the fid stayed on the server until the session
// ended.
// =============================================================================

static struct test_dying   g_fw_dying;
static struct p9_attached *g_fw_a;
static int                 g_fw_rc;

static void fw_walk(void *arg) {
    (void)arg;
    g_fw_rc = p9_client_walk_one(g_fw_a->client, 0, 30, (const u8 *)"w", 1, NULL);
}

void test_p9_closer_flushed_walk_fid_clunked(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    g_rec_a = (struct srv_rec){0};
    TEST_EXPECT_EQ(p9_mq_loopback_init(&g_mq_a, rec_responder, &g_rec_a), 0, "mq");
    struct p9_attached *a = closer_session(p9_mq_loopback_ops_for(&g_mq_a));
    TEST_ASSERT(a != NULL, "session");
    struct Spoor *root = p9_attached_root_spoor(a);   // installs the orphan sink
    TEST_ASSERT(root != NULL, "root Spoor");
    g_fw_a = a;

    // The mq's recv never blocks, so the test holds the reader role, as a
    // peer blocked in recv would, and the walker parks behind it.
    hold_reader(a->client, true);
    g_fw_rc = 0x7fffffff;
    TEST_ASSERT(test_dying_start(&g_fw_dying, fw_walk, NULL, /*dead_now=*/false),
                "walker");
    TEST_YIELD_UNTIL(test_dying_parked(&g_fw_dying) &&
                     __atomic_load_n(&g_rec_a.nwalk, __ATOMIC_ACQUIRE) == 1);
    test_dying_kill(&g_fw_dying);
    TEST_YIELD_UNTIL(test_dying_done(&g_fw_dying));
    test_dying_reap(&g_fw_dying);
    hold_reader(a->client, false);
    TEST_EXPECT_EQ((u64)(s64)g_fw_rc, (u64)(s64)-P9_E_IO, "the dead owner's walk failed");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&g_rec_a.nflush, __ATOMIC_ACQUIRE), (u64)1,
                   "and was flushed");

    // A survivor's reader drains the late Rwalk, then the Rflush.
    TEST_EXPECT_EQ(p9_client_reader_pump_once(a->client), 1, "the late Rwalk");
    TEST_EXPECT_EQ(p9_client_reader_pump_once(a->client), 1, "the Rflush");
    TEST_YIELD_UNTIL(srv_clunked(&g_rec_a, 30));
    TEST_YIELD_UNTIL(closer_quiet());
    struct p9_closer_stats st = closer_now();
    TEST_EXPECT_EQ(st.sent, base.sent + 1, "a closer clunked the orphan");
    TEST_EXPECT_EQ(st.live_refusals, base.live_refusals, "no refusal line");
    TEST_EXPECT_EQ(a->client->orphan_handed, (u64)1, "the fid went to the sink");
    TEST_ASSERT(!p9_session_fid_bound(&a->client->session, 30), "fid 30 is clunked");

    spoor_clunk(root);
    p9_attached_unref(a);
    p9_mq_loopback_destroy(&g_mq_a);
}

// =============================================================================
// The orphan sink cannot allocate its node, so the fid stays bound. On a live
// session that is a leak, reported like the hand-off's; on a session a peer
// already saw die, the fid died with it, and nothing is reported -- as the
// sink's twin without the failure drops the entry.
// =============================================================================

void test_p9_closer_orphan_oom_on_dead_session_quiet(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    g_rec_a = (struct srv_rec){0};
    TEST_EXPECT_EQ(p9_mq_loopback_init(&g_mq_a, rec_responder, &g_rec_a), 0, "mq");
    struct p9_attached *a = closer_session(p9_mq_loopback_ops_for(&g_mq_a));
    TEST_ASSERT(a != NULL, "session");
    struct Spoor *root = p9_attached_root_spoor(a);   // installs the orphan sink
    TEST_ASSERT(root != NULL, "root Spoor");
    struct p9_client *c = a->client;
    TEST_ASSERT(c->orphan_sink != NULL, "the sink is installed");
    TEST_EXPECT_EQ(p9_client_walk_one(c, 0, 40, (const u8 *)"o", 1, NULL), 0,
                   "binds fid 40");

    p9_client_mark_devgone(c);
    (void)p9_closer_fail_nodes_for_test(1);
    spin_lock(&c->lock);
    int rc = c->orphan_sink(c->orphan_sink_arg, 40);
    spin_unlock(&c->lock);
    TEST_EXPECT_EQ(p9_closer_fail_nodes_for_test(0), 0u, "the sink's node failed");
    TEST_EXPECT_EQ((u64)(s64)rc, (u64)(s64)-1, "the fid was not queued");
    TEST_ASSERT(p9_session_fid_bound(&c->session, 40), "it stays with the session");
    struct p9_closer_stats st = closer_now();
    TEST_EXPECT_EQ(st.live_refusals, base.live_refusals,
                   "no refusal line: the fid died with its session");
    TEST_EXPECT_EQ(st.pending, base.pending, "nothing queued");

    spoor_clunk(root);
    p9_attached_unref(a);
    p9_mq_loopback_destroy(&g_mq_a);
}
