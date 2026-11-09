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
#include <thylacine/larder.h>
#include <thylacine/loom.h>
#include <thylacine/notes.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>     // SYS_FILE_LOCK_*
#include <thylacine/thread.h>

#include "../../arch/arm64/uart.h"
#include <thylacine/types.h>

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
    u32 nfsync;
    u32 fsync_at;    // nmsg at the last Tfsync
    u32 nwrite_refused;
    u32 nlock;
    u32 lock_at;     // nmsg at the last Tlock
    u32 ngetattr;
    u32 fail_unlocks;      // answer this many Tlock UNLCKs with Rlerror(fail_unlock_ecode)
    u32 fail_unlock_ecode;
    u8  lock_type;
    u16 lock_cid_len;
};

// Every request on the session wbc_open builds, in arrival order, with the
// thread that sent it: the responder runs inside the sender's send, under the
// ring's lock. xw_diag prints it, so a second Twrite names its sender.
#define WIRE_LOG 16u
struct wire_ent {
    u8  type;
    u16 tag;
    int tid, pid;
    u64 off;
    u32 len;
};
static struct srv_rec *g_wire_rec;
static u32             g_wire_n;
static struct wire_ent g_wire[WIRE_LOG];

static void wire_log(u8 type, u16 tag, const u8 *req, size_t req_len) {
    u32 n = g_wire_n;
    if (n >= WIRE_LOG) return;
    struct wire_ent *e = &g_wire[n];
    struct Thread *t = current_thread();
    e->type = type;
    e->tag  = tag;
    e->tid  = t ? t->tid : -1;
    e->pid  = t && t->proc ? t->proc->pid : -1;
    e->off  = 0;
    e->len  = 0;
    if (type == P9_TWRITE && req_len >= P9_HDR_LEN + 16) {
        const u8 *b = req + P9_HDR_LEN + 4;
        for (u32 i = 0; i < 8; i++) e->off |= (u64)b[i] << (8 * i);
        e->len = (u32)b[8] | (u32)b[9] << 8 | (u32)b[10] << 16 | (u32)b[11] << 24;
    }
    g_wire_n = n + 1;
}

