// dev9p_poll -- dev9p's remote-readiness bridge (net-6b-2b; the #98 SAMPLE/ARM
// split; NET-DESIGN section 12.2, ARCH 23.3, specs/net_poll.tla +
// specs/net_poll_teardown.tla).
//
// A netd readiness file (/net/<proto>/N/ready) or a ptyfs `<n>ready` file carries
// QTPOLL on its qid: its server answers reads whose OFFSET carries the poll event
// mask (9p_wire.h P9_POLL_*). Two reads, two jobs, never one read for both:
//
//   SNAPSHOT  offset = mask | P9_POLL_SNAPSHOT. The server answers at once with
//             the revents now, 0 included. The poll core's SAMPLE
//             (dev9p_poll_snapshot; net_poll.tla Scan / SnapshotReply).
//   ARM       offset = mask. The server holds the read until the file is ready
//             for the mask, evaluating it on arrival, so readiness that rose
//             after the snapshot is answered at once. Sent only when the call
//             will park, after the poller's hook is on the list (dev9p_poll_arm;
//             PollerArm). Its answer is a WAKE, never read as readiness: the
//             woken poller samples again.
//
// Nothing synchronous waits on either reply, so a boot-spawned kthread drives the
// 9P elected reader (#841) for every client with a read out (the cons_poll
// console_mgr + Loom-4 SQPOLL analog; net_poll.tla KthreadWalk):
//   - reap a terminal ARM: walk its poll-state's hook list in process context,
//     then free it;
//   - collect a STRANDED arm -- non-terminal, and no hook on its list, because
//     every poller that wanted it has moved on -- and flush it in the same
//     locked step that unlinks it (GcArm; net_poll_teardown BUGGY_SPLIT_GC). A
//     SNAPSHOT is never collected (BUGGY_GC_SNAPSHOT): it has no hook by design,
//     and the poller that sent it releases it itself;
//   - pump the reader of every distinct client with an arm or a snapshot out
//     (F1: one client's parked arm must not starve another client's reply);
//   - park when there is none.
//
// A SHORTAGE IS NOT AN ANSWER (NP-4a). p9_client_submit_async reports no free tag
// or a full send ring as -P9_E_AGAIN and fires the completion with it; both
// completions here then leave everything alone, because the read never left the
// kernel and is its submitter's again. A snapshot waits UNSENT for the poll core
// to resend it; an arm is freed, and the core bounds its park by the retry timer.
//
// LOCK ORDER (acyclic):
//   g_dev9p_poll_lock -> c->lock           (the arm submit; the collector's flush)
//   g_dev9p_poll_lock -> poll_list lock     (the GC's empty-check, nested so it
//                                            is atomic with the unlink vs a reuse)
//   poll_list lock    -> g_timerwait -> rendez -> cpu_sched   (a walk's wakes)
//   c->lock           -> rendez             (a completion wakes the kthread or a
//                                            poller)
// g_dev9p_poll_lock is never held across a wakeup, a pump, a snapshot submit, or
// an unref. The one abandon under it is the collector's, which must be (below).

#include <thylacine/dev9p.h>

#include <thylacine/9p_attach.h>
#include <thylacine/9p_client.h>
#include <thylacine/9p_session.h>
#include <thylacine/9p_wire.h>
#include <thylacine/poll.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../mm/slub.h"
#include "../arch/arm64/timer.h"   // timer_now_ns -- the reader-pump idle deadline

// =============================================================================
// State.
// =============================================================================

// One ARM in flight. `rpc` at OFFSET 0 so the completion recovers the container
// with a single cast (the audited Loom offset-0 idiom). #294 cancel-at-close: an
// arm does NOT pin the readiness Spoor (that would defer dev9p_close past the
// user's fd-close -- the permanent-slot-leak root). It holds instead (a) a
// poll-state ref (ps->refs; keeps op->ps deref-safe) + (b) a session ref
// (p9_attached_ref on attached_owner; keeps op->client alive -- the kthread
// borrows the client, never owning the lifetime). So the arm survives
// independent of the user's fd, and dev9p_close runs AT fd-close, cancels it, and
// delivers the `ready`-fd Tclunk deterministically (specs/net_poll_teardown.tla,
// Fix=TRUE). `attached_owner` is NULL only on the test path (dev9p_attach_client
// -- the client is externally owned). An arm is linked into the registry only
// once it is on the wire; `terminal` is set by its completion and read by the
// kthread's reap.
struct dev9p_poll_op {
    struct p9_rpc            rpc;       // OFFSET 0 -- the completion casts (op *)rpc
    struct dev9p_poll_state *ps;        // the poll-state whose list it wakes (holds a ps ref)
    struct p9_attached      *attached_owner; // session ref (=> client alive); NULL = test path
    struct p9_client        *client;    // borrowed (valid while the session ref / ext-owner holds)
    u32                      fid;        // the readiness file's 9P fid
    u16                      mask;       // the events it covers = its Tread offset
    bool                     terminal;   // answered (atomic; reaped by the kthread)
    struct dev9p_poll_op    *next;       // g_dev9p_poll_ops chain (under g_dev9p_poll_lock)
};

