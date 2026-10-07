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
#include <thylacine/handle.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

void test_p9_closer_dying_close_delivers_tclunk(void);
void test_p9_closer_exit_close_hands_off_tclunk(void);
void test_p9_closer_dying_close_hands_off_staged_run(void);
void test_p9_closer_forced_exit_close_hands_off_flush(void);
void test_p9_closer_kthread_close_hands_off_staged_run(void);
void test_p9_closer_close_job_retries_a_refused_write(void);
void test_p9_closer_first_kill_forces_exits_close(void);
extern void proc_close_handles_at_exit_for_test(struct Proc *p);
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
    u32 nmsg;        // every request, in arrival order
    u32 nwrite;
    u64 write_off;   // the last Twrite's
    u32 write_len;
    u32 write_sum;   // its payload's byte sum
    u32 write_at;    // nmsg at the last Twrite
    u32 clunk_at;    // nmsg at the last Tclunk
    u32 fail_writes; // answer this many Twrites with Rlerror(EIO)
    u32 nwrite_refused;
};

static int rec_responder(void *ctx, const u8 *req, size_t req_len,
                         u8 *resp, size_t resp_cap) {
    struct srv_rec *r = (struct srv_rec *)ctx;
    u32 size; u8 type; u16 tag;
    if (r && p9_peek_header(req, req_len, &size, &type, &tag) == 0) {
        r->nmsg++;
        if (type == P9_TWRITE && req_len >= P9_HDR_LEN + 16 && r->fail_writes) {
            r->fail_writes--;
            __atomic_store_n(&r->nwrite_refused, r->nwrite_refused + 1,
                             __ATOMIC_RELEASE);
            if (resp_cap < 11) return -1;
            resp[0] = 11; resp[1] = resp[2] = resp[3] = 0;
            resp[4] = P9_RLERROR;
            resp[5] = (u8)tag; resp[6] = (u8)(tag >> 8);
            resp[7] = (u8)T_E_IO; resp[8] = resp[9] = resp[10] = 0;
            return 11;
        } else if (type == P9_TWRITE && req_len >= P9_HDR_LEN + 16) {
            const u8 *b = req + P9_HDR_LEN + 4;           // past the fid
            u64 off = 0;
            for (u32 i = 0; i < 8; i++) off |= (u64)b[i] << (8 * i);
            u32 cnt = (u32)b[8] | (u32)b[9] << 8 | (u32)b[10] << 16 |
                      (u32)b[11] << 24;
            u32 sum = 0;
            for (u32 i = 0; i < cnt && P9_HDR_LEN + 16 + i < req_len; i++)
                sum += b[12 + i];
            r->write_off = off;
            r->write_len = cnt;
            r->write_sum = sum;
            r->write_at  = r->nmsg;
            __atomic_store_n(&r->nwrite, r->nwrite + 1, __ATOMIC_RELEASE);
        } else if (type == P9_TCLUNK && req_len >= P9_HDR_LEN + 4) {
            r->clunk_at = r->nmsg;
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
// An exit close (exit_close_active), which no death reaches, never waits for
// its server to clunk a fid (dec-2026-10-07-exit-close, part A). Its Tclunk
// meets a full request ring with the reader held, where a clunk that may wait
// would park for progress; this one hands the fid to a closer and returns,
// and the closer sends it.
// =============================================================================

static struct test_dying g_ec_thread;

static void ec_drop(void *arg) {
    (void)arg;
    struct Thread *self = current_thread();
    self->exit_close_active = true;
    spoor_clunk(g_t1_spoor);
    self->exit_close_active = false;
}

void test_p9_closer_exit_close_hands_off_tclunk(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    g_rec_a = (struct srv_rec){0};
    TEST_EXPECT_EQ(p9_mq_loopback_init(&g_mq_a, rec_responder, &g_rec_a), 0, "mq");
    struct p9_attached *a = closer_session(p9_mq_loopback_ops_for(&g_mq_a));
    TEST_ASSERT(a != NULL, "session");
    struct Spoor *root = p9_attached_root_spoor(a);
    TEST_ASSERT(root != NULL, "root Spoor");
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

    // No death reaches the closing thread, so a clunk that waited (the RED
    // case) is released by progress: the reader freed, then a walk whose
    // reader departure signals it. All before any assert below.
    g_t1_spoor = walked;
    hold_reader(a->client, true);
    g_mq_a.eagain_budget = 1;                    // the Tclunk meets a full ring
    bool started = test_dying_start(&g_ec_thread, ec_drop, NULL, /*dead_now=*/false);
    TEST_YIELD_UNTIL_SOFT(!started || test_dying_done(&g_ec_thread));
    bool returned = started && test_dying_done(&g_ec_thread);
    hold_reader(a->client, false);
    if (started && !returned) {
        (void)p9_client_walk_one(a->client, 0, p9_client_alloc_fid(a->client),
                                 (const u8 *)"u", 1, NULL);
        TEST_YIELD_UNTIL_SOFT(test_dying_done(&g_ec_thread));
    }
    if (started) test_dying_reap(&g_ec_thread);
    u32 budget = g_mq_a.eagain_budget;
    TEST_YIELD_UNTIL_SOFT(srv_clunked(&g_rec_a, fid));
    TEST_YIELD_UNTIL_SOFT(closer_quiet());
    struct p9_closer_stats st = closer_now();
    bool clunked = srv_clunked(&g_rec_a, fid);
    bool unbound = !p9_session_fid_bound(&a->client->session, fid);

    spoor_clunk(root);
    p9_attached_unref(a);                        // the construction reference, last
    p9_mq_loopback_destroy(&g_mq_a);

    TEST_ASSERT(started, "a closing thread");
    TEST_ASSERT(returned, "the exit close returned without waiting");
    TEST_EXPECT_EQ((u64)budget, 0ull, "its Tclunk met the full ring");
    TEST_ASSERT(clunked, "the server saw the Tclunk");
    TEST_EXPECT_EQ(st.sent, base.sent + 1, "a closer sent it");
    TEST_EXPECT_EQ(st.live_refusals, base.live_refusals, "no refusal line");
    TEST_EXPECT_EQ(st.spawned - base.spawned, st.reaped - base.reaped,
                   "every spare was reaped");
    TEST_ASSERT(unbound, "the fid is clunked");
}

// =============================================================================
// The rest of a last close that may not wait for its server -- the staged
// write-behind run and the fid's clunk -- goes to a closer, which writes the run
// and then clunks (dec-2026-10-07-exit-close, part C). The fixture: a closer
// session whose client stages writes, and a file created under its root with
// WBC_LEN patterned bytes staged at offset 0, nothing on the wire yet.
// =============================================================================

#define WBC_LEN 256u

struct wbc {
    struct p9_attached *a;
    struct Spoor       *root;
    struct Spoor       *f;
    u32                 fid;
    u64                 budget0;
};

static u8 wbc_pat(u32 i) { return (u8)(i * 7u + 3u); }

static u32 wbc_sum(void) {
    u32 s = 0;
    for (u32 i = 0; i < WBC_LEN; i++) s += wbc_pat(i);
    return s;
}

static bool wbc_open(struct wbc *w) {
    g_rec_a = (struct srv_rec){0};
    if (p9_mq_loopback_init(&g_mq_a, rec_responder, &g_rec_a) != 0) return false;
    w->a = closer_session(p9_mq_loopback_ops_for(&g_mq_a));
    if (!w->a) { p9_mq_loopback_destroy(&g_mq_a); return false; }
    w->root = p9_attached_root_spoor(w->a);
    ((struct dev9p_priv *)w->root->aux)->attached_owner = w->a;
    p9_attached_ref(w->a);
    w->a->client->loose = true;
    __atomic_store_n(&w->a->client->cacheable, true, __ATOMIC_RELAXED);
    w->f = NULL;
    struct Spoor *nc = spoor_clone(w->root);
    if (!nc) return false;
    struct Walkqid *wq = dev9p.walk(w->root, nc, NULL, 0);
    if (!wq) { spoor_clunk(nc); return false; }
    walkqid_free(wq);
    w->f = dev9p.create(nc, "wbfile", 1 /*OWRITE*/, 0644u, 1000u);
    if (!w->f) { spoor_clunk(nc); return false; }
    w->budget0 = dev9p_wb_budget_used();
    u8 chunk[WBC_LEN];
    for (u32 i = 0; i < WBC_LEN; i++) chunk[i] = wbc_pat(i);
    if (dev9p.write(w->f, chunk, WBC_LEN, 0) != (long)WBC_LEN) return false;
    w->fid = ((struct dev9p_priv *)w->f->aux)->fid;
    return __atomic_load_n(&g_rec_a.nwrite, __ATOMIC_ACQUIRE) == 0;   // staged
}

static void wbc_end(struct wbc *w) {
    if (w->root) spoor_clunk(w->root);
    if (w->a) p9_attached_unref(w->a);           // the construction reference, last
    p9_mq_loopback_destroy(&g_mq_a);
}

// The server answers again: the ring takes frames, the reader is free, and a
// walk's reader departure wakes whoever parked for progress meanwhile.
static void wbc_unstall(struct wbc *w) {
    g_mq_a.eagain_budget = 0;
    hold_reader(w->a->client, false);
    (void)p9_client_walk_one(w->a->client, 0, p9_client_alloc_fid(w->a->client),
                             (const u8 *)"u", 1, NULL);
}

static struct test_dying g_wbc_thread;
static int               g_wbc_crc;

static void wbc_drop(void *arg) {
    (void)arg;
    g_wbc_crc = spoor_clunk_rc(g_t1_spoor);
}

static void wbc_exit_drop(void *arg) {
    (void)arg;
    struct Thread *self = current_thread();
    self->exit_close_active = true;
    g_wbc_crc = spoor_clunk_rc(g_t1_spoor);
    self->exit_close_active = false;
}

// A killed thread's own last close cannot send, so it never flushes: it hands
// the staged run to a closer, which writes it. Before part C the close's flush
// failed at once and the run was freed with the priv.
void test_p9_closer_dying_close_hands_off_staged_run(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    struct wbc w = {0};
    bool opened = wbc_open(&w);
    bool started = false;
    if (opened) {
        g_t1_spoor = w.f;
        g_wbc_crc  = 1;
        started = test_dying_start(&g_wbc_thread, wbc_drop, NULL, /*dead_now=*/true);
        if (started) {
            TEST_YIELD_UNTIL(test_dying_done(&g_wbc_thread));
            test_dying_reap(&g_wbc_thread);
        }
        TEST_YIELD_UNTIL_SOFT(srv_clunked(&g_rec_a, w.fid));
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
    }
    struct p9_closer_stats st = closer_now();
    struct srv_rec rec = g_rec_a;
    u64 budget = dev9p_wb_budget_used();
    wbc_end(&w);

    TEST_ASSERT(opened, "a staged file on a closer session");
    TEST_ASSERT(started, "a dying thread");
    TEST_EXPECT_EQ((u64)(s64)g_wbc_crc, 0ull, "the close reports nothing lost");
    TEST_EXPECT_EQ((u64)rec.nwrite, 1ull, "the closer wrote the kept run");
    TEST_EXPECT_EQ(rec.write_off, 0ull, "at offset 0");
    TEST_EXPECT_EQ((u64)rec.write_len, (u64)WBC_LEN, "all of it");
    TEST_EXPECT_EQ((u64)rec.write_sum, (u64)wbc_sum(), "the bytes write() took");
    TEST_ASSERT(rec.write_at < rec.clunk_at, "then the Tclunk");
    TEST_EXPECT_EQ(st.jobs, base.jobs + 1, "one close job ran");
    TEST_EXPECT_EQ(st.job_errors, base.job_errors, "and it lost nothing");
    TEST_EXPECT_EQ(st.dropped, base.dropped, "no entry was dropped");
    TEST_EXPECT_EQ(st.sent, base.sent + 1, "a closer sent the Tclunk");
    TEST_EXPECT_EQ(budget, w.budget0, "the run's budget charge came back");
}

// A plain exit close waits for its server (I-38: the parent's wait returns after
// the flush). A kill that finds the Proc terminating forces it (part B): the
// close returns without the server, and the closer writes the run once the
// server answers (part C). Every send meets a full ring, so before the kill
// nothing reaches the server and the close sleeps; the only way the bytes land
// is the closer's write.
void test_p9_closer_forced_exit_close_hands_off_flush(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    struct wbc w = {0};
    bool opened = wbc_open(&w);
    bool started = false, waited = false, returned = false;
    u32  before = 0;
    if (opened) {
        g_t1_spoor = w.f;
        g_wbc_crc  = 1;
        hold_reader(w.a->client, true);
        g_mq_a.eagain_budget = 0xffffffffu;
        started = test_dying_start(&g_wbc_thread, wbc_exit_drop, NULL,
                                   /*dead_now=*/true);
        if (started) {
            TEST_YIELD_UNTIL_SOFT(test_dying_parked(&g_wbc_thread) ||
                                  test_dying_done(&g_wbc_thread));
            waited = !test_dying_done(&g_wbc_thread);
            // The bare call is sound here: the Proc's one thread sleeps in the
            // close, and nothing else kills or reaps it.
            proc_group_kill(g_wbc_thread.proc);
            TEST_YIELD_UNTIL_SOFT(test_dying_done(&g_wbc_thread));
            returned = test_dying_done(&g_wbc_thread);
        }
        before = __atomic_load_n(&g_rec_a.nwrite, __ATOMIC_ACQUIRE);
        wbc_unstall(&w);
        if (started) {
            TEST_YIELD_UNTIL_SOFT(test_dying_done(&g_wbc_thread));
            if (test_dying_done(&g_wbc_thread)) test_dying_reap(&g_wbc_thread);
        }
        TEST_YIELD_UNTIL_SOFT(srv_clunked(&g_rec_a, w.fid));
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
    }
    struct p9_closer_stats st = closer_now();
    struct srv_rec rec = g_rec_a;
    u64 budget = dev9p_wb_budget_used();
    wbc_end(&w);

    TEST_ASSERT(opened, "a staged file on a closer session");
    TEST_ASSERT(started, "a closing thread");
    TEST_ASSERT(waited, "a plain exit close waits for its server (control)");
    TEST_ASSERT(returned, "the forcing kill ended the close without the server");
    TEST_EXPECT_EQ((u64)before, 0ull, "nothing reached the server before then");
    TEST_EXPECT_EQ((u64)(s64)g_wbc_crc, 0ull, "the close reports nothing lost");
    TEST_EXPECT_EQ((u64)rec.nwrite, 1ull, "the closer wrote the run");
    TEST_EXPECT_EQ(rec.write_off, 0ull, "at offset 0");
    TEST_EXPECT_EQ((u64)rec.write_len, (u64)WBC_LEN, "all of it");
    TEST_EXPECT_EQ((u64)rec.write_sum, (u64)wbc_sum(), "the bytes write() took");
    TEST_ASSERT(rec.write_at < rec.clunk_at, "then the Tclunk");
    TEST_EXPECT_EQ(st.jobs, base.jobs + 1, "one close job ran");
    TEST_EXPECT_EQ(st.job_errors, base.job_errors, "and it lost nothing");
    TEST_EXPECT_EQ(st.dropped, base.dropped, "no entry was dropped");
    TEST_EXPECT_EQ(budget, w.budget0, "the run's budget charge came back");
}

// A kernel thread something joins without bound (closes_never_wait: the Loom
// SQPOLL kthread) never waits for its server in a last close. Every send meets
// a full ring; the close returns at once and the closer writes the run later.
static struct Spoor  *g_kc_spoor;
static volatile int   g_kc_crc;
static volatile bool  g_kc_done;
static volatile bool  g_kc_exited;

static void kc_entry(void) {
    current_thread()->closes_never_wait = true;
    g_kc_crc = spoor_clunk_rc(g_kc_spoor);
    __atomic_store_n(&g_kc_done, true, __ATOMIC_RELEASE);
    test_kthread_park_terminal(&g_kc_exited);
}

void test_p9_closer_kthread_close_hands_off_staged_run(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    struct wbc w = {0};
    bool opened = wbc_open(&w);
    struct Thread *t = NULL;
    bool returned = false;
    u32  before = 0;
    if (opened) {
        g_kc_spoor  = w.f;
        g_kc_crc    = 1;
        g_kc_done   = false;
        g_kc_exited = false;
        hold_reader(w.a->client, true);
        g_mq_a.eagain_budget = 0xffffffffu;
        t = thread_create(kproc(), kc_entry);
        if (t) {
            ready(t);
            TEST_YIELD_UNTIL_SOFT(__atomic_load_n(&g_kc_done, __ATOMIC_ACQUIRE));
            returned = __atomic_load_n(&g_kc_done, __ATOMIC_ACQUIRE);
        }
        before = __atomic_load_n(&g_rec_a.nwrite, __ATOMIC_ACQUIRE);
        wbc_unstall(&w);
        if (t) {
            TEST_YIELD_UNTIL(__atomic_load_n(&g_kc_done, __ATOMIC_ACQUIRE));
            test_kthread_join_free(t, &g_kc_exited);
        }
        TEST_YIELD_UNTIL_SOFT(srv_clunked(&g_rec_a, w.fid));
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
    }
    struct p9_closer_stats st = closer_now();
    struct srv_rec rec = g_rec_a;
    u64 budget = dev9p_wb_budget_used();
    wbc_end(&w);

    TEST_ASSERT(opened, "a staged file on a closer session");
    TEST_ASSERT(t != NULL, "a kernel thread");
    TEST_ASSERT(returned, "its last close returned without waiting");
    TEST_EXPECT_EQ((u64)before, 0ull, "nothing reached the server before then");
    TEST_EXPECT_EQ((u64)(s64)g_kc_crc, 0ull, "the close reports nothing lost");
    TEST_EXPECT_EQ((u64)rec.nwrite, 1ull, "the closer wrote the run");
    TEST_EXPECT_EQ((u64)rec.write_len, (u64)WBC_LEN, "all of it");
    TEST_EXPECT_EQ((u64)rec.write_sum, (u64)wbc_sum(), "the bytes write() took");
    TEST_ASSERT(rec.write_at < rec.clunk_at, "then the Tclunk");
    TEST_EXPECT_EQ(st.jobs, base.jobs + 1, "one close job ran");
    TEST_EXPECT_EQ(st.dropped, base.dropped, "no entry was dropped");
    TEST_EXPECT_EQ(budget, w.budget0, "the run's budget charge came back");
}

// A close job whose write the server refuses once is run again, as a Tclunk
// that meets back-pressure is: a write never sent for want of memory comes back
// the same -EIO, and the run's explicit offsets make the resend idempotent.
void test_p9_closer_close_job_retries_a_refused_write(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    struct wbc w = {0};
    bool opened = wbc_open(&w);
    bool started = false;
    if (opened) {
        g_rec_a.fail_writes = 1;
        g_t1_spoor = w.f;
        g_wbc_crc  = 1;
        started = test_dying_start(&g_wbc_thread, wbc_drop, NULL, /*dead_now=*/true);
        if (started) {
            TEST_YIELD_UNTIL(test_dying_done(&g_wbc_thread));
            test_dying_reap(&g_wbc_thread);
        }
        TEST_YIELD_UNTIL_SOFT(srv_clunked(&g_rec_a, w.fid));
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
    }
    struct p9_closer_stats st = closer_now();
    struct srv_rec rec = g_rec_a;
    u64 budget = dev9p_wb_budget_used();
    wbc_end(&w);

    TEST_ASSERT(opened, "a staged file on a closer session");
    TEST_ASSERT(started, "a dying thread");
    TEST_EXPECT_EQ((u64)(s64)g_wbc_crc, 0ull, "the close reports nothing lost");
    TEST_EXPECT_EQ((u64)rec.nwrite_refused, 1ull, "the server refused the first write");
    TEST_EXPECT_EQ((u64)rec.nwrite, 1ull, "the closer wrote the run again");
    TEST_EXPECT_EQ((u64)rec.write_len, (u64)WBC_LEN, "all of it");
    TEST_EXPECT_EQ((u64)rec.write_sum, (u64)wbc_sum(), "the bytes write() took");
    TEST_ASSERT(rec.write_at < rec.clunk_at, "then the Tclunk");
    TEST_EXPECT_EQ(st.jobs, base.jobs + 1, "one close job ran");
    TEST_EXPECT_EQ(st.job_errors, base.job_errors, "and it lost nothing");
    TEST_EXPECT_EQ(budget, w.budget0, "the run's budget charge came back");
}

// An exits() close sets no group_exit_msg, so its first kill wins the CAS; the
// final-close mark the at-exit close sets is what makes that kill find the close
// under way and force it (part B). The Proc's own thread runs the real at-exit
// close over a handle table holding the staged file; every send meets a full
// ring, so the close sleeps until the kill.
static hidx_t g_xc_fd;

static void xc_exit_close(void *arg) {
    (void)arg;
    struct Proc *self = current_thread()->proc;
    g_xc_fd = handle_alloc(self, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE, g_t1_spoor);
    if (g_xc_fd < 0) { spoor_clunk(g_t1_spoor); return; }
    proc_close_handles_at_exit_for_test(self);
}

void test_p9_closer_first_kill_forces_exits_close(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    struct wbc w = {0};
    bool opened = wbc_open(&w);
    bool started = false, waited = false, first = false, returned = false;
    u32  before = 0;
    if (opened) {
        g_t1_spoor = w.f;                         // the handle table adopts it
        g_xc_fd    = -1;
        hold_reader(w.a->client, true);
        g_mq_a.eagain_budget = 0xffffffffu;
        started = test_dying_start(&g_wbc_thread, xc_exit_close, NULL,
                                   /*dead_now=*/false);
        if (started) {
            TEST_YIELD_UNTIL_SOFT(test_dying_parked(&g_wbc_thread) ||
                                  test_dying_done(&g_wbc_thread));
            waited = !test_dying_done(&g_wbc_thread);
            first  = __atomic_load_n(&g_wbc_thread.proc->group_exit_msg,
                                     __ATOMIC_ACQUIRE) == NULL;
            // The bare call is sound here: the Proc's one thread sleeps in the
            // close, and nothing else kills or reaps it.
            proc_group_kill(g_wbc_thread.proc);
            TEST_YIELD_UNTIL_SOFT(test_dying_done(&g_wbc_thread));
            returned = test_dying_done(&g_wbc_thread);
        }
        before = __atomic_load_n(&g_rec_a.nwrite, __ATOMIC_ACQUIRE);
        wbc_unstall(&w);
        if (started) {
            TEST_YIELD_UNTIL_SOFT(test_dying_done(&g_wbc_thread));
            if (test_dying_done(&g_wbc_thread)) test_dying_reap(&g_wbc_thread);
        }
        TEST_YIELD_UNTIL_SOFT(srv_clunked(&g_rec_a, w.fid));
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
    }
    struct p9_closer_stats st = closer_now();
    struct srv_rec rec = g_rec_a;
    u64 budget = dev9p_wb_budget_used();
    wbc_end(&w);

    TEST_ASSERT(opened, "a staged file on a closer session");
    TEST_ASSERT(started, "a closing thread");
    TEST_ASSERT(g_xc_fd >= 0, "the staged file is in its handle table");
    TEST_ASSERT(waited, "an unforced exits() close waits for its server (control)");
    TEST_ASSERT(first, "the kill is the first termination");
    TEST_ASSERT(returned, "one kill forced the exits() close");
    TEST_EXPECT_EQ((u64)before, 0ull, "nothing reached the server before then");
    TEST_EXPECT_EQ((u64)rec.nwrite, 1ull, "the closer wrote the run");
    TEST_EXPECT_EQ((u64)rec.write_len, (u64)WBC_LEN, "all of it");
    TEST_EXPECT_EQ((u64)rec.write_sum, (u64)wbc_sum(), "the bytes write() took");
    TEST_ASSERT(rec.write_at < rec.clunk_at, "then the Tclunk");
    TEST_EXPECT_EQ(st.jobs, base.jobs + 1, "one close job ran");
    TEST_EXPECT_EQ(st.job_errors, base.job_errors, "and it lost nothing");
    TEST_EXPECT_EQ(st.dropped, base.dropped, "no entry was dropped");
    TEST_EXPECT_EQ(budget, w.budget0, "the run's budget charge came back");
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

static int stall_recv_now(void *ctx, u8 *buf, size_t cap) {
    struct stall_tp *st = (struct stall_tp *)ctx;
    if (__atomic_load_n(&st->stalled, __ATOMIC_ACQUIRE)) return P9_TRANSPORT_EAGAIN;
    return st->inner.recv_now(st->inner.ctx, buf, cap);
}

static bool stall_recv_ready(void *ctx, struct poll_waiter *pw) {
    struct stall_tp *st = (struct stall_tp *)ctx;
    return st->inner.recv_ready(st->inner.ctx, pw);
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
    ops.recv_ready        = stall_recv_ready;
    ops.recv_now          = stall_recv_now;
    ops.hangup            = NULL;   // the inner's would take the wrapper's ctx
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
    TEST_EXPECT_EQ(p9_client_reader_pump_ready(a->client), 1, "the late Rwalk");
    TEST_EXPECT_EQ(p9_client_reader_pump_ready(a->client), 1, "the Rflush");
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