static int rec_responder(void *ctx, const u8 *req, size_t req_len,
                         u8 *resp, size_t resp_cap) {
    struct srv_rec *r = (struct srv_rec *)ctx;
    u32 size; u8 type; u16 tag;
    if (r && p9_peek_header(req, req_len, &size, &type, &tag) == 0) {
        r->nmsg++;
        if (r == g_wire_rec) wire_log(type, tag, req, req_len);
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
        } else if (type == P9_TLOCK && req_len >= P9_HDR_LEN + 5) {
            r->lock_at   = r->nmsg;
            r->lock_type = req[11];
            if (req_len >= 38) r->lock_cid_len = (u16)req[36] | (u16)req[37] << 8;
            __atomic_store_n(&r->nlock, r->nlock + 1, __ATOMIC_RELEASE);
            if (req[11] == P9_LOCK_TYPE_UNLCK && r->fail_unlocks) {
                r->fail_unlocks--;
                u32 ec = r->fail_unlock_ecode;
                if (resp_cap < 11) return -1;
                resp[0] = 11; resp[1] = resp[2] = resp[3] = 0;
                resp[4] = P9_RLERROR;
                resp[5] = (u8)tag; resp[6] = (u8)(tag >> 8);
                resp[7] = (u8)ec; resp[8] = (u8)(ec >> 8);
                resp[9] = (u8)(ec >> 16); resp[10] = (u8)(ec >> 24);
                return 11;
            }
            if (resp_cap < P9_HDR_LEN + 1) return -1;
            resp[0] = 8; resp[1] = resp[2] = resp[3] = 0;
            resp[4] = P9_RLOCK;
            resp[5] = (u8)tag; resp[6] = (u8)(tag >> 8);
            resp[7] = P9_LOCK_SUCCESS;
            return 8;
        } else if (type == P9_TGETATTR) {
            __atomic_store_n(&r->ngetattr, r->ngetattr + 1, __ATOMIC_RELEASE);
        } else if (type == P9_TFSYNC) {
            r->fsync_at = r->nmsg;
            __atomic_store_n(&r->nfsync, r->nfsync + 1, __ATOMIC_RELEASE);
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
    p9_mq_loopback_set_eagain_budget(&g_mq_a, 1);  // the Tclunk meets a full ring
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
    u32 budget = p9_mq_loopback_eagain_budget(&g_mq_a);
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
    g_wire_rec = &g_rec_a;
    g_wire_n   = 0;
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
    p9_mq_loopback_set_eagain_budget(&g_mq_a, 0);
    hold_reader(w->a->client, false);
    (void)p9_client_walk_one(w->a->client, 0, p9_client_alloc_fid(w->a->client),
                             (const u8 *)"u", 1, NULL);
}

static struct test_dying g_wbc_thread;
static int               g_wbc_crc;

// A descriptor's close: the last close, then the slot's reference.
static int wbc_fd_close(struct Spoor *c) {
    int frc = dev9p.fd_close(c, true);
    int crc = spoor_clunk_rc(c);
    return frc ? frc : crc;
}

static void wbc_drop(void *arg) {
    (void)arg;
    g_wbc_crc = wbc_fd_close(g_t1_spoor);
}

static void wbc_exit_drop(void *arg) {
    (void)arg;
    struct Thread *self = current_thread();
    self->exit_close_active = true;
    g_wbc_crc = wbc_fd_close(g_t1_spoor);
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

// B-3h (ARCH 9.6.11): a killed lock holder's release. The open file's staged
// run, which another Proc still shares (so no close of it hands the run over),
// is written by a closer before the lock fid's Tclunk releases the lock. The
// dying thread can send neither: its flush goes to the closer as a job alone,
// and its Tclunk after it.
static struct Spoor *g_lr_owner;

static void lr_release(void *arg) {
    (void)arg;
    (void)dev9p.lock_sync(g_t1_spoor, DEV_LOCK_SYNC_RELEASE);
    spoor_clunk(g_lr_owner);
}

void test_p9_closer_dying_lock_release_flushes_shared_run(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    struct wbc w = {0};
    bool opened = wbc_open(&w);
    bool started = false;
    u32 ofid = P9_NOFID;
    if (opened) {
        int err = 0;
        g_lr_owner = dev9p.lock_owner(w.f, &err);
        if (g_lr_owner) {
            ofid = ((struct dev9p_priv *)g_lr_owner->aux)->fid;
            g_t1_spoor = w.f;
            started = test_dying_start(&g_wbc_thread, lr_release, NULL, /*dead_now=*/true);
            if (started) {
                TEST_YIELD_UNTIL(test_dying_done(&g_wbc_thread));
                test_dying_reap(&g_wbc_thread);
            }
            TEST_YIELD_UNTIL_SOFT(srv_clunked(&g_rec_a, ofid));
            TEST_YIELD_UNTIL_SOFT(closer_quiet());
        }
    }
    struct p9_closer_stats st = closer_now();
    struct srv_rec rec = g_rec_a;   // before the shared file's own close
    int crc = w.f ? spoor_clunk_rc(w.f) : 0;
    wbc_end(&w);

    TEST_ASSERT(opened, "a staged file on a closer session");
    TEST_ASSERT(ofid != P9_NOFID, "a lock fid minted from it");
    TEST_ASSERT(started, "a dying thread");
    TEST_EXPECT_EQ((u64)rec.nwrite, 1ull, "a closer wrote the shared run");
    TEST_EXPECT_EQ((u64)rec.write_len, (u64)WBC_LEN, "all of it");
    TEST_EXPECT_EQ((u64)rec.write_sum, (u64)wbc_sum(), "the bytes write() took");
    TEST_ASSERT(srv_clunked(&rec, ofid), "the lock fid's Tclunk was sent");
    TEST_ASSERT(rec.write_at < rec.clunk_at, "after the run landed");
    TEST_ASSERT(!srv_clunked(&rec, w.fid), "and the shared file stayed open");
    TEST_EXPECT_EQ(st.jobs, base.jobs + 1, "one job ran");
    TEST_EXPECT_EQ(st.sent, base.sent + 1, "one Tclunk sent: the lock fid's");
    TEST_EXPECT_EQ(st.dropped, base.dropped, "and the job alone asked for none");
    TEST_EXPECT_EQ(st.refused, base.refused, "nor had one refused");
    TEST_EXPECT_EQ((u64)(s64)crc, 0ull, "the shared file's close later loses nothing");
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
        p9_mq_loopback_set_eagain_budget(&g_mq_a, 0xffffffffu);
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

// A last reference never waits for its server, on any thread: an exec evicting
// an image, a reap draining a mapping, and the Loom SQPOLL kthread, which
// loom_free joins without bound. This thread has no kill to end a wait and no
// mark that it is special. Every send meets a full ring; the drop returns at once
// and the closer writes the run later.
static struct Spoor  *g_kc_spoor;
static volatile int   g_kc_crc;
static volatile bool  g_kc_done;
static volatile bool  g_kc_exited;

static void kc_entry(void) {
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
        p9_mq_loopback_set_eagain_budget(&g_mq_a, 0xffffffffu);
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

// A dying thread's last close, with a holder that may never let go (a read-only
// map's): it can send nothing, and no last reference is coming to hand the work
// on. So a closer lands the run and releases the lock now, with the holder alive.
static void hc_close(void *arg) {
    (void)arg;
    g_wbc_crc = dev9p.fd_close(g_t1_spoor, true);
}

void test_p9_closer_dying_last_close_with_holder(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    struct wbc w = {0};
    bool opened = wbc_open(&w), took = false, staged = false, started = false;
    u32  nwrite0 = 0, nlock0 = 0, inside = 0, past = 0;
    u64  own0 = 0, own1 = 0;
    u8   pg[16];
    if (opened) {
        took = dev9p.lock(w.f, SYS_FILE_LOCK_WR, 0, 0, NULL) == 0;   // lands the first run
        u8 chunk[WBC_LEN];
        for (u32 i = 0; i < WBC_LEN; i++) chunk[i] = wbc_pat(i);
        staged  = dev9p.write(w.f, chunk, WBC_LEN, WBC_LEN) == (long)WBC_LEN;
        // The grant dropped what the first run installed. An own page that ends
        // where this run starts: a flush that installed would extend it.
        larder_page_install_own(&w.a->client->larder, w.f->qid.path, 0, 0, chunk, WBC_LEN);
        nwrite0 = __atomic_load_n(&g_rec_a.nwrite, __ATOMIC_ACQUIRE);
        nlock0  = __atomic_load_n(&g_rec_a.nlock, __ATOMIC_ACQUIRE);
        inside  = larder_page_serve(&w.a->client->larder, w.f->qid.path, 0, 0,
                                    sizeof(pg), 0, pg, NULL);
        own0    = w.a->client->larder.page_own_installs;
        spoor_ref(w.f);            // the holder
        g_t1_spoor = w.f;
        g_wbc_crc  = 1;
        started = test_dying_start(&g_wbc_thread, hc_close, NULL, /*dead_now=*/true);
        if (started) {
            TEST_YIELD_UNTIL(test_dying_done(&g_wbc_thread));
            test_dying_reap(&g_wbc_thread);
        }
        TEST_YIELD_UNTIL_SOFT(__atomic_load_n(&g_rec_a.nlock, __ATOMIC_ACQUIRE) > nlock0);
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
    }
    struct p9_closer_stats st = closer_now();
    struct srv_rec rec = g_rec_a;   // with the holder alive
    u64 budget = dev9p_wb_budget_used();
    if (opened) {
        own1 = w.a->client->larder.page_own_installs;
        past = larder_page_serve(&w.a->client->larder, w.f->qid.path, 0, 0,
                                 sizeof(pg), 0, pg, NULL);
        spoor_clunk(w.f);          // the holder's
        spoor_clunk(w.f);          // the slot's
        TEST_YIELD_UNTIL_SOFT(srv_clunked(&g_rec_a, w.fid));
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
    }
    bool clunked = opened && srv_clunked(&g_rec_a, w.fid);
    wbc_end(&w);

    TEST_ASSERT(opened, "a staged file on a closer session");
    TEST_ASSERT(took, "a lock");
    TEST_ASSERT(staged, "and a run staged under it");
    TEST_ASSERT(started, "a dying thread");
    TEST_EXPECT_EQ((u64)inside, (u64)sizeof(pg), "an own page ends where the run starts");
    TEST_EXPECT_EQ(own1, own0, "a flush past the last close extends nothing");
    TEST_EXPECT_EQ((u64)past, 0ull, "and the file's pages have gone");
    TEST_EXPECT_EQ((u64)(s64)g_wbc_crc, 0ull, "the close reports nothing lost");
    TEST_EXPECT_EQ((u64)rec.nwrite, (u64)nwrite0 + 1, "a closer landed the run");
    TEST_EXPECT_EQ(rec.write_off, (u64)WBC_LEN, "at its offset");
    TEST_EXPECT_EQ((u64)rec.write_len, (u64)WBC_LEN, "all of it");
    TEST_EXPECT_EQ((u64)rec.nlock, (u64)nlock0 + 1, "then released the lock");
    TEST_EXPECT_EQ((u64)rec.lock_type, (u64)P9_LOCK_TYPE_UNLCK, "with an unlock");
    TEST_ASSERT(rec.lock_cid_len > 0, "naming what the take named");
    TEST_ASSERT(rec.write_at < rec.lock_at, "sent after the run landed");
    TEST_ASSERT(!srv_clunked(&rec, w.fid), "the holder keeps the fid");
    TEST_EXPECT_EQ(st.jobs, base.jobs + 1, "one job ran");
    TEST_EXPECT_EQ(st.job_errors, base.job_errors, "and it lost nothing");
    TEST_EXPECT_EQ(budget, w.budget0, "the run's budget charge came back");
    TEST_ASSERT(clunked, "the holder's drop clunked the fid");
}

// That close's job fails: write() said those bytes were written and nobody reads
// the latch from here on, so the job says so. The lock and the buffer still go.
void test_p9_closer_job_loses_a_run_loudly(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    struct wbc w = {0};
    bool opened = wbc_open(&w), took = false, staged = false, started = false;
    u32  nlock0 = 0;
    if (opened) {
        took = dev9p.lock(w.f, SYS_FILE_LOCK_WR, 0, 0, NULL) == 0;   // lands the first run
        u8 chunk[WBC_LEN];
        for (u32 i = 0; i < WBC_LEN; i++) chunk[i] = wbc_pat(i);
        staged = dev9p.write(w.f, chunk, WBC_LEN, WBC_LEN) == (long)WBC_LEN;
        nlock0 = __atomic_load_n(&g_rec_a.nlock, __ATOMIC_ACQUIRE);
        p9_close_flush_quiet_for_test(true);
        g_rec_a.fail_writes = 1;
        spoor_ref(w.f);            // the holder
        g_t1_spoor = w.f;
        g_wbc_crc  = 1;
        started = test_dying_start(&g_wbc_thread, hc_close, NULL, /*dead_now=*/true);
        if (started) {
            TEST_YIELD_UNTIL(test_dying_done(&g_wbc_thread));
            test_dying_reap(&g_wbc_thread);
        }
        TEST_YIELD_UNTIL_SOFT(__atomic_load_n(&g_rec_a.nlock, __ATOMIC_ACQUIRE) > nlock0);
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
        p9_close_flush_quiet_for_test(false);
    }
    struct p9_closer_stats st = closer_now();
    struct srv_rec rec = g_rec_a;
    u64 budget = dev9p_wb_budget_used();
    if (opened) {
        spoor_clunk(w.f);          // the holder's
        spoor_clunk(w.f);          // the slot's
        TEST_YIELD_UNTIL_SOFT(srv_clunked(&g_rec_a, w.fid));
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
    }
    wbc_end(&w);

    TEST_ASSERT(opened, "a staged file on a closer session");
    TEST_ASSERT(took, "a lock");
    TEST_ASSERT(staged, "and a run staged under it");
    TEST_ASSERT(started, "a dying thread");
    TEST_EXPECT_EQ((u64)rec.fail_writes, 0ull, "the server refused the write");
    TEST_EXPECT_EQ(st.jobs, base.jobs + 1, "one job ran");
    TEST_EXPECT_EQ(st.job_errors, base.job_errors + 1, "and it reported the loss");
    TEST_EXPECT_EQ((u64)rec.nlock, (u64)nlock0 + 1, "the lock went anyway");
    TEST_EXPECT_EQ((u64)rec.lock_type, (u64)P9_LOCK_TYPE_UNLCK, "by an unlock");
    TEST_EXPECT_EQ(budget, w.budget0, "and the buffer came back");
}

// A dying last close of an attach root: the session owns the fid, so no clunk
// follows to release the lock. With no holder at all, a closer still does it.
void test_p9_closer_dying_last_close_of_a_root(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    struct wbc w = {0};
    bool opened = wbc_open(&w), took = false, started = false;
    u32  nlock0 = 0;
    int  ref = 0;
    if (opened) {
        took   = dev9p.lock(w.root, SYS_FILE_LOCK_WR, 0, 0, NULL) == 0;
        nlock0 = __atomic_load_n(&g_rec_a.nlock, __ATOMIC_ACQUIRE);
        ref    = __atomic_load_n(&w.root->ref, __ATOMIC_ACQUIRE);
        g_t1_spoor = w.root;
        g_wbc_crc  = 1;
        started = test_dying_start(&g_wbc_thread, hc_close, NULL, /*dead_now=*/true);
        if (started) {
            TEST_YIELD_UNTIL(test_dying_done(&g_wbc_thread));
            test_dying_reap(&g_wbc_thread);
        }
        TEST_YIELD_UNTIL_SOFT(__atomic_load_n(&g_rec_a.nlock, __ATOMIC_ACQUIRE) > nlock0);
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
    }
    struct p9_closer_stats st = closer_now();
    struct srv_rec rec = g_rec_a;
    if (opened) {
        spoor_clunk(w.f);          // the file wbc_open staged: its own hand-off
        TEST_YIELD_UNTIL_SOFT(srv_clunked(&g_rec_a, w.fid));
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
    }
    wbc_end(&w);

    TEST_ASSERT(opened, "a session");
    TEST_ASSERT(took, "a lock on its root");
    TEST_ASSERT(started, "a dying thread");
    TEST_EXPECT_EQ((u64)ref, 1ull, "the root has no other holder");
    TEST_EXPECT_EQ((u64)rec.nlock, (u64)nlock0 + 1, "a closer released the lock");
    TEST_EXPECT_EQ((u64)rec.lock_type, (u64)P9_LOCK_TYPE_UNLCK, "with an unlock");
    TEST_EXPECT_EQ(st.jobs, base.jobs + 1, "by a job");
}

// A peer is flushing when the last close comes, and that close's thread is dying.
// The job waits for it asleep (no death ends a closer's wait, and a hung server
// can make it long), and what the peer installed goes when it retires: that write
// lands past the last close, unordered with the file's later writers.
void test_p9_closer_job_waits_out_a_peer_flush(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    struct wbc w = {0};
    bool opened = wbc_open(&w), started = false;
    u64  sleeps0 = 0, sleeps = 0;
    u32  nwrite_mid = 1, inside = 0, past = 1;
    u8   pg[16];
    if (opened) {
        struct dev9p_priv *p = (struct dev9p_priv *)w.f->aux;
        spin_lock(&p->wb_lock);
        p->wb_flushers++;          // the peer: wbc_open's run is frozen, on the wire
        spin_unlock(&p->wb_lock);
        spoor_ref(w.f);            // the holder
        sleeps0 = dev9p_wb_flush_sleeps();
        g_t1_spoor = w.f;
        g_wbc_crc  = 1;
        started = test_dying_start(&g_wbc_thread, hc_close, NULL, /*dead_now=*/true);
        if (started) {
            TEST_YIELD_UNTIL(test_dying_done(&g_wbc_thread));
            test_dying_reap(&g_wbc_thread);
        }
        TEST_YIELD_UNTIL_SOFT(dev9p_wb_flush_sleeps() > sleeps0);
        sleeps     = dev9p_wb_flush_sleeps();
        nwrite_mid = __atomic_load_n(&g_rec_a.nwrite, __ATOMIC_ACQUIRE);

        // The peer's write lands: its install, then its retirement.
        u8 chunk[WBC_LEN];
        for (u32 i = 0; i < WBC_LEN; i++) chunk[i] = wbc_pat(i);
        larder_page_install_own(&w.a->client->larder, w.f->qid.path, 0, 0, chunk, WBC_LEN);
        inside = larder_page_serve(&w.a->client->larder, w.f->qid.path, 0, 0,
                                   sizeof(pg), 0, pg, NULL);
        spin_lock(&p->wb_lock);
        p->wb_flushers--;
        p->wb_len = 0;
        spin_unlock(&p->wb_lock);
        TEST_YIELD_UNTIL_SOFT(closer_now().jobs > base.jobs);
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
        past = larder_page_serve(&w.a->client->larder, w.f->qid.path, 0, 0,
                                 sizeof(pg), 0, pg, NULL);
    }
    struct p9_closer_stats st = closer_now();
    u32 nwrite_end = __atomic_load_n(&g_rec_a.nwrite, __ATOMIC_ACQUIRE);
    u64 budget = dev9p_wb_budget_used();
    if (opened) {
        spoor_clunk(w.f);          // the holder's
        spoor_clunk(w.f);          // the slot's
        TEST_YIELD_UNTIL_SOFT(srv_clunked(&g_rec_a, w.fid));
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
    }
    wbc_end(&w);

    TEST_ASSERT(opened, "a staged file on a closer session");
    TEST_ASSERT(started, "a dying thread");
    TEST_ASSERT(sleeps > sleeps0, "the closer slept on the peer's flush");
    TEST_EXPECT_EQ((u64)nwrite_mid, 0ull, "and sent nothing while it did");
    TEST_EXPECT_EQ((u64)inside, (u64)sizeof(pg), "the peer's pages served");
    TEST_EXPECT_EQ((u64)past, 0ull, "until the job dropped them");
    TEST_EXPECT_EQ((u64)nwrite_end, 0ull, "the peer's run was the only write");
    TEST_EXPECT_EQ(st.jobs, base.jobs + 1, "one job ran");
    TEST_EXPECT_EQ(st.job_errors, base.job_errors, "and it lost nothing");
    TEST_EXPECT_EQ(budget, w.budget0, "the buffer came back");
}

// The path a lock takes: a dying thread's release queues one job and its last
// close a second, and the first job's flush retires before the second looks. The
// first stands in for itself here, held at the peer's flush. Its install landed
// past the last close, unordered with the file's later writers, so the second job
// goes by what stood at that close and not by what it finds.
static void hc_sync_close(void *arg) {
    (void)arg;
    (void)dev9p.lock_sync(g_t1_spoor, DEV_LOCK_SYNC_RELEASE);
    g_wbc_crc = dev9p.fd_close(g_t1_spoor, true);
}

void test_p9_closer_job_drops_a_flush_that_retired_first(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    struct wbc w = {0};
    bool opened = wbc_open(&w), started = false;
    u32  inside = 0, past = 1;
    u8   pg[16];
    if (opened) {
        struct dev9p_priv *p = (struct dev9p_priv *)w.f->aux;
        spin_lock(&p->wb_lock);
        p->wb_flushers++;          // the flush: wbc_open's run, on the wire
        spin_unlock(&p->wb_lock);
        g_t1_spoor = w.f;
        g_wbc_crc  = 1;
        started = test_dying_start(&g_wbc_thread, hc_sync_close, NULL, /*dead_now=*/true);
        if (started) {
            TEST_YIELD_UNTIL(test_dying_done(&g_wbc_thread));
            test_dying_reap(&g_wbc_thread);
        }
        // The flush lands: its install, then its retirement, with both jobs queued.
        u8 chunk[WBC_LEN];
        for (u32 i = 0; i < WBC_LEN; i++) chunk[i] = wbc_pat(i);
        larder_page_install_own(&w.a->client->larder, w.f->qid.path, 0, 0, chunk, WBC_LEN);
        inside = larder_page_serve(&w.a->client->larder, w.f->qid.path, 0, 0,
                                   sizeof(pg), 0, pg, NULL);
        spin_lock(&p->wb_lock);
        p->wb_flushers--;
        p->wb_len = 0;
        spin_unlock(&p->wb_lock);
        TEST_YIELD_UNTIL_SOFT(closer_now().jobs >= base.jobs + 2);
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
        past = larder_page_serve(&w.a->client->larder, w.f->qid.path, 0, 0,
                                 sizeof(pg), 0, pg, NULL);
    }
    struct p9_closer_stats st = closer_now();
    u32 nwrite = __atomic_load_n(&g_rec_a.nwrite, __ATOMIC_ACQUIRE);
    u64 budget = dev9p_wb_budget_used();
    if (opened) {
        spoor_clunk(w.f);          // the slot's
        TEST_YIELD_UNTIL_SOFT(srv_clunked(&g_rec_a, w.fid));
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
    }
    wbc_end(&w);

    TEST_ASSERT(opened, "a staged file on a closer session");
    TEST_ASSERT(started, "a dying thread");
    TEST_EXPECT_EQ((u64)inside, (u64)sizeof(pg), "the flush's pages served");
    TEST_EXPECT_EQ((u64)past, 0ull, "until the last close's job dropped them");
    TEST_EXPECT_EQ(st.jobs, base.jobs + 2, "two jobs ran");
    TEST_EXPECT_EQ(st.job_errors, base.job_errors, "and lost nothing");
    TEST_EXPECT_EQ((u64)nwrite, 0ull, "the flush's run was the only write");
    TEST_EXPECT_EQ(budget, w.budget0, "the buffer came back");
}

// The same two jobs, and the server refuses the first one's write. The second has
// nothing to land, and nobody else reads the latch: write() said those bytes were
// written, so it reports them.
void test_p9_closer_job_reports_a_first_jobs_loss(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    struct wbc w = {0};
    bool opened = wbc_open(&w), started = false;
    if (opened) {
        struct dev9p_priv *p = (struct dev9p_priv *)w.f->aux;
        spin_lock(&p->wb_lock);
        p->wb_flushers++;          // holds the first job until both are queued
        spin_unlock(&p->wb_lock);
        p9_close_flush_quiet_for_test(true);
        g_rec_a.fail_writes = 1;
        g_t1_spoor = w.f;
        g_wbc_crc  = 1;
        started = test_dying_start(&g_wbc_thread, hc_sync_close, NULL, /*dead_now=*/true);
        if (started) {
            TEST_YIELD_UNTIL(test_dying_done(&g_wbc_thread));
            test_dying_reap(&g_wbc_thread);
        }
        spin_lock(&p->wb_lock);
        p->wb_flushers--;          // the run stays staged: the first job lands it
        spin_unlock(&p->wb_lock);
        TEST_YIELD_UNTIL_SOFT(closer_now().jobs >= base.jobs + 2);
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
        p9_close_flush_quiet_for_test(false);
    }
    struct p9_closer_stats st = closer_now();
    struct srv_rec rec = g_rec_a;
    u64 budget = dev9p_wb_budget_used();
    if (opened) {
        spoor_clunk(w.f);          // the slot's
        TEST_YIELD_UNTIL_SOFT(srv_clunked(&g_rec_a, w.fid));
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
    }
    wbc_end(&w);

    TEST_ASSERT(opened, "a staged file on a closer session");
    TEST_ASSERT(started, "a dying thread");
    TEST_EXPECT_EQ((u64)rec.fail_writes, 0ull, "the server refused the write");
    TEST_EXPECT_EQ(st.jobs, base.jobs + 2, "two jobs ran");
    TEST_EXPECT_EQ(st.job_errors, base.job_errors + 1, "and the last close's reported it, once");
    TEST_EXPECT_EQ((u64)(s64)g_wbc_crc, 0ull, "the close itself reported nothing");
    TEST_EXPECT_EQ(budget, w.budget0, "the buffer came back");
}

// The same first job, and it fails before the close comes: no run stands at that
// close, so no second job follows it, and a dying thread's answer reaches nobody.
// The close says it.
static u64 g_hc_jobs;

static void hc_sync_then_close(void *arg) {
    (void)arg;
    (void)dev9p.lock_sync(g_t1_spoor, DEV_LOCK_SYNC_RELEASE);
    TEST_YIELD_UNTIL_SOFT(closer_now().jobs >= g_hc_jobs);
    g_wbc_crc = dev9p.fd_close(g_t1_spoor, true);
}

void test_p9_closer_dying_close_reports_a_retired_flush(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    struct wbc w = {0};
    bool opened = wbc_open(&w), started = false;
    if (opened) {
        p9_close_flush_quiet_for_test(true);
        g_rec_a.fail_writes = 1;
        g_t1_spoor = w.f;
        g_wbc_crc  = 0;
        g_hc_jobs  = base.jobs + 1;
        started = test_dying_start(&g_wbc_thread, hc_sync_then_close, NULL, /*dead_now=*/true);
        if (started) {
            TEST_YIELD_UNTIL(test_dying_done(&g_wbc_thread));
            test_dying_reap(&g_wbc_thread);
        }
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
        p9_close_flush_quiet_for_test(false);
    }
    struct p9_closer_stats st = closer_now();
    struct srv_rec rec = g_rec_a;
    u64 budget = dev9p_wb_budget_used();
    if (opened) {
        spoor_clunk(w.f);          // the slot's
        TEST_YIELD_UNTIL_SOFT(srv_clunked(&g_rec_a, w.fid));
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
    }
    wbc_end(&w);

    TEST_ASSERT(opened, "a staged file on a closer session");
    TEST_ASSERT(started, "a dying thread");
    TEST_EXPECT_EQ((u64)rec.fail_writes, 0ull, "the server refused the write");
    TEST_EXPECT_EQ(st.jobs, base.jobs + 1, "one job ran");
    TEST_EXPECT_EQ(st.job_errors, base.job_errors + 1, "and the close reported its loss");
    TEST_ASSERT(g_wbc_crc != 0, "the answer nobody got");
    TEST_EXPECT_EQ(budget, w.budget0, "the buffer came back");
}

// A live thread's last close fails its flush. close(2) hands that to the program,
// which is all it owes. An exit's close has no program to hand it to.
void test_p9_closer_unheard_close_reports_its_loss(void) {
    for (u32 i = 0; i < 2; i++) {
        bool heard = i == 0;
        TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
        struct p9_closer_stats base = closer_now();
        struct wbc w = {0};
        bool opened = wbc_open(&w);
        int  crc = 0;
        if (opened) {
            p9_close_flush_quiet_for_test(true);
            g_rec_a.fail_writes = 1;
            crc = dev9p.fd_close(w.f, heard);
            p9_close_flush_quiet_for_test(false);
        }
        struct p9_closer_stats st = closer_now();
        if (opened) {
            spoor_clunk(w.f);      // the slot's
            TEST_YIELD_UNTIL_SOFT(srv_clunked(&g_rec_a, w.fid));
            TEST_YIELD_UNTIL_SOFT(closer_quiet());
        }
        wbc_end(&w);

        TEST_ASSERT(opened, "a staged file on a closer session");
        TEST_ASSERT(crc != 0, "the server refused the write");
        if (heard)
            TEST_EXPECT_EQ(st.job_errors, base.job_errors, "the program hears it");
        else
            TEST_EXPECT_EQ(st.job_errors, base.job_errors + 1, "nobody does: the close says it");
    }
}

// A dying last close with a holder and a lock, and no run: the job only unlocks.
// What landed before that close landed inside the open, so its pages stand.
void test_p9_closer_job_keeps_what_landed_inside_the_open(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    struct wbc w = {0};
    bool opened = wbc_open(&w), took = false, started = false;
    u32  nlock0 = 0, inside = 0, past = 0;
    u8   pg[16];
    if (opened) {
        took = dev9p.lock(w.f, SYS_FILE_LOCK_WR, 0, 0, NULL) == 0;   // lands the run
        u8 chunk[WBC_LEN];
        for (u32 i = 0; i < WBC_LEN; i++) chunk[i] = wbc_pat(i);
        // The grant dropped what the run installed. One that lands after a grant
        // stays, as this page does.
        larder_page_install_own(&w.a->client->larder, w.f->qid.path, 0, 0, chunk, WBC_LEN);
        nlock0 = __atomic_load_n(&g_rec_a.nlock, __ATOMIC_ACQUIRE);
        inside = larder_page_serve(&w.a->client->larder, w.f->qid.path, 0, 0,
                                   sizeof(pg), 0, pg, NULL);
        spoor_ref(w.f);            // the holder
        g_t1_spoor = w.f;
        g_wbc_crc  = 1;
        started = test_dying_start(&g_wbc_thread, hc_close, NULL, /*dead_now=*/true);
        if (started) {
            TEST_YIELD_UNTIL(test_dying_done(&g_wbc_thread));
            test_dying_reap(&g_wbc_thread);
        }
        TEST_YIELD_UNTIL_SOFT(__atomic_load_n(&g_rec_a.nlock, __ATOMIC_ACQUIRE) > nlock0);
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
        past = larder_page_serve(&w.a->client->larder, w.f->qid.path, 0, 0,
                                 sizeof(pg), 0, pg, NULL);
    }
    struct p9_closer_stats st = closer_now();
    struct srv_rec rec = g_rec_a;
    if (opened) {
        spoor_clunk(w.f);          // the holder's
        spoor_clunk(w.f);          // the slot's
        TEST_YIELD_UNTIL_SOFT(srv_clunked(&g_rec_a, w.fid));
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
    }
    wbc_end(&w);

    TEST_ASSERT(opened, "a staged file on a closer session");
    TEST_ASSERT(took, "a lock");
    TEST_ASSERT(started, "a dying thread");
    TEST_EXPECT_EQ((u64)inside, (u64)sizeof(pg), "its page serves");
    TEST_EXPECT_EQ((u64)past, (u64)sizeof(pg), "and still does past the close's job");
    TEST_EXPECT_EQ((u64)(s64)g_wbc_crc, 0ull, "the close reports nothing lost");
    TEST_EXPECT_EQ((u64)rec.nlock, (u64)nlock0 + 1, "the job released the lock");
    TEST_EXPECT_EQ((u64)rec.lock_type, (u64)P9_LOCK_TYPE_UNLCK, "with an unlock");
    TEST_EXPECT_EQ(st.jobs, base.jobs + 1, "one job ran");
    TEST_EXPECT_EQ(st.job_errors, base.job_errors, "and it lost nothing");
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
        p9_mq_loopback_set_eagain_budget(&g_mq_a, 0xffffffffu);
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

// The exit wins in the final close (I-24, ARCH 7.9.1 part B,
// dec-2026-10-13-exit-wins-final-close): a kill that lands after a Proc's last
// thread committed to exit forces its final close and leaves the exit's
// status, whether the thread left by exits() or as the last SYS_THREAD_EXIT. A
// real child holds the staged file in its handle table and every send meets a
// full ring, so its close sleeps until a kill forces it. The kill lands while
// the close sleeps, or at its start: on the exiting thread, between the
// commit's unlock and the close, where only a mark published at the commit
// lets a first kill find the close under way.
static struct Proc   *volatile g_xw_proc;
static volatile int            g_xw_tid;
static volatile bool           g_xw_last_out;
static volatile bool           g_xw_window;
static volatile bool           g_xw_hook_ran;

static void xw_window_kill(void) {
    proc_exit_close_test_set_window_hook(NULL);
    g_xw_hook_ran = true;
    irq_state_t s = proc_table_lock_acquire();
    proc_group_kill(current_thread()->proc);
    proc_table_lock_release(s);
}

static void xw_child(void *arg) {
    (void)arg;
    struct Proc *self = current_thread()->proc;
    g_xc_fd = handle_alloc(self, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE, g_t1_spoor);
    if (g_xc_fd < 0) { spoor_clunk(g_t1_spoor); exits_code(3, "no handle"); }
    if (g_xw_window) proc_exit_close_test_set_window_hook(xw_window_kill);
    __atomic_store_n(&g_xw_tid, current_thread()->tid, __ATOMIC_RELEASE);
    __atomic_store_n(&g_xw_proc, self, __ATOMIC_RELEASE);
    if (g_xw_last_out) thread_exit_self();
    exits_code(5, "five");
}

static bool xw_zombie(void) {
    struct Proc *p = __atomic_load_n(&g_xw_proc, __ATOMIC_ACQUIRE);
    return p && __atomic_load_n(&p->state, __ATOMIC_ACQUIRE) == PROC_STATE_ZOMBIE;
}

// The child's thread is freed at its last switch-out (ARCH 7.3.2), which a
// kill in the close window can bring before this runs, so it is read by tid.
static bool xw_in_close(void) {
    struct Proc *p = __atomic_load_n(&g_xw_proc, __ATOMIC_ACQUIRE);
    return p &&
           (__atomic_load_n(&p->proc_flags, __ATOMIC_ACQUIRE) & PROC_FLAG_EXIT_CLOSING) &&
           test_thread_sleeping(p, __atomic_load_n(&g_xw_tid, __ATOMIC_ACQUIRE));
}

struct xw_obs {
    bool opened, started, installed, slept, forced, hook_ran, reaped;
    int  status;
    u32  before, nwrite, write_len, write_sum;
    // What the server and the pool showed when the waits for the clunk and the
    // quiet pool ended, and how long each took: on a mismatch xw_expect prints
    // them, and a second, longer look tells a slow closer from a stuck one.
    bool clunked, quiet;
    u32  nclunk, nmsg;
    u64  clunk_wait_ns, quiet_wait_ns;
    struct p9_closer_stats base, at;
    bool late_clunked, late_quiet;
    u32  late_nwrite;
    u64  late_wait_ns;
    struct p9_closer_stats late;
    int  self_tid, child_pid;
    u32  nwire;
    struct wire_ent wire[WIRE_LOG];
};

static void xw_run(bool last_out, bool window, struct xw_obs *o) {
    struct wbc w = {0};
    for (size_t i = 0; i < sizeof(*o); i++) ((u8 *)o)[i] = 0;
    o->status = -1;
    o->base   = closer_now();
    o->opened = wbc_open(&w);
    if (o->opened) {
        g_t1_spoor    = w.f;                     // the child's handle table adopts it
        g_xc_fd       = -1;
        g_xw_proc     = NULL;
        g_xw_tid      = 0;
        g_xw_last_out = last_out;
        g_xw_window   = window;
        g_xw_hook_ran = false;
        hold_reader(w.a->client, true);
        p9_mq_loopback_set_eagain_budget(&g_mq_a, 0xffffffffu);
        int pid = rfork(RFPROC, xw_child, NULL);
        o->started   = pid > 0;
        o->child_pid = pid;
        if (o->started) {
            TEST_YIELD_UNTIL_SOFT(xw_zombie() || xw_in_close());
            o->installed = g_xc_fd >= 0;
            o->slept     = !xw_zombie() && xw_in_close();
            if (!window && o->slept) {
                // Only this test, the child's parent, reaps it, so the Proc
                // outlives the call.
                irq_state_t s = proc_table_lock_acquire();
                proc_group_kill(g_xw_proc);
                proc_table_lock_release(s);
                TEST_YIELD_UNTIL_SOFT(xw_zombie());
            }
            o->forced = xw_zombie();             // before the server answered
        }
        o->hook_ran = g_xw_hook_ran;
        o->before   = __atomic_load_n(&g_rec_a.nwrite, __ATOMIC_ACQUIRE);
        wbc_unstall(&w);
        if (o->started) {
            // A close that never ends fails here rather than hanging the
            // suite; the fixture it still uses is then left alone.
            TEST_YIELD_UNTIL_SOFT(xw_zombie());
            int st = -1;
            if (xw_zombie())
                o->reaped = wait_pid_for(pid, WAIT_PACKED, &st) == pid;
            o->status = st;
        }
        proc_exit_close_test_set_window_hook(NULL);
        if (o->started && !o->reaped) return;
        u64 t0 = timer_now_ns();
        TEST_YIELD_UNTIL_SOFT(srv_clunked(&g_rec_a, w.fid));
        u64 t1 = timer_now_ns();
        TEST_YIELD_UNTIL_SOFT(closer_quiet());
        u64 t2 = timer_now_ns();
        o->clunk_wait_ns = t1 - t0;
        o->quiet_wait_ns = t2 - t1;
        o->clunked = srv_clunked(&g_rec_a, w.fid);
        o->quiet   = closer_quiet();
        o->at      = closer_now();
        o->nclunk  = __atomic_load_n(&g_rec_a.nclunk, __ATOMIC_ACQUIRE);
        o->nmsg    = g_rec_a.nmsg;
    }
    o->nwrite    = __atomic_load_n(&g_rec_a.nwrite, __ATOMIC_ACQUIRE);
    o->write_len = g_rec_a.write_len;
    o->write_sum = g_rec_a.write_sum;
    if (o->opened && (o->nwrite != 1 || !o->clunked || !o->quiet)) {
        // The second look: up to ten budgets more for the write, the clunk and
        // a quiet pool. A closer that is only slow gets there; a lost one never.
        u64 t3 = timer_now_ns(), stop = t3 + 10ull * TEST_YIELD_BUDGET_NS;
        while (!(__atomic_load_n(&g_rec_a.nwrite, __ATOMIC_ACQUIRE) >= 1 &&
                 srv_clunked(&g_rec_a, w.fid) && closer_quiet()) &&
               timer_now_ns() < stop)
            sched();
        o->late_wait_ns = timer_now_ns() - t3;
        o->late_nwrite  = __atomic_load_n(&g_rec_a.nwrite, __ATOMIC_ACQUIRE);
        o->late_clunked = srv_clunked(&g_rec_a, w.fid);
        o->late_quiet   = closer_quiet();
        o->late         = closer_now();
    }
    o->self_tid = current_thread()->tid;
    o->nwire    = g_wire_n;
    for (u32 i = 0; i < g_wire_n && i < WIRE_LOG; i++) o->wire[i] = g_wire[i];
    wbc_end(&w);
}

static void xw_put(const char *k, u64 v) {
    uart_puts(" ");
    uart_puts(k);
    uart_puts("=");
    uart_putdec(v);
}

static void xw_diag(const struct xw_obs *o, int code) {
    uart_puts("    xw-diag code=");
    uart_putdec((u64)code);
    xw_put("nwrite", o->nwrite);       xw_put("write_len", o->write_len);
    xw_put("nclunk", o->nclunk);       xw_put("nmsg", o->nmsg);
    xw_put("clunked", o->clunked);     xw_put("quiet", o->quiet);
    xw_put("clunk_wait_ms", o->clunk_wait_ns / 1000000ull);
    xw_put("quiet_wait_ms", o->quiet_wait_ns / 1000000ull);
    xw_put("jobs+", o->at.jobs - o->base.jobs);
    xw_put("job_errors+", o->at.job_errors - o->base.job_errors);
    xw_put("dropped+", o->at.dropped - o->base.dropped);
    xw_put("refused+", o->at.refused - o->base.refused);
    xw_put("sent+", o->at.sent - o->base.sent);
    xw_put("threads", o->at.threads);  xw_put("idle", o->at.idle);
    xw_put("idle_parked", o->at.idle_parked);
    xw_put("retired", o->at.retired);  xw_put("runq", o->at.runq);
    xw_put("pending", o->at.pending);
    uart_puts("\n    xw-diag late:");
    xw_put("wait_ms", o->late_wait_ns / 1000000ull);
    xw_put("nwrite", o->late_nwrite);  xw_put("clunked", o->late_clunked);
    xw_put("quiet", o->late_quiet);
    xw_put("jobs+", o->late.jobs - o->base.jobs);
    xw_put("job_errors+", o->late.job_errors - o->base.job_errors);
    xw_put("dropped+", o->late.dropped - o->base.dropped);
    xw_put("pending", o->late.pending);
    uart_puts("\n    xw-diag wire:");
    xw_put("self_tid", (u64)o->self_tid);
    xw_put("child_pid", (u64)o->child_pid);
    xw_put("n", o->nwire);
    for (u32 i = 0; i < o->nwire && i < WIRE_LOG; i++) {
        const struct wire_ent *e = &o->wire[i];
        uart_puts("\n    xw-diag wire:");
        xw_put("type", e->type);  xw_put("tag", e->tag);
        xw_put("tid", (u64)e->tid); xw_put("pid", (u64)e->pid);
        if (e->type == P9_TWRITE) { xw_put("off", e->off); xw_put("len", e->len); }
    }
    uart_puts("\n");
}

// Static: two observations with their stats copies are too big for the stack
// the suite runs on.
static struct xw_obs g_xw_ex, g_xw_lo;

static void xw_expect(const struct xw_obs *o, int code) {
    TEST_ASSERT(o->opened, "a staged file on a closer session");
    TEST_ASSERT(o->started && o->installed, "a child holding it in its handle table");
    TEST_ASSERT(o->reaped, "the child was reaped");
    TEST_ASSERT(o->forced, "the kill forced the final close: it ended before the server answered");
    TEST_EXPECT_EQ((u64)o->before, 0ull, "nothing reached the server before the unstall");
    TEST_EXPECT_NE(o->status, WAIT_STATUS_SIGNALED(NOTE_SIG_KILL),
        "the kill replaced the exit's status");
    TEST_EXPECT_EQ(o->status, WAIT_STATUS_EXITED(code), "the exit's own status stands");
    if (o->nwrite != 1 || !o->clunked || !o->quiet) xw_diag(o, code);
    TEST_EXPECT_EQ((u64)o->nwrite, 1ull, "the closer wrote the run");
    TEST_EXPECT_EQ((u64)o->write_len, (u64)WBC_LEN, "all of it");
    TEST_EXPECT_EQ((u64)o->write_sum, (u64)wbc_sum(), "the bytes write() took");
}

void test_p9_closer_kill_in_final_close_keeps_exit(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct xw_obs *ex = &g_xw_ex, *lo = &g_xw_lo;
    xw_run(false, false, ex);
    xw_run(true,  false, lo);
    TEST_ASSERT(ex->slept && lo->slept,
        "premise: each close waited for its server until the kill");
    xw_expect(ex, 5);                            // exits_code(5)
    xw_expect(lo, 0);                            // the SYS_THREAD_EXIT convention
}

void test_p9_closer_kill_at_final_close_start_forces_it(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct xw_obs *ex = &g_xw_ex, *lo = &g_xw_lo;
    xw_run(false, true, ex);
    xw_run(true,  true, lo);
    TEST_ASSERT(ex->hook_ran && lo->hook_ran, "premise: each kill landed in the window");
    xw_expect(ex, 5);
    xw_expect(lo, 0);
}

// A Loom op drives the fid straight to the wire, past every write-behind path,
// so registering a dev9p Spoor flushes its staged run and stops it staging: a
// Loom FSYNC then reaches the server after the bytes write() took, and a later
// write() goes straight through. The control is the fixture: before the
// registration the same kind of write() staged, with nothing on the wire. The
// flush's own pages go too -- the ring's WRITEs would leave them stale -- and
// so does the buffer, which nothing can stage into again.
void test_p9_closer_loom_register_flushes_staged_run(void) {
    struct wbc w = {0};
    bool opened = wbc_open(&w);
    struct Loom *l = NULL;
    int  reg = -1, entered = -1, crc = -1;
    s32  cres = -1;
    u32  after_reg = 0, after_append = 0;
    u8   tail[16];
    for (u32 i = 0; i < sizeof(tail); i++) tail[i] = (u8)(0xA0u + i);
    u32  paged = ~0u;
    u64  owned = 0, budget_reg = 0;
    u8   page[WBC_LEN];
    struct srv_rec rec = {0}, rec_reg = {0};
    if (opened) {
        l = loom_create(8, 16, false);
        if (l) {
            spoor_ref(w.f);                       // the table adopts this one
            rights_t rt = RIGHT_READ | RIGHT_WRITE;
            u64 owned0 = w.a->client->larder.page_own_installs;
            reg = loom_register_handles(l, &w.f, &rt, 1);
            if (reg != 0) spoor_clunk(w.f);
            after_reg  = __atomic_load_n(&g_rec_a.nwrite, __ATOMIC_ACQUIRE);
            rec_reg    = g_rec_a;
            owned      = w.a->client->larder.page_own_installs - owned0;
            budget_reg = dev9p_wb_budget_used();
            u64 seq0  = 0;
            paged = larder_page_serve(&w.a->client->larder, w.f->qid.path, 0, 0,
                                      WBC_LEN, 0, page, &seq0);
            if (reg == 0) {
                struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
                struct loom_sqe *sqes = (struct loom_sqe *)(l->ring_kva + l->sqe_off);
                struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);
                u32 *sqa = (u32 *)(l->ring_kva + l->sq_array_off);
                for (u32 i = 0; i < sizeof(sqes[0]); i++) ((u8 *)&sqes[0])[i] = 0;
                sqes[0].opcode     = LOOM_OP_FSYNC;
                sqes[0].handle_idx = 0;
                sqes[0].user_data  = 0x10037u;
                sqa[0] = 0;
                __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);
                entered = loom_enter(l, /*to_submit=*/1, /*min_complete=*/1, 0);
                if (l->cq_tail >= 1) cres = cqes[0].result;
            }
            (void)dev9p.write(w.f, tail, sizeof(tail), WBC_LEN);
            after_append = __atomic_load_n(&g_rec_a.nwrite, __ATOMIC_ACQUIRE);
            loom_unref(l);
        }
        rec = g_rec_a;
        crc = spoor_clunk_rc(w.f);
    }
    wbc_end(&w);

    TEST_ASSERT(opened, "a staged file on a closer session");
    TEST_ASSERT(l != NULL, "loom_create");
    TEST_EXPECT_EQ((u64)(s64)reg, 0ull, "the registration succeeds");
    TEST_EXPECT_EQ((u64)after_reg, 1ull, "and flushed the staged run");
    TEST_EXPECT_EQ(rec_reg.write_off, 0ull, "from offset 0");
    TEST_EXPECT_EQ((u64)rec_reg.write_len, (u64)WBC_LEN, "all of it");
    TEST_EXPECT_EQ((u64)rec_reg.write_sum, (u64)wbc_sum(), "the bytes write() took");
    TEST_EXPECT_EQ(owned, 1ull, "the flush cached its page (control)");
    TEST_EXPECT_EQ((u64)paged, 0ull, "and the registration dropped it");
    TEST_EXPECT_EQ(budget_reg, w.budget0, "and gave back the staging buffer");
    TEST_EXPECT_EQ((u64)rec.write_off, (u64)WBC_LEN, "the last Twrite is the append");
    TEST_EXPECT_EQ((u64)entered, 1ull, "one SQE consumed");
    TEST_EXPECT_EQ((u64)(s64)cres, 0ull, "the Loom fsync succeeds");
    TEST_EXPECT_EQ((u64)rec.nfsync, 1ull, "the server saw one Tfsync");
    TEST_ASSERT(rec.fsync_at > rec_reg.nmsg && rec.fsync_at < rec.write_at,
                "after the run's Twrite and before the append's");
    TEST_EXPECT_EQ((u64)after_append, 2ull, "a write() after it goes straight through");
    TEST_EXPECT_EQ((u64)(s64)crc, 0ull, "the close reports nothing lost");
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
    g_sp_dying.proc = NULL;
    self_pump_body();
    // After a failed assertion the sender may still wait in A's recv: let it
    // go and reap it before its session goes. No assertion here (see above).
    if (g_stall.inited) stall_release(&g_stall);
    if (g_sp_dying.proc) {
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

// =============================================================================
// An unlock that never left for want of memory is sent again. A Tlock UNLCK
// whose reply buffer cannot be allocated never reaches the wire, and on a live
// session nothing would release the range before the fid's clunk: the aux
// join's audit round 3 F2, where the unlock a lock call owes when its
// descriptor was closed during the call kept the Proc's lock for its life
// while the call answered EBADF. The reply buffer is kmalloc(recv_cap), so a
// recv_cap past any order the buddy allocator serves makes every send fail
// until it is put back. A lock call's unlock (dev9p.lock, the re-check's path)
// and a last close's (dev9p.fd_close) wait until the server answers. An exit's
// close sends once and leaves the rest to the clunk. A closer's, for a close on
// a thread a death has reached, waits about a second, then says what it could
// not do. And a close's whole-file unlock first learns the file's identity
// (dev9p.lock_ident), which waits as the unlock does.
// =============================================================================

#define UO_STARVED ((size_t)1 << 40)

struct uo {
    struct p9_attached *a;
    struct Spoor       *root;
    struct Spoor       *f;
    u32                 fid;
    size_t              cap0;
};

// A leg whose sender never returned leaves its session behind rather than free
// what that sender reads, and every later leg refuses to start beside it.
static bool g_uo_abandoned;

static bool uo_open(struct uo *u) {
    if (g_uo_abandoned) return false;
    g_rec_a = (struct srv_rec){0};
    if (p9_mq_loopback_init(&g_mq_a, rec_responder, &g_rec_a) != 0) return false;
    u->a = closer_session(p9_mq_loopback_ops_for(&g_mq_a));
    if (!u->a) { p9_mq_loopback_destroy(&g_mq_a); return false; }
    u->root = p9_attached_root_spoor(u->a);
    if (u->root) {
        // The attach syscall's stamp, which the closer's hand-off needs.
        struct dev9p_priv *rp = (struct dev9p_priv *)u->root->aux;
        rp->attached_owner = u->a;
        p9_attached_ref(u->a);
        u->f = spoor_clone(u->root);
    }
    if (u->f) {
        const char *name = "victim";
        struct Walkqid *w = dev9p.walk(u->root, u->f, &name, 1);
        if (w) {
            walkqid_free(w);
            u->fid = ((struct dev9p_priv *)u->f->aux)->fid;
        } else {
            spoor_clunk(u->f);
            u->f = NULL;
        }
    }
    u->cap0 = u->a->client->recv_cap;
    return u->root && u->f;
}

static void uo_end(struct uo *u) {
    if (!u->a) return;
    if (u->f) spoor_clunk(u->f);
    if (u->root) spoor_clunk(u->root);
    p9_attached_unref(u->a);                     // the construction reference, last
    p9_mq_loopback_destroy(&g_mq_a);
}

// The leg's teardown, once nothing it started still runs.
static void uo_finish(struct uo *u, bool sender_gone) {
    if (sender_gone) uo_end(u);
    else             g_uo_abandoned = true;
}

static void uo_set_cap(struct p9_client *c, size_t cap) {
    spin_lock(&c->lock);
    c->recv_cap = cap;
    spin_unlock(&c->lock);
}

static u32 uo_resends(struct p9_client *c) {
    spin_lock(&c->lock);
    u32 n = c->unsent_resends;
    spin_unlock(&c->lock);
    return n;
}

static u32 uo_nlock(void)    { return __atomic_load_n(&g_rec_a.nlock, __ATOMIC_ACQUIRE); }
static u32 uo_ngetattr(void) { return __atomic_load_n(&g_rec_a.ngetattr, __ATOMIC_ACQUIRE); }

// A take the server grants, so that the open holds a lock its close must give up.
static int uo_take(struct uo *u) {
    return dev9p.lock(u->f, SYS_FILE_LOCK_WR, 0, 0, NULL);
}

enum uo_mode { UO_UNLOCK, UO_CLOSE, UO_IDENT };

static struct Spoor  *g_uo_spoor;
static volatile int   g_uo_rc;
static volatile bool  g_uo_done;
static volatile bool  g_uo_exited;
static enum uo_mode   g_uo_mode;

static void uo_entry(void) {
    int rc;
    switch (g_uo_mode) {
    case UO_CLOSE: rc = dev9p.fd_close(g_uo_spoor, /*heard=*/true);              break;
    case UO_IDENT: rc = dev9p.lock_ident(g_uo_spoor);                            break;
    default:       rc = dev9p.lock(g_uo_spoor, SYS_FILE_LOCK_UN, 0, 0, NULL);  break;
    }
    g_uo_rc = rc;
    __atomic_store_n(&g_uo_done, true, __ATOMIC_RELEASE);
    test_kthread_park_terminal(&g_uo_exited);
}

// The sender runs on a kernel thread while every send is refused. It must still
// be waiting after two sends were refused, with nothing on the wire; once the
// buffer can be had, it sends, and the server sees it once.
struct uo_out {
    bool thread, waited, joined;
    u32  nlock_starved, nlock_end, ngetattr_starved, ngetattr_end, resends;
    u8   type_end;
    int  rc;
};

static void uo_starve_and_feed(struct uo *u, enum uo_mode mode, struct uo_out *o) {
    struct p9_client *c = u->a->client;
    u32 nlock0 = uo_nlock(), ga0 = uo_ngetattr();
    g_uo_spoor  = u->f;
    g_uo_rc     = 1;
    g_uo_done   = false;
    g_uo_exited = false;
    g_uo_mode   = mode;
    u32 r0 = uo_resends(c);
    uo_set_cap(c, UO_STARVED);
    struct Thread *t = thread_create(kproc(), uo_entry);
    o->thread = t != NULL;
    if (t) {
        ready(t);
        TEST_YIELD_UNTIL_SOFT(uo_resends(c) >= r0 + 2 ||
                              __atomic_load_n(&g_uo_done, __ATOMIC_ACQUIRE));
    }
    o->nlock_starved    = uo_nlock() - nlock0;
    o->ngetattr_starved = uo_ngetattr() - ga0;
    o->waited = t && uo_resends(c) >= r0 + 2 &&
                !__atomic_load_n(&g_uo_done, __ATOMIC_ACQUIRE);
    uo_set_cap(c, u->cap0);
    if (t) {
        TEST_YIELD_UNTIL_SOFT(__atomic_load_n(&g_uo_done, __ATOMIC_ACQUIRE));
        if (__atomic_load_n(&g_uo_done, __ATOMIC_ACQUIRE)) {
            test_kthread_join_free(t, &g_uo_exited);
            o->joined = true;
        }
    }
    o->nlock_end    = uo_nlock() - nlock0;
    o->ngetattr_end = uo_ngetattr() - ga0;
    o->type_end     = g_rec_a.lock_type;
    o->resends      = uo_resends(c) - r0;
    o->rc           = g_uo_rc;
}

void test_p9_closer_unlock_waits_out_a_reply_oom(void) {
    struct uo u = {0};
    bool opened = uo_open(&u);
    int  prc = 1;
    u32  ctl_nlock = 0;
    bool ctl_bound = false;
    struct uo_out o = {0};
    if (opened) {
        // CONTROL: the starvation reaches the wire's send. A plain Tlock UNLCK
        // fails without the server seeing it, and the fid stays bound.
        struct p9_client *c = u.a->client;
        uo_set_cap(c, UO_STARVED);
        u8 st = P9_LOCK_ERROR;
        prc = p9_client_lock(c, u.fid, P9_LOCK_TYPE_UNLCK, 0, 0, 0, NULL, 0, &st);
        uo_set_cap(c, u.cap0);
        ctl_nlock = uo_nlock();
        ctl_bound = p9_session_fid_bound(&c->session, u.fid);

        uo_starve_and_feed(&u, UO_UNLOCK, &o);
    }
    uo_finish(&u, !o.thread || o.joined);

    TEST_ASSERT(opened, "a walked file on its own session");
    TEST_EXPECT_EQ((u64)(s64)prc, (u64)(s64)-P9_E_IO,
                   "CONTROL: an unlock with no reply buffer fails");
    TEST_EXPECT_EQ(ctl_nlock, 0u, "CONTROL: and never reaches the server");
    TEST_ASSERT(ctl_bound, "CONTROL: the fid stays bound, so the server holds its locks");
    TEST_ASSERT(o.thread, "a kernel thread");
    TEST_ASSERT(o.waited, "dev9p's unlock waits, sending again, while no buffer can be had");
    TEST_ASSERT(o.joined, "and returns once it can be had");
    TEST_EXPECT_EQ(o.nlock_starved, 0u, "nothing reached the server meanwhile");
    TEST_EXPECT_EQ((u64)(s64)o.rc, 0, "then the unlock is answered");
    TEST_EXPECT_EQ(o.nlock_end, 1u, "one Tlock reached the server");
    TEST_EXPECT_EQ((u32)o.type_end, (u32)P9_LOCK_TYPE_UNLCK, "and it unlocks");
}

void test_p9_closer_close_unlock_waits_out_a_reply_oom(void) {
    struct uo u = {0};
    bool opened = uo_open(&u);
    int  trc = 1;
    bool held = false, held_after = true;
    struct uo_out o = {0};
    if (opened) {
        trc = uo_take(&u);
        held = dev9p.lock_held(u.f);
        uo_starve_and_feed(&u, UO_CLOSE, &o);
        held_after = dev9p.lock_held(u.f);
    }
    uo_finish(&u, !o.thread || o.joined);

    TEST_ASSERT(opened, "a walked file on its own session");
    TEST_EXPECT_EQ((u64)(s64)trc, 0, "the take is granted");
    TEST_ASSERT(held, "CONTROL: the open holds a lock its close must give up");
    TEST_ASSERT(o.thread, "a kernel thread");
    TEST_ASSERT(o.waited, "the last close waits, sending again, while no buffer can be had");
    TEST_ASSERT(o.joined, "and returns once it can be had");
    TEST_EXPECT_EQ(o.nlock_starved, 0u, "nothing reached the server meanwhile");
    TEST_EXPECT_EQ((u64)(s64)o.rc, 0, "then the close returns");
    TEST_EXPECT_EQ(o.nlock_end, 1u, "one Tlock reached the server");
    TEST_EXPECT_EQ((u32)o.type_end, (u32)P9_LOCK_TYPE_UNLCK, "and it unlocks");
    TEST_ASSERT(!held_after, "the open holds nothing after its close");
}

// An exit's close (exit_close_active, as proc_close_handles_at_exit sets it)
// may not wait for memory: its Proc's is freed at reap, after the close. It
// sends once and returns. The unlock it could not send stays owed, and with
// nothing else holding the open, the clunk that follows releases it.
static struct test_dying g_uo_dying;
static volatile int      g_uo_dying_rc;

static void uo_exit_close(void *arg) {
    struct Thread *self = current_thread();
    self->exit_close_active = true;
    g_uo_dying_rc = dev9p.fd_close((struct Spoor *)arg, /*heard=*/false);
    self->exit_close_active = false;
}

void test_p9_closer_exit_close_unlock_sends_once(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    struct uo u = {0};
    bool opened = uo_open(&u);
    int  trc = 1;
    bool started = false, returned = false, reaped = false;
    bool owed = false, clunked = false, quiet = false;
    u32  nlock0 = 0, nlock_starved = 1, nlock_end = 1, resends = 1;
    struct p9_closer_stats st = base;
    if (opened) {
        struct p9_client *c = u.a->client;
        trc = uo_take(&u);
        nlock0 = uo_nlock();
        u32 r0 = uo_resends(c);
        g_uo_dying_rc = 1;
        uo_set_cap(c, UO_STARVED);
        started = test_dying_start(&g_uo_dying, uo_exit_close, u.f, /*dead_now=*/false);
        if (started) TEST_YIELD_UNTIL_SOFT(test_dying_done(&g_uo_dying));
        returned      = started && test_dying_done(&g_uo_dying);
        resends       = uo_resends(c) - r0;
        nlock_starved = uo_nlock() - nlock0;
        owed          = dev9p.lock_held(u.f);
        uo_set_cap(c, u.cap0);
        if (started && !returned) TEST_YIELD_UNTIL_SOFT(test_dying_done(&g_uo_dying));
        if (started && test_dying_done(&g_uo_dying)) {
            test_dying_reap(&g_uo_dying);
            reaped = true;
            // The open's last reference: its clunk releases what the fid holds.
            spoor_clunk(u.f);
            u.f = NULL;
            TEST_YIELD_UNTIL_SOFT(srv_clunked(&g_rec_a, u.fid));
            TEST_YIELD_UNTIL_SOFT(closer_quiet());
        }
        clunked   = srv_clunked(&g_rec_a, u.fid);
        quiet     = closer_quiet();
        nlock_end = uo_nlock() - nlock0;
        st        = closer_now();
    }
    uo_finish(&u, !started || reaped);

    TEST_ASSERT(opened, "a walked file on its own session");
    TEST_EXPECT_EQ((u64)(s64)trc, 0, "the take is granted");
    TEST_ASSERT(started, "a thread in its exit's close");
    TEST_ASSERT(returned, "the exit's close returns while no buffer can be had");
    TEST_EXPECT_EQ((u64)(s64)g_uo_dying_rc, 0, "and reports nothing");
    TEST_EXPECT_EQ(resends, 0u, "its unlock was sent once, never again");
    TEST_EXPECT_EQ(nlock_starved, 0u, "and never reached the server");
    TEST_ASSERT(owed, "the unlock stays owed");
    TEST_ASSERT(clunked, "the last reference's clunk reaches the server");
    TEST_EXPECT_EQ(nlock_end, 0u, "and it is all the server saw: the clunk releases the locks");
    TEST_EXPECT_EQ(st.jobs, base.jobs, "no close job: nothing else held the open");
    TEST_EXPECT_EQ(st.unlocks_lost, base.unlocks_lost, "nothing was reported lost");
    TEST_ASSERT(quiet, "the pool is quiet again");
}

// A refusal is the server's answer, never an unlock that never left: an exit's
// close owes nothing after one, whatever the ecode -- EAGAIN, which a frame that
// never left once shared, and EINTR, which a caught note's flush shares, though
// a close has no note to take.
void test_p9_closer_exit_close_refused_unlock_is_answered(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    static const u32 ecodes[2] = { T_E_AGAIN, T_E_INTR };
    struct uo u = {0};
    bool opened = uo_open(&u);
    int  trc[2] = { 1, 1 };
    bool started[2] = { false, false }, reaped[2] = { false, false };
    bool owed[2] = { true, true };
    u32  nlock[2] = { 0, 0 };
    if (opened) {
        for (int i = 0; i < 2; i++) {
            trc[i] = uo_take(&u);
            u32 nlock0 = uo_nlock();
            g_rec_a.fail_unlock_ecode = ecodes[i];
            g_rec_a.fail_unlocks      = 1;
            g_uo_dying_rc = 1;
            started[i] = test_dying_start(&g_uo_dying, uo_exit_close, u.f, /*dead_now=*/false);
            if (started[i]) {
                TEST_YIELD_UNTIL_SOFT(test_dying_done(&g_uo_dying));
                if (test_dying_done(&g_uo_dying)) {
                    test_dying_reap(&g_uo_dying);
                    reaped[i] = true;
                }
            }
            nlock[i] = uo_nlock() - nlock0;
            owed[i]  = dev9p.lock_held(u.f);
            g_rec_a.fail_unlocks = 0;
            if (!reaped[i]) break;
        }
    }
    TEST_YIELD_UNTIL_SOFT(closer_quiet());
    bool quiet = closer_quiet();
    struct p9_closer_stats st = closer_now();
    uo_finish(&u, !started[0] || (reaped[0] && (!started[1] || reaped[1])));

    // Each round says which it is, so a red names the ecode without a reading of
    // the code.
    static const char *const take_msg[2] = {
        "EAGAIN round: the take is granted", "EINTR round: the take is granted" };
    static const char *const ret_msg[2] = {
        "EAGAIN round: the exit's close returns", "EINTR round: the exit's close returns" };
    static const char *const sent_msg[2] = {
        "EAGAIN round: its one unlock reached the server, which refused it",
        "EINTR round: its one unlock reached the server, which refused it" };
    static const char *const owed_msg[2] = {
        "EAGAIN round: a refusal is an answer, nothing stays owed",
        "EINTR round: a refusal is an answer, nothing stays owed" };
    TEST_ASSERT(opened, "a walked file on its own session");
    for (int i = 0; i < 2; i++) {
        TEST_EXPECT_EQ((u64)(s64)trc[i], 0, take_msg[i]);
        TEST_ASSERT(started[i] && reaped[i], ret_msg[i]);
        TEST_EXPECT_EQ(nlock[i], 1u, sent_msg[i]);
        TEST_ASSERT(!owed[i], owed_msg[i]);
    }
    TEST_EXPECT_EQ(st.jobs, base.jobs, "no close job");
    TEST_EXPECT_EQ(st.unlocks_lost, base.unlocks_lost, "nothing was reported lost");
    TEST_ASSERT(quiet, "the pool is quiet again");
}

// A death reaches the closing thread, and another reference keeps the open
// alive: its close hands the unlock to a closer, whose send is refused the same
// way. The closer waits too, about a second, as for its own sends.
static void uo_dying_close(void *arg) {
    g_uo_dying_rc = dev9p.fd_close((struct Spoor *)arg, /*heard=*/false);
}

void test_p9_closer_closer_unlock_waits_out_a_reply_oom(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    struct uo u = {0};
    bool opened = uo_open(&u);
    int  trc = 1;
    bool started = false, waited = false, quiet = false, held_after = true;
    u32  r0 = 0, nlock0 = 0, nlock_starved = 0, nlock_end = 0;
    u8   type_end = 0;
    struct p9_closer_stats st = base;
    if (opened) {
        struct p9_client *c = u.a->client;
        trc = uo_take(&u);
        nlock0 = uo_nlock();
        r0 = uo_resends(c);
        spoor_ref(u.f);                          // the reference that keeps it alive
        g_uo_dying_rc = 1;
        uo_set_cap(c, UO_STARVED);
        started = test_dying_start(&g_uo_dying, uo_dying_close, u.f, /*dead_now=*/true);
        if (started) {
            TEST_YIELD_UNTIL_SOFT(test_dying_done(&g_uo_dying));
            TEST_YIELD_UNTIL_SOFT(uo_resends(c) >= r0 + 2);
        }
        nlock_starved = uo_nlock() - nlock0;
        waited = started && test_dying_done(&g_uo_dying) && uo_resends(c) >= r0 + 2;
        uo_set_cap(c, u.cap0);
        if (started) {
            TEST_YIELD_UNTIL_SOFT(uo_nlock() > nlock0);
            TEST_YIELD_UNTIL_SOFT(closer_quiet());
            test_dying_reap(&g_uo_dying);
        }
        quiet      = closer_quiet();
        nlock_end  = uo_nlock() - nlock0;
        type_end   = g_rec_a.lock_type;
        held_after = dev9p.lock_held(u.f);
        st = closer_now();
        spoor_clunk(u.f);                        // the extra reference
    }
    uo_finish(&u, quiet);

    TEST_ASSERT(opened, "a walked file on its own session");
    TEST_EXPECT_EQ((u64)(s64)trc, 0, "the take is granted");
    TEST_ASSERT(started, "a dying thread");
    TEST_EXPECT_EQ((u64)(s64)g_uo_dying_rc, 0, "its close returns at once");
    TEST_ASSERT(waited, "the closer waits, sending again, while no buffer can be had");
    TEST_EXPECT_EQ(nlock_starved, 0u, "nothing reached the server meanwhile");
    TEST_EXPECT_EQ(nlock_end, 1u, "then the closer's Tlock reached the server");
    TEST_EXPECT_EQ((u32)type_end, (u32)P9_LOCK_TYPE_UNLCK, "and it unlocks");
    TEST_ASSERT(!held_after, "the open holds nothing");
    TEST_EXPECT_EQ(st.jobs, base.jobs + 1, "one close job ran");
    TEST_EXPECT_EQ(st.unlocks_lost, base.unlocks_lost, "nothing was reported lost");
    TEST_ASSERT(quiet, "the pool is quiet again");
}

// The same, but the shortage outlasts the closer's patience: it gives up after
// P9_RESEND_CLOSER sends and says so (`9p: close: unlock of fid`, counted). The
// open keeps the lock until its last reference clunks the fid.
void test_p9_closer_closer_unlock_gives_up_loudly(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    struct uo u = {0};
    bool opened = uo_open(&u);
    int  trc = 1;
    bool started = false, quiet = false, owed = false;
    u32  nlock0 = 0, nlock_end = 1, resends = 0;
    struct p9_closer_stats st = base;
    if (opened) {
        struct p9_client *c = u.a->client;
        trc = uo_take(&u);
        nlock0 = uo_nlock();
        u32 r0 = uo_resends(c);
        spoor_ref(u.f);                          // the reference that keeps it alive
        g_uo_dying_rc = 1;
        p9_close_flush_quiet_for_test(true);     // the line this leg makes on purpose
        uo_set_cap(c, UO_STARVED);
        started = test_dying_start(&g_uo_dying, uo_dying_close, u.f, /*dead_now=*/true);
        if (started) {
            TEST_YIELD_UNTIL_SOFT(test_dying_done(&g_uo_dying));
            // About a second of waits: two yield budgets cover it.
            TEST_YIELD_UNTIL_SOFT(closer_now().unlocks_lost > base.unlocks_lost);
            TEST_YIELD_UNTIL_SOFT(closer_now().unlocks_lost > base.unlocks_lost);
            TEST_YIELD_UNTIL_SOFT(closer_quiet());
            test_dying_reap(&g_uo_dying);
        }
        quiet     = closer_quiet();
        resends   = uo_resends(c) - r0;
        nlock_end = uo_nlock() - nlock0;
        owed      = dev9p.lock_held(u.f);
        st        = closer_now();
        uo_set_cap(c, u.cap0);
        p9_close_flush_quiet_for_test(false);
        spoor_clunk(u.f);                        // the extra reference
    }
    uo_finish(&u, quiet);

    TEST_ASSERT(opened, "a walked file on its own session");
    TEST_EXPECT_EQ((u64)(s64)trc, 0, "the take is granted");
    TEST_ASSERT(started, "a dying thread");
    TEST_EXPECT_EQ((u64)(s64)g_uo_dying_rc, 0, "its close returns at once");
    TEST_EXPECT_EQ(st.unlocks_lost, base.unlocks_lost + 1, "the closer gives up and says so");
    TEST_EXPECT_EQ(resends, P9_RESEND_CLOSER, "after its bound of sends again");
    TEST_EXPECT_EQ(nlock_end, 0u, "none of which reached the server");
    TEST_ASSERT(owed, "the open still holds the lock, for its last reference's clunk");
    TEST_EXPECT_EQ(st.jobs, base.jobs + 1, "one close job ran");
    TEST_ASSERT(quiet, "the pool is quiet again");
}

// A closer's unlock that the server refuses is answered, not lost: Stratum
// answers ESTALE for a file unlinked or replaced under the lock, whose records
// went with it. No line, no count, and the grant is gone.
void test_p9_closer_closer_refused_unlock_is_answered(void) {
    TEST_ASSERT(closer_quiet(), "the pool is quiet at entry");
    struct p9_closer_stats base = closer_now();
    struct uo u = {0};
    bool opened = uo_open(&u);
    int  trc = 1;
    bool started = false, quiet = false, held_after = true;
    u32  nlock0 = 0, nlock_end = 0;
    struct p9_closer_stats st = base;
    if (opened) {
        trc = uo_take(&u);
        nlock0 = uo_nlock();
        spoor_ref(u.f);                          // the reference that keeps it alive
        g_uo_dying_rc = 1;
        g_rec_a.fail_unlock_ecode = T_E_STALE;
        g_rec_a.fail_unlocks      = 1;
        started = test_dying_start(&g_uo_dying, uo_dying_close, u.f, /*dead_now=*/true);
        if (started) {
            TEST_YIELD_UNTIL_SOFT(test_dying_done(&g_uo_dying));
            TEST_YIELD_UNTIL_SOFT(uo_nlock() > nlock0 && closer_quiet());
            test_dying_reap(&g_uo_dying);
        }
        quiet      = closer_quiet();
        nlock_end  = uo_nlock() - nlock0;
        held_after = dev9p.lock_held(u.f);
        st         = closer_now();
        g_rec_a.fail_unlocks = 0;
        spoor_clunk(u.f);                        // the extra reference
    }
    uo_finish(&u, quiet);

    TEST_ASSERT(opened, "a walked file on its own session");
    TEST_EXPECT_EQ((u64)(s64)trc, 0, "the take is granted");
    TEST_ASSERT(started, "a dying thread");
    TEST_EXPECT_EQ((u64)(s64)g_uo_dying_rc, 0, "its close returns at once");
    TEST_EXPECT_EQ(nlock_end, 1u, "the closer's one unlock reached the server, which refused it");
    TEST_EXPECT_EQ(st.unlocks_lost, base.unlocks_lost, "a refusal is no loss: nothing reported");
    TEST_ASSERT(!held_after, "and the grant is gone");
    TEST_EXPECT_EQ(st.jobs, base.jobs + 1, "one close job ran");
    TEST_ASSERT(quiet, "the pool is quiet again");
}

// A close's whole-file unlock through a Proc's lock owner first learns the
// identity of the file closed (handle.c, lock_owners_on_close). A Tgetattr lost
// to the same shortage would lose that unlock one round trip earlier, so it is
// sent again too.
void test_p9_closer_lock_ident_waits_out_a_reply_oom(void) {
    struct uo u = {0};
    bool opened = uo_open(&u);
    int  prc = 1;
    u32  ctl_ga = 1;
    u8   ident0 = 0xff, ident_end = DEV9P_IDENT_UNKNOWN;
    struct uo_out o = {0};
    if (opened) {
        struct p9_client *c = u.a->client;
        struct dev9p_priv *fp = (struct dev9p_priv *)u.f->aux;
        ident0 = __atomic_load_n(&fp->lock_ident, __ATOMIC_ACQUIRE);
        // CONTROL: the starvation reaches a Tgetattr's send.
        struct p9_attr at;
        u32 g0 = uo_ngetattr();
        uo_set_cap(c, UO_STARVED);
        prc = p9_client_getattr(c, u.fid, P9_GETATTR_GEN, &at);
        uo_set_cap(c, u.cap0);
        ctl_ga = uo_ngetattr() - g0;

        uo_starve_and_feed(&u, UO_IDENT, &o);
        ident_end = __atomic_load_n(&fp->lock_ident, __ATOMIC_ACQUIRE);
    }
    uo_finish(&u, !o.thread || o.joined);

    TEST_ASSERT(opened, "a walked file on its own session");
    TEST_EXPECT_EQ((u32)ident0, (u32)DEV9P_IDENT_UNKNOWN,
                   "CONTROL: the walked file's identity is not known yet");
    TEST_EXPECT_EQ((u64)(s64)prc, (u64)(s64)-P9_E_IO,
                   "CONTROL: a Tgetattr with no reply buffer fails");
    TEST_EXPECT_EQ(ctl_ga, 0u, "CONTROL: and never reaches the server");
    TEST_ASSERT(o.thread, "a kernel thread");
    TEST_ASSERT(o.waited, "the identity learn waits, sending again, while no buffer can be had");
    TEST_ASSERT(o.joined, "and returns once it can be had");
    TEST_EXPECT_EQ(o.ngetattr_starved, 0u, "nothing reached the server meanwhile");
    TEST_EXPECT_EQ((u64)(s64)o.rc, 0, "then it is answered");
    TEST_EXPECT_EQ(o.ngetattr_end, 1u, "one Tgetattr reached the server");
    TEST_ASSERT(ident_end != DEV9P_IDENT_UNKNOWN, "and the file's identity is known");
}

// The wait ends when the session dies, with no buffer ever had: the server drops
// the fid's locks with the session, and nothing could send the unlock now.
void test_p9_closer_unlock_ends_when_its_session_dies(void) {
    struct uo u = {0};
    bool opened = uo_open(&u);
    bool thread = false, waited = false, returned = false;
    u32  nlock_end = 1;
    int  rc = 1;
    if (opened) {
        struct p9_client *c = u.a->client;
        g_uo_spoor  = u.f;
        g_uo_rc     = 1;
        g_uo_done   = false;
        g_uo_exited = false;
        g_uo_mode   = UO_UNLOCK;
        u32 r0 = uo_resends(c);
        uo_set_cap(c, UO_STARVED);
        struct Thread *t = thread_create(kproc(), uo_entry);
        thread = t != NULL;
        if (t) {
            ready(t);
            TEST_YIELD_UNTIL_SOFT(uo_resends(c) >= r0 + 2 ||
                                  __atomic_load_n(&g_uo_done, __ATOMIC_ACQUIRE));
            waited = uo_resends(c) >= r0 + 2 &&
                     !__atomic_load_n(&g_uo_done, __ATOMIC_ACQUIRE);
            p9_client_mark_devgone(c);
            TEST_YIELD_UNTIL_SOFT(__atomic_load_n(&g_uo_done, __ATOMIC_ACQUIRE));
            returned = __atomic_load_n(&g_uo_done, __ATOMIC_ACQUIRE);
            if (returned) test_kthread_join_free(t, &g_uo_exited);
        }
        rc        = g_uo_rc;
        nlock_end = uo_nlock();
        uo_set_cap(c, u.cap0);
    }
    uo_finish(&u, !thread || returned);

    TEST_ASSERT(opened, "a walked file on its own session");
    TEST_ASSERT(thread, "a kernel thread");
    TEST_ASSERT(waited, "CONTROL: the unlock is waiting, sending again, when the session dies");
    TEST_ASSERT(returned, "the session's death ends the wait");
    TEST_EXPECT_EQ((u64)(s64)rc, (u64)(s64)-T_E_IO, "the unlock answers EIO");
    TEST_EXPECT_EQ(nlock_end, 0u, "nothing reached the server");
}