_Static_assert(__builtin_offsetof(struct dev9p_poll_op, rpc) == 0,
               "p9_rpc must be at offset 0 -- the arm completion recovers the container");

// One SNAPSHOT: the request behind a poll core slot (`s->op`). Lives from the
// first send attempt to the core's release, always inside one pass of one poll
// call, whose held Spoor ref keeps the priv -- and so the session -- alive
// throughout; the snapshot's own session ref is for the kthread's borrow. `s` is
// on the poller's stack: only the completion writes it, and the release is the
// c->lock barrier after which no completion can. `live` = linked and not yet
// answered: counted in g_dev9p_poll_snap_live so the kthread pumps its client --
// even while it waits UNSENT, because the reply that frees a tag (an Rflush) has
// to be read by someone.
struct dev9p_poll_snap {
    struct p9_rpc            rpc;       // OFFSET 0 -- the completion casts (snap *)rpc
    struct poll_snap        *s;         // the poller's slot (its stack); until the release
    struct p9_attached      *attached_owner; // session ref; NULL = test path
    struct p9_client        *client;    // borrowed, as for an arm
    u32                      fid;
    u16                      mask;      // the requested events (the offset's low bits)
    bool                     live;      // atomic; exactly one of complete/release clears it
    struct dev9p_poll_snap  *next;      // g_dev9p_poll_snaps chain (under g_dev9p_poll_lock)
};

_Static_assert(__builtin_offsetof(struct dev9p_poll_snap, rpc) == 0,
               "p9_rpc must be at offset 0 -- the snapshot completion recovers the container");

// Per-readiness-Spoor poll state: the list the arm's answer walks. Lazily
// allocated by the first arm of a QTPOLL Spoor; hung off dev9p_priv->poll. #294:
// independently REFCOUNTED -- the priv holds one ref (dropped via
// dev9p_poll_priv_release at dev9p_close) and each outstanding arm holds one;
// freed when both drop, so dev9p_close can free the priv + clunk the fid while an
// arm the kthread still owns keeps ps alive (specs/net_poll_teardown.tla
// NoUseAfterFreePs). Multi-thread-Proc-reachable (handle_dup shares the Spoor ->
// the same priv -> the same poll-state): poll_list has its own lock; refs is
// atomic; `op` is under g_dev9p_poll_lock.
struct dev9p_poll_state {
    struct poll_waiter_list  poll_list;     // pollers' hooks (own lock)
    struct dev9p_poll_op    *op;            // the newest arm (under g_lock); NULL = none
    int                      refs;          // atomic: priv (1) + 1 per outstanding arm
};

#define DEV9P_POLL_IDLE_NS  (20ull * 1000ull * 1000ull)   // 20ms reader-pump idle deadline
// Distinct QTPOLL clients the kthread pumps per cycle (F1: the global pump must be
// FAIR across clients -- pumping only one would starve a second client's pending
// reply). v1.0 has TWO QTPOLL clients (the netd /net mount + the ptyfs /dev/pts
// mount, item 10); the per-user-netd v1.x config a handful. With MORE than this many
// simultaneous QTPOLL clients each holding a perpetually-parked arm, the cap is NOT
// graceful: the registries are LIFO and the collect always walks from the head with
// no rotation, so the >MAX clients nearest the head are pumped every cycle and a
// TAIL client is STARVED outright (its reply never demuxed -> its pollers hang), not
// merely delayed (R2-F1). The v1.x close (the per-client work-queue / a fair
// round-robin cursor) must use a fair start, not this head-anchored scan.
// Unreachable below the cap, far above the realistic per-user-netd count.
#define DEV9P_POLL_MAX_PUMP 16

static spin_lock_t              g_dev9p_poll_lock;
static struct dev9p_poll_op    *g_dev9p_poll_ops;        // the arm registry (under g_lock)
static u32                      g_dev9p_poll_op_count;    // its length (atomic; the park cond)
static struct dev9p_poll_snap  *g_dev9p_poll_snaps;      // snapshots in flight (under g_lock)
static u32                      g_dev9p_poll_snap_live;   // live snapshots (atomic; the park cond)
static struct Rendez            g_dev9p_poll_rendez;      // the kthread park
static bool                     g_dev9p_poll_inited;
static int                      g_dev9p_poll_test_gc;     // atomic; see dev9p_poll_test_gc
static bool                     g_dev9p_poll_test_held;   // atomic

void dev9p_poll_init(void) {
    if (g_dev9p_poll_inited) return;
    spin_lock_init(&g_dev9p_poll_lock);
    rendez_init(&g_dev9p_poll_rendez);
    g_dev9p_poll_ops = NULL;
    g_dev9p_poll_op_count = 0;
    g_dev9p_poll_snaps = NULL;
    g_dev9p_poll_snap_live = 0;
    g_dev9p_poll_inited = true;
}

// Whether c's readiness lives in its server. A regular dev9p file (no QTPOLL on
// its cached qid) is POSIX always-ready -- a regular file is never read by
// poll(). The kthread drives the reads with a frame-boundary recv DEADLINE (so a
// held arm lets it re-check its work, never blocking forever), which REQUIRES a
// deadline-capable transport; netd's and ptyfs's mounts are srvconn, so this
// holds in v1.0, and a hypothetical QTPOLL server without one degrades to
// always-ready (fail-safe -- the gate the Loom SQPOLL register applies too).
static bool dev9p_poll_is_remote(struct Spoor *c, struct dev9p_priv *p) {
    return p && (c->qid.type & QTPOLL) &&
           p9_client_recv_is_deadline_capable(p->client);
}

// =============================================================================
// Refcounted poll-state + arm lifetime (#294 cancel-at-close).
// =============================================================================

// Take a poll-state ref. The caller already holds a ref (the priv's, or
// g_dev9p_poll_lock with ps reachable via p->poll), so ps cannot be freed under
// us -- RELAXED is sufficient (no synchronizes-with needed to acquire an existing
// object).
static void dev9p_poll_state_ref(struct dev9p_poll_state *ps) {
    __atomic_fetch_add(&ps->refs, 1, __ATOMIC_RELAXED);
}

// Drop a poll-state ref; free on the last. ACQ_REL so the freeing thread observes
// every prior holder's writes (and the free is not reordered before this drop). At
// the last drop there is no registered poller -- the sys_poll_for_proc 2C-F1
// discipline keeps a registered poller's Spoor obj-ref alive, so dev9p_close (the
// Spoor's LAST ref) cannot run with a poller still on poll_list -- and no arm, so
// the free races nothing.
static void dev9p_poll_state_unref(struct dev9p_poll_state *ps) {
    if (__atomic_fetch_sub(&ps->refs, 1, __ATOMIC_ACQ_REL) == 1)
        kfree(ps);
}

// The Spoor's poll-state, allocated on first use. The candidate is allocated
// OUTSIDE g_lock and published under it, the loser of a race freeing its own.
// The lockless fast path ACQUIRE-loads, pairing with the RELEASE publish, so it
// observes the initialized poll_list (F5). NULL only for want of memory.
static struct dev9p_poll_state *dev9p_poll_state_get(struct dev9p_priv *p) {
    struct dev9p_poll_state *ps = __atomic_load_n(&p->poll, __ATOMIC_ACQUIRE);
    if (ps) return ps;
    struct dev9p_poll_state *cand = kmalloc(sizeof(*cand), KP_ZERO);
    if (!cand) return NULL;
    poll_waiter_list_init(&cand->poll_list);
    cand->refs = 1;   // #294: the priv's ref (dropped at dev9p_close via
                      // dev9p_poll_priv_release). MUST be set before publish, or
                      // the first arm's teardown takes refs 1->0 and frees ps out
                      // from under p->poll.
    spin_lock(&g_dev9p_poll_lock);
    if (!__atomic_load_n(&p->poll, __ATOMIC_RELAXED))
        __atomic_store_n(&p->poll, cand, __ATOMIC_RELEASE);   // win: publish
    ps = __atomic_load_n(&p->poll, __ATOMIC_RELAXED);
    spin_unlock(&g_dev9p_poll_lock);
    if (ps != cand) kfree(cand);       // lost the race
    return ps;
}

// Free a torn-down arm: drop its session ref (=> may destroy the client+attached
// on the last ref) + its poll-state ref (=> may free ps) + kfree the arm. The
// caller has already unlinked it (or never linked it) and, if it was on the wire,
// abandoned it at the client, so nothing else references it. OUTSIDE
// g_dev9p_poll_lock -- the session unref may sleep (attached_destroy_inner does
// wire clunks). Captures ps BEFORE the kfree so the unref does not read freed
// memory.
static void dev9p_poll_op_free(struct dev9p_poll_op *op) {
    struct dev9p_poll_state *ps = op->ps;
    if (op->attached_owner) p9_attached_unref(op->attached_owner);
    kfree(op);
    dev9p_poll_state_unref(ps);
}

// Unlink `op` from the arm registry if it is still there. Returns whether this
// call removed it: whoever removes an arm owns its teardown. Under g_lock.
static bool dev9p_poll_unlink_op_locked(struct dev9p_poll_op *op) {
    struct dev9p_poll_op **pp = &g_dev9p_poll_ops;
    while (*pp && *pp != op) pp = &(*pp)->next;
    if (*pp != op) return false;
    *pp = op->next;
    __atomic_fetch_sub(&g_dev9p_poll_op_count, 1u, __ATOMIC_RELEASE);
    return true;
}

// =============================================================================
// The two reads: builders, reply decoding, completions.
// =============================================================================

static int dev9p_poll_snap_build(struct p9_session *s, u8 *out, size_t cap, void *ctx) {
    struct dev9p_poll_snap *sn = (struct dev9p_poll_snap *)ctx;
    return p9_session_send_read(s, out, cap, sn->fid,
                                (u64)sn->mask | P9_POLL_SNAPSHOT, 4u);
}

static int dev9p_poll_arm_build(struct p9_session *s, u8 *out, size_t cap, void *ctx) {
    struct dev9p_poll_op *op = (struct dev9p_poll_op *)ctx;
    return p9_session_send_read(s, out, cap, op->fid, (u64)op->mask, 4u);
}

// A readiness reply as revents: the server's u32 LE (ninep::ready_answer, 4
// bytes). A 9P error -- the connection or its server died, or the server refused
// the read -- is POLLERR, a condition the poller must observe. A short reply is 0.
static u16 dev9p_poll_revents_of(int status, struct p9_dispatch_result *dr) {
    if (status < 0) return POLLERR;
    if (!dr || !dr->read_data || dr->read_count < 4u) return 0;
    u32 bits = (u32)dr->read_data[0]
             | ((u32)dr->read_data[1] << 8)
             | ((u32)dr->read_data[2] << 16)
             | ((u32)dr->read_data[3] << 24);
    return (u16)(bits & P9_POLL_MASK);
}

// Linked-and-unanswered ends here, exactly once: the answer and the release
// race for it.
static void dev9p_poll_snap_unlive(struct dev9p_poll_snap *sn) {
    if (__atomic_exchange_n(&sn->live, false, __ATOMIC_ACQ_REL))
        __atomic_fetch_sub(&g_dev9p_poll_snap_live, 1u, __ATOMIC_RELEASE);
}

// The snapshot's answer (net_poll.tla SnapshotReply). Runs UNDER c->lock from the
// demux or mark_dead -- whichever thread is the client's reader -- or, for a
// failure inside the submit, in the submitting poller itself. So it MUST NOT
// sleep, MUST NOT take g_dev9p_poll_lock, and MUST NOT re-enter the p9_client_*
// API (the 9p_client.h seam contract). It writes the poller's slot and wakes the
// poller. The slot is on the poller's stack and stays there: the poller cannot
// return before its release, which takes c->lock after this has run.
static void dev9p_poll_snap_complete(struct p9_rpc *rpc, int status,
                                     struct p9_dispatch_result *dr) {
    if (status == -P9_E_AGAIN) return;     // never sent: still its submitter's
    struct dev9p_poll_snap *sn = (struct dev9p_poll_snap *)rpc;   // rpc at offset 0
    struct poll_snap *s = sn->s;
    s->revents = (u16)(dev9p_poll_revents_of(status, dr) & (sn->mask | POLL_OUTPUT_ONLY));
    __atomic_store_n(&s->state, (u8)POLL_SNAP_ANSWERED, __ATOMIC_RELEASE);
    dev9p_poll_snap_unlive(sn);
    (void)wakeup(s->rendez);               // c->lock -> rendez (leaf; no cycle)
}

// The arm's answer (net_poll.tla ArmReply). Same context and contract as the
// snapshot's. A WAKE only: whatever bitmap it carries is not readiness -- the
// socket may be drained again before the poller looks -- so it marks the arm
// spent and wakes the kthread, whose walk wakes the pollers to sample again.
static void dev9p_poll_arm_complete(struct p9_rpc *rpc, int status,
                                    struct p9_dispatch_result *dr) {
    (void)dr;
    if (status == -P9_E_AGAIN) return;     // never sent: its submitter frees it
    struct dev9p_poll_op *op = (struct dev9p_poll_op *)rpc;       // rpc at offset 0
    __atomic_store_n(&op->terminal, true, __ATOMIC_RELEASE);
    (void)wakeup(&g_dev9p_poll_rendez);    // c->lock -> rendez (leaf; no cycle)
}

// =============================================================================
// The Dev slots: .poll_snapshot / .poll_snapshot_release / .poll_arm.
// =============================================================================

void dev9p_poll_snapshot(struct Spoor *c, short events, struct poll_snap *s) {
    struct dev9p_priv *p = dev9p_priv_of(c);
    if (!dev9p_poll_is_remote(c, p)) {
        s->revents = (u16)(events & POLL_REQUESTABLE);
        __atomic_store_n(&s->state, (u8)POLL_SNAP_ANSWERED, __ATOMIC_RELEASE);
        return;
    }
    s->remote = true;

    // A resend reuses the request a shortage handed back; a first send (or one
    // after a failed allocation) builds it.
    struct dev9p_poll_snap *sn = (struct dev9p_poll_snap *)s->op;
    if (!sn) {
        sn = kmalloc(sizeof(*sn), KP_ZERO);
        if (!sn) {
            // No memory is a shortage, not an answer: the settle resends.
            __atomic_store_n(&s->state, (u8)POLL_SNAP_UNSENT, __ATOMIC_RELEASE);
            return;
        }
        sn->rpc.on_complete = dev9p_poll_snap_complete;
        sn->s               = s;
        sn->client          = p->client;
        sn->fid             = p->fid;
        sn->mask            = (u16)(events & POLL_REQUESTABLE);
        sn->attached_owner  = p->attached_owner;
        if (sn->attached_owner) p9_attached_ref(sn->attached_owner);
        sn->live            = true;
        spin_lock(&g_dev9p_poll_lock);
        sn->next = g_dev9p_poll_snaps;
        g_dev9p_poll_snaps = sn;
        __atomic_fetch_add(&g_dev9p_poll_snap_live, 1u, __ATOMIC_RELEASE);
        spin_unlock(&g_dev9p_poll_lock);
        s->op = sn;
    }

    // SENT before the submit: the answer (under c->lock, after the send) is the
    // only later writer, and it must not be overwritten. A shortage fires the
    // completion with -P9_E_AGAIN, which leaves the slot alone for this store.
    __atomic_store_n(&s->state, (u8)POLL_SNAP_SENT, __ATOMIC_RELEASE);
    int rc = p9_client_submit_async(sn->client, &sn->rpc, dev9p_poll_snap_build, sn);
    if (rc == -P9_E_AGAIN)
        __atomic_store_n(&s->state, (u8)POLL_SNAP_UNSENT, __ATOMIC_RELEASE);
    // Pump its client: for the answer, or -- when UNSENT -- for the replies that
    // give a tag back.
    (void)wakeup(&g_dev9p_poll_rendez);
}

void dev9p_poll_snapshot_release(struct Spoor *c, struct poll_snap *s) {
    (void)c;
    struct dev9p_poll_snap *sn = (struct dev9p_poll_snap *)s->op;
    if (!sn) return;
    s->op = NULL;
    // The barrier. Under c->lock: an answer being delivered right now finishes
    // before this returns, and one still due is flushed (Tflush; its late reply
    // is discarded ownerless) and can no longer fire. An UNSENT snapshot is in no
    // inflight slot, so this is a no-op for it.
    p9_client_abandon_async(sn->client, &sn->rpc);

    spin_lock(&g_dev9p_poll_lock);
    struct dev9p_poll_snap **pp = &g_dev9p_poll_snaps;
    while (*pp && *pp != sn) pp = &(*pp)->next;
    if (*pp == sn) *pp = sn->next;
    dev9p_poll_snap_unlive(sn);        // with the unlink, so the kthread's view agrees
    spin_unlock(&g_dev9p_poll_lock);

    if (sn->attached_owner) p9_attached_unref(sn->attached_owner);
    kfree(sn);
}

int dev9p_poll_arm(struct Spoor *c, short events, struct poll_waiter *pw) {
    struct dev9p_priv *p = dev9p_priv_of(c);
    if (!dev9p_poll_is_remote(c, p)) return 1;   // nothing remote to wait for
    struct dev9p_poll_state *ps = dev9p_poll_state_get(p);
    if (!ps) return 0;                  // no memory: no hook, and uncovered

    // The hook FIRST (PollerArm): the arm's answer, whenever it comes, has a hook
    // to walk -- including an answer that beats this call's return.
    poll_waiter_list_register(&ps->poll_list, pw);

    const u16 asked = (u16)(events & POLL_REQUESTABLE);
    struct dev9p_poll_op *cand    = kmalloc(sizeof(*cand), KP_ZERO);
    struct dev9p_poll_op *failed  = NULL;   // never went out: ours to free
    struct dev9p_poll_op *abandon = NULL;   // widened away: flush + free
    int armed = 0;

    spin_lock(&g_dev9p_poll_lock);
    struct dev9p_poll_op *live = ps->op;
    bool live_ok = live && !__atomic_load_n(&live->terminal, __ATOMIC_ACQUIRE);
    u16 want = (u16)((live_ok ? live->mask : 0u) | asked);
    if (live_ok && (want & ~live->mask) == 0u) {
        armed = 1;                      // the arm out already covers these events
    } else if (cand) {
        // A fresh arm for the union (a terminal arm awaiting its reap is simply
        // superseded: its walk still wakes the pollers it served, and each arms
        // again for itself). The refs are taken before the submit, so an arm
        // that is linked holds them, and one that fails is freed like any other.
        cand->rpc.on_complete = dev9p_poll_arm_complete;
        cand->ps              = ps;
        cand->client          = p->client;
        cand->fid             = p->fid;
        cand->mask            = want;
        cand->attached_owner  = p->attached_owner;
        dev9p_poll_state_ref(ps);
        if (cand->attached_owner) p9_attached_ref(cand->attached_owner);
        // g_lock -> c->lock. On success the answer may already have fired
        // (terminal) -- linking it now still gets it reaped and its walk done.
        if (p9_client_submit_async(p->client, &cand->rpc, dev9p_poll_arm_build,
                                   cand) == 0) {
            cand->next = g_dev9p_poll_ops;
            g_dev9p_poll_ops = cand;
            __atomic_fetch_add(&g_dev9p_poll_op_count, 1u, __ATOMIC_RELEASE);
            ps->op = cand;
            // Widened: the old arm goes only now that its replacement is on the
            // wire, so its pollers were covered throughout.
            if (live_ok && dev9p_poll_unlink_op_locked(live)) abandon = live;
            armed = 1;
        } else {
            // A shortage, or a dead session. Nothing went out, the completion
            // has already run, and a live arm this one would have widened stays
            // -- the pollers it covers stay covered; this one is not.
            failed = cand;
        }
        cand = NULL;
    }
    spin_unlock(&g_dev9p_poll_lock);

    if (cand) kfree(cand);              // unused (the reuse path)
    if (failed) dev9p_poll_op_free(failed);
    if (abandon) {
        // Flush the widened-away arm at the client, then free it (its session +
        // ps refs) -- outside g_lock (abandon takes c->lock; the unref may sleep).
        p9_client_abandon_async(abandon->client, &abandon->rpc);
        dev9p_poll_op_free(abandon);
    }
    if (armed) (void)wakeup(&g_dev9p_poll_rendez);
    return armed;
}

// =============================================================================
// The global poll-pump kthread (KthreadWalk).
// =============================================================================

// Add `client` to the pump set unless it is there; take an EXTRA session ref as
// the borrow-guard (the client stays alive past the unlock + across the blocking
// pump even if its op is freed meanwhile). `owner` NULL only on the test path
// (the client is externally owned): store NULL + skip the unref symmetrically.
static void dev9p_poll_collect_one(struct p9_client *client, struct p9_attached *owner,
                                   struct p9_client **out_cl,
                                   struct p9_attached **out_pin, u32 *n, u32 max) {
    if (*n >= max) return;
    for (u32 i = 0; i < *n; i++)
        if (out_cl[i] == client) return;
    out_cl[*n]  = client;
    out_pin[*n] = owner;
    if (owner) p9_attached_ref(owner);
    (*n)++;
}

// Collect the DISTINCT clients with a read out -- a non-terminal arm or a live
// snapshot -- whose elected readers the kthread must drive this cycle (the Loom
// loom_first_inflight_client pattern, fanned out across clients; F1 fairness).
// out_cl[i] / out_pin[i] are paired; returns the count (<= max). Takes
// g_dev9p_poll_lock. v1.0 has TWO QTPOLL clients -> collects up to 2; with MORE
// than `max` this head-anchored scan STARVES the tail (R2-F1; see
// DEV9P_POLL_MAX_PUMP).
static u32 dev9p_poll_collect_clients(struct p9_client **out_cl,
                                      struct p9_attached **out_pin, u32 max) {
    u32 n = 0;
    spin_lock(&g_dev9p_poll_lock);
    for (struct dev9p_poll_op *op = g_dev9p_poll_ops; op; op = op->next) {
        if (__atomic_load_n(&op->terminal, __ATOMIC_ACQUIRE)) continue;
        dev9p_poll_collect_one(op->client, op->attached_owner, out_cl, out_pin, &n, max);
    }
    for (struct dev9p_poll_snap *sn = g_dev9p_poll_snaps; sn; sn = sn->next) {
        if (!__atomic_load_n(&sn->live, __ATOMIC_ACQUIRE)) continue;
        dev9p_poll_collect_one(sn->client, sn->attached_owner, out_cl, out_pin, &n, max);
    }
    spin_unlock(&g_dev9p_poll_lock);
    return n;
}

static int dev9p_poll_park_cond(void *arg) {
    (void)arg;
    // Wake if an arm is linked (fresh or terminal) or a snapshot is live. Both
    // counts are bumped before the submitter's wake, so the rendez lock makes the
    // wake register-then-observe (the cons_mgr_pending discipline).
    return (__atomic_load_n(&g_dev9p_poll_op_count, __ATOMIC_ACQUIRE) != 0u ||
            __atomic_load_n(&g_dev9p_poll_snap_live, __ATOMIC_ACQUIRE) != 0u) ? 1 : 0;
}

// One service+pump cycle. Reap terminal arms (walk the list + free), collect
// stranded ones (flush + free), pump every client with a read out, or park.
static void dev9p_poll_service_once(void) {
    struct dev9p_poll_op *reap = NULL;       // terminal -> walk poll_list + free
    struct dev9p_poll_op *abandon = NULL;    // stranded -> Tflush + free
    int gc_mode = __atomic_load_n(&g_dev9p_poll_test_gc, __ATOMIC_ACQUIRE);

    // Phase 1 (under g_lock): collect terminal + stranded arms. The empty-check is
    // NESTED under g_lock (g_lock -> poll_list lock) so it is atomic with the
    // unlink + ps->op clear vs a concurrent dev9p_poll_arm (which registers its
    // pw BEFORE taking g_lock, then reuses ps->op under g_lock): a poller already
    // on the list defeats the GC here; one that registers after we GC sees ps->op
    // cleared and submits fresh. No lost wake either way. Snapshots are not
    // looked at: their pollers release them.
    //
    // A stranded arm is FLUSHED here too, under g_lock with its unlink
    // (net_poll_teardown BUGGY_SPLIT_GC). A close that takes g_lock after this
    // finds ps->op cleared and cancels nothing, so the arm's read must already
    // be off the fid: while it is live, the session refuses the close's Tclunk
    // (any_outstanding_on_fid) and dev9p_close has no fallback -- the server's
    // slot would stay bound for the life of the session.
    spin_lock(&g_dev9p_poll_lock);
    struct dev9p_poll_op **pp = &g_dev9p_poll_ops;
    while (*pp) {
        struct dev9p_poll_op *op = *pp;
        bool term = __atomic_load_n(&op->terminal, __ATOMIC_ACQUIRE);
        bool stranded = !term && gc_mode != DEV9P_POLL_GC_SKIP &&
                        poll_waiter_list_empty(&op->ps->poll_list);
        if (term || stranded) {
            *pp = op->next;
            __atomic_fetch_sub(&g_dev9p_poll_op_count, 1u, __ATOMIC_RELEASE);
            if (op->ps->op == op) op->ps->op = NULL;
            if (term) {
                op->next = reap;
                reap = op;
            } else {
                p9_client_abandon_async(op->client, &op->rpc);   // g_lock -> c->lock
                op->next = abandon;
                abandon = op;
            }
            continue;   // *pp already advanced
        }
        pp = &op->next;
    }
    spin_unlock(&g_dev9p_poll_lock);

    // Test hook: stop between the collect and the frees while a test closes a
    // file whose arm this pass collected.
    if (abandon && gc_mode == DEV9P_POLL_GC_HOLD) {
        __atomic_store_n(&g_dev9p_poll_test_held, true, __ATOMIC_RELEASE);
        while (__atomic_load_n(&g_dev9p_poll_test_gc, __ATOMIC_ACQUIRE) == DEV9P_POLL_GC_HOLD)
            sched();
        __atomic_store_n(&g_dev9p_poll_test_held, false, __ATOMIC_RELEASE);
    }

    // Phase 2 (outside g_lock): wake the pollers of each answered arm, then free
    // it. The arm's ps ref keeps ps alive across the walk; the free drops it last.
    while (reap) {
        struct dev9p_poll_op *next = reap->next;
        poll_waiter_list_wake(&reap->ps->poll_list);   // process context
        dev9p_poll_op_free(reap);
        reap = next;
    }

    // Phase 2b (outside g_lock): free each stranded arm, flushed in Phase 1. No
    // poller cares, so no walk.
    while (abandon) {
        struct dev9p_poll_op *next = abandon->next;
        dev9p_poll_op_free(abandon);
        abandon = next;
    }

    // Phase 3: pump the elected reader of EVERY distinct client with a read out
    // (F1), else park. Each pin keeps its client alive across its pump.
    struct p9_client   *clients[DEV9P_POLL_MAX_PUMP];
    struct p9_attached *pins[DEV9P_POLL_MAX_PUMP];
    u32 npump = dev9p_poll_collect_clients(clients, pins, DEV9P_POLL_MAX_PUMP);
    if (npump == 0) {
        // Nothing out -> park. The cond re-checks both counts under the rendez
        // lock (register-then-observe vs a concurrent submit's wake). kproc never
        // group-terminates, so SLEEP_INTR (a defensive death-interrupt) just
        // re-loops -- there is no caller state to unwind.
        (void)sleep(&g_dev9p_poll_rendez, dev9p_poll_park_cond, NULL);
        return;
    }
    bool yield = false;
    for (u32 i = 0; i < npump; i++) {
        u64 deadline = timer_now_ns() + DEV9P_POLL_IDLE_NS;
        int rc = p9_client_reader_pump_once_deadline(clients[i], deadline);
        if (pins[i]) p9_attached_unref(pins[i]);   // release this client's borrow-guard
        // PROGRESS: demuxed a frame (an answer may have landed). IDLE: the
        // boundary deadline lapsed, stream synced. BUSY: another thread holds the
        // reader role and will demux for us. DEAD: client_mark_dead_locked already
        // completed every read in flight on it (arms terminal, snapshots answered
        // POLLERR); only an UNSENT snapshot can still name it, until its poller
        // resends into the dead session -- so yield rather than spin on it.
        if (rc == P9_PUMP_BUSY || rc == P9_PUMP_DEAD) yield = true;
    }
    if (yield) sched();
}

void dev9p_poll_pump_main(void) {
    for (;;) dev9p_poll_service_once();
}

// =============================================================================
// Teardown.
// =============================================================================

void dev9p_poll_priv_release(struct dev9p_priv *p) {
    if (!p || !p->poll) return;
    // #294 cancel-at-close (specs/net_poll_teardown.tla, Fix=TRUE). dev9p_close runs
    // at the Spoor's LAST ref. A registered poller holds the Spoor obj-ref (the
    // sys_poll_for_proc 2C-F1 discipline retains it until AFTER the unregister
    // sweep), so poll_list is empty here, and no snapshot is out (the core
    // releases each before dropping the ref) -- but an ARM may still be live: it
    // pins ps + the session, NOT the Spoor, so it does NOT defer this close. Grab
    // it from the registry (whoever removes it owns the teardown; the kthread may
    // have collected it first), cancel it at the client, and free it. The caller
    // (dev9p_close) then clunks the `ready` fid -- delivered DETERMINISTICALLY now,
    // not hinged on the kthread GC firing (the permanent-slot-leak root). The
    // cancel (abandon_async) runs BEFORE that Tclunk so netd releases the held
    // readiness Tread and the kernel arm does not strand awaiting a reply that
    // will never come. An older, superseded arm of this ps -- terminal, awaiting
    // its reap -- is the kthread's: it holds its own ps ref.
    struct dev9p_poll_state *ps = p->poll;
    struct dev9p_poll_op *grabbed = NULL;

    spin_lock(&g_dev9p_poll_lock);
    struct dev9p_poll_op *op = ps->op;
    if (op) {
        // ps->op is consistent with the registry under g_lock (an arm is linked
        // + published together; the kthread Phase 1 unlinks + clears ps->op; the
        // widen swaps both atomically). So ps->op != NULL => op is in the
        // registry unless the kthread already collected it -- own it only if
        // still present.
        if (dev9p_poll_unlink_op_locked(op)) grabbed = op;
        ps->op = NULL;
    }
    spin_unlock(&g_dev9p_poll_lock);

    if (grabbed) {
        // Cancel at the client (clear c->inflight[tag] + Tflush; #845) so no late
        // completion fires on the freed arm and it does not strand awaiting a
        // reply. Then free it (drop the session + ps refs). Outside g_lock. The
        // client is alive: the priv still holds its session ref (dropped last, in
        // dev9p_close, after the Tclunk) and `grabbed` holds its own.
        p9_client_abandon_async(grabbed->client, &grabbed->rpc);
        dev9p_poll_op_free(grabbed);
    }

    // Drop the priv's ps ref. If the kthread still owns an arm (we did not grab
    // it), that arm's ps ref keeps ps alive until the kthread tears it down; else
    // this is the last ref and frees ps. Either way no UAF: the kthread only derefs
    // ps via a live op->ps, never via the now-cleared p->poll.
    p->poll = NULL;
    dev9p_poll_state_unref(ps);
}

// Test accessors: the arm registry's length, and the snapshots still linked (a
// released snapshot is unlinked, so a count back at its baseline means every one
// was released). No lock for the atomic count; the walk takes g_lock.
u32 dev9p_poll_op_count_for_test(void) {
    return __atomic_load_n(&g_dev9p_poll_op_count, __ATOMIC_ACQUIRE);
}

u32 dev9p_poll_snap_count_for_test(void) {
    u32 n = 0;
    spin_lock(&g_dev9p_poll_lock);
    for (struct dev9p_poll_snap *sn = g_dev9p_poll_snaps; sn; sn = sn->next) n++;
    spin_unlock(&g_dev9p_poll_lock);
    return n;
}

// Whether the kthread is asleep on its park. With no read out it stays there, so
// a test that waits for this before destroying its client knows no pump of that
// client is still running.
bool dev9p_poll_parked_for_test(void) {
    return __atomic_load_n(&g_dev9p_poll_rendez.waiter, __ATOMIC_ACQUIRE) != NULL;
}

// The collector's test mode: SKIP leaves stranded arms for a close to cancel;
// HOLD stops a pass that collected one between the collect and the frees, and
// dev9p_poll_gc_held_for_test says when it is stopped there.
void dev9p_poll_test_gc(enum dev9p_poll_test_gc_mode mode) {
    __atomic_store_n(&g_dev9p_poll_test_gc, (int)mode, __ATOMIC_RELEASE);
}

bool dev9p_poll_gc_held_for_test(void) {
    return __atomic_load_n(&g_dev9p_poll_test_held, __ATOMIC_ACQUIRE);
}

// The test runner's release, after every test: a mode left set by a test that
// failed before restoring it would stop or starve the collector for the rest of
// the boot.
bool dev9p_poll_test_gc_release(void) {
    return __atomic_exchange_n(&g_dev9p_poll_test_gc, (int)DEV9P_POLL_GC_RUN,
                               __ATOMIC_ACQ_REL) != (int)DEV9P_POLL_GC_RUN;
}
