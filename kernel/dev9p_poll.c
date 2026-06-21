// dev9p_poll -- the dev9p `.poll` readiness bridge (net-6b-2b; NET-DESIGN
// section 12.2, specs/net_poll.tla). The one new kernel surface in the net arc.
//
// A poll() on a netd readiness file (/net/<proto>/N/ready, marked qid.type
// QTPOLL) must block until the socket is readable/writable per the requested
// events. The poller PARKS in sys_poll_for_proc -- it does NOT block-read -- so no
// synchronous reader drives the 9P elected reader (#841) for the outstanding
// readiness Tread. A boot-spawned GLOBAL poll-pump kthread drives it (the
// cons_poll console_mgr + Loom-4 SQPOLL analog).
//
// The three spec actions (specs/net_poll.tla) map to:
//
//   dev9p_poll(c, events, pw)              [PollerRegister]
//     - QTPOLL gate: a regular dev9p file (no QTPOLL on its cached qid) is POSIX
//       always-ready (events & POLL_REQUESTABLE -- the prior NULL-slot behavior).
//       Only a netd readiness file is probed. dev9p is generic (Stratum / corvus
//       too); the gate FAILS SAFE (unmarked -> always-ready, never an unsound
//       probe of a regular file).
//     - register pw on the Spoor's poll-state hook list FIRST.
//     - ensure a non-terminal readiness probe is outstanding (a Tread whose OFFSET
//       carries the event mask) BEFORE returning the not-ready sample -- the
//       PROBE-then-observe (I-9 generalized): no readiness edge that lands between
//       the sample and the park is lost (the registered hook + the outstanding
//       probe guarantee a wake). Submit fresh if none / terminal; widen by
//       abandon+resubmit if a broader poller's events aren't covered.
//     - sample the poll-state cached_revents (atomic) + return revents & events.
//
//   dev9p_poll_complete(rpc, status, dr)   [NetdReplyDemux] -- UNDER c->lock
//     - record the 4-byte readiness bitmap into the op's poll-state cached_revents
//       (atomic) + mark the op terminal + wake the kthread. NO sleep, NO
//       poll-state lock, NO p9_client re-entry (the 9p_client.h seam contract).
//
//   dev9p_poll_pump_main()                 [KthreadWalk]
//     - reap terminal ops: walk their poll_list (poll_waiter_list_wake, PROCESS
//       context) + release the spoor_ref pin;
//     - GC stranded ops (non-terminal, empty poll_list -> the poll() that needed
//       it ended; abandon via Tflush so it does not leak the pinned Spoor);
//     - pump the elected reader of EVERY distinct client with a remaining
//       outstanding op (borrow each client from a live op's pin -- the Loom
//       borrow-guard, NEVER owning the client lifetime; F1: fairness across
//       clients -- pumping only one would starve a second QTPOLL client's reply);
//     - park when the registry is empty.
//
// LOCK ORDER (verified acyclic):
//   g_dev9p_poll_lock -> c->lock           (dev9p_poll submit / GC abandon)
//   g_dev9p_poll_lock -> poll_list lock     (GC empty-check, nested for atomicity
//                                            with the unlink vs a concurrent reuse)
//   poll_list lock    -> g_timerwait -> rendez -> cpu_sched   (the wake; OUTSIDE g_lock)
//   c->lock           -> rendez             (dev9p_poll_complete wakes the kthread)
// g_dev9p_poll_lock is NEVER held across a wakeup or across the blocking pump; the
// kthread takes c->lock (pump) and poll_list lock (wake) SEPARATELY, never nested.

#include <thylacine/dev9p.h>

#include <thylacine/9p_attach.h>
#include <thylacine/9p_client.h>
#include <thylacine/9p_session.h>
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

// One in-flight readiness op. `rpc` at OFFSET 0 so dev9p_poll_complete recovers
// the container with a single cast (the audited Loom offset-0 idiom). #294
// cancel-at-close: the op does NOT pin the readiness Spoor (that would defer
// dev9p_close past the user's fd-close -- the permanent-slot-leak root). It holds
// instead (a) a poll-state ref (ps->refs; keeps op->ps deref-safe) + (b) a session
// ref (p9_attached_ref on attached_owner; keeps op->client alive -- the kthread
// borrows the client, never owning the lifetime). So the op survives independent
// of the user's fd, and dev9p_close runs AT fd-close, cancels the op, and delivers
// the `ready`-fd Tclunk deterministically (specs/net_poll_teardown.tla, Fix=TRUE).
// `attached_owner` is NULL only on the test path (dev9p_attach_client -- the client
// is externally owned); a netd QTPOLL Spoor always carries a session. `terminal` is
// set by complete (under c->lock, atomic) and read by the kthread reap.
struct dev9p_poll_op {
    struct p9_rpc            rpc;       // OFFSET 0 -- the completion casts (op *)rpc
    struct dev9p_poll_state *ps;        // the poll-state this op probes for (holds a ps ref)
    struct p9_attached      *attached_owner; // session ref (=> client alive); NULL = test path
    struct p9_client        *client;    // borrowed (valid while the session ref / ext-owner holds)
    u32                      fid;        // the readiness file's 9P fid
    u16                      mask;       // the event mask = the Tread offset
    bool                     terminal;   // completion fired (atomic; reaped by the kthread)
    struct dev9p_poll_op    *next;       // g_dev9p_poll_ops chain (under g_dev9p_poll_lock)
};

_Static_assert(__builtin_offsetof(struct dev9p_poll_op, rpc) == 0,
               "p9_rpc must be at offset 0 -- dev9p_poll_complete recovers the container");

// Per-readiness-Spoor poll state. Lazily allocated by dev9p_poll the first time a
// QTPOLL Spoor is polled; hung off dev9p_priv->poll. #294: independently
// REFCOUNTED -- the priv holds one ref (dropped via dev9p_poll_priv_release at
// dev9p_close) and each outstanding op holds one (taken at submit, dropped at op
// teardown); freed when both drop. This decoupling is what lets dev9p_close free
// the priv + clunk the fid while an op the kthread still owns keeps ps (op->ps)
// alive -- no UAF (specs/net_poll_teardown.tla NoUseAfterFreePs). Multi-thread-
// Proc-reachable (handle_dup shares the Spoor -> the same priv -> the same poll-
// state): poll_list has its own lock; cached_revents + refs are atomic; `op` +
// `wanted_mask` are under g_dev9p_poll_lock.
struct dev9p_poll_state {
    struct poll_waiter_list  poll_list;     // pollers' hooks (own lock)
    u32                      cached_revents; // atomic: DEV9P_POLL_VALID | revents
    struct dev9p_poll_op    *op;            // the outstanding op (under g_lock); NULL = none
    u16                      wanted_mask;    // union of pollers' events for the live op (g_lock)
    int                      refs;           // atomic: priv (1) + 1 per outstanding op; free at 0
};

#define DEV9P_POLL_VALID    0x00010000u   // cached_revents bit 16: a completion recorded
#define DEV9P_POLL_RV_MASK  0x0000ffffu   // the revents bits (POLLIN/OUT/ERR/HUP fit in 16)
#define DEV9P_POLL_IDLE_NS  (20ull * 1000ull * 1000ull)   // 20ms reader-pump idle deadline
// Distinct QTPOLL clients the kthread pumps per cycle (F1: the global pump must be
// FAIR across clients -- pumping only one would starve a second client's pending
// reply). v1.0 has exactly ONE QTPOLL client (the single netd /net mount); the
// per-user-netd v1.x config has a handful. With MORE than this many simultaneous
// QTPOLL clients each holding a perpetually-parked op, the cap is NOT graceful: the
// registry is LIFO and the collect always walks from the head with no rotation, so
// the >MAX clients nearest the head are pumped every cycle and a TAIL client is
// STARVED outright (its reply never demuxed -> its pollers hang), not merely delayed
// (R2-F1). The v1.x close (the per-client work-queue / a fair round-robin cursor)
// must use a fair start, not this head-anchored scan. Unreachable below the cap, far
// above the realistic per-user-netd count.
#define DEV9P_POLL_MAX_PUMP 16

static spin_lock_t            g_dev9p_poll_lock;
static struct dev9p_poll_op  *g_dev9p_poll_ops;       // the registry (under g_lock)
static u32                    g_dev9p_poll_op_count;   // registry length (atomic; the park cond)
static struct Rendez          g_dev9p_poll_rendez;     // the kthread park
static bool                   g_dev9p_poll_inited;

void dev9p_poll_init(void) {
    if (g_dev9p_poll_inited) return;
    spin_lock_init(&g_dev9p_poll_lock);
    rendez_init(&g_dev9p_poll_rendez);
    g_dev9p_poll_ops = NULL;
    g_dev9p_poll_op_count = 0;
    g_dev9p_poll_inited = true;
}

// =============================================================================
// Refcounted poll-state + op lifetime (#294 cancel-at-close).
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
// Spoor's LAST ref) cannot run with a poller still on poll_list -- and no op, so
// the free races nothing.
static void dev9p_poll_state_unref(struct dev9p_poll_state *ps) {
    if (__atomic_fetch_sub(&ps->refs, 1, __ATOMIC_ACQ_REL) == 1)
        kfree(ps);
}

// Free a torn-down op: drop its session ref (=> may destroy the client+attached on
// the last ref) + its poll-state ref (=> may free ps) + kfree the op. The caller
// has already removed it from the registry and (if it had an in-flight reply)
// abandoned it at the client, so nothing else references it. Runs OUTSIDE
// g_dev9p_poll_lock -- the session unref may sleep (attached_destroy_inner does
// wire clunks). Captures ps BEFORE the kfree so the unref does not read freed
// memory.
static void dev9p_poll_op_free(struct dev9p_poll_op *op) {
    struct dev9p_poll_state *ps = op->ps;
    if (op->attached_owner) p9_attached_unref(op->attached_owner);
    kfree(op);
    dev9p_poll_state_unref(ps);
}

// =============================================================================
// The async readiness op: build thunk + completion callback.
// =============================================================================

// The Tmsg builder for the readiness probe: a Tread whose OFFSET carries the poll
// event mask + a 4-byte count (the bitmap netd returns). netd's `ready` file
// serves a Tread(offset = mask) as a NON-consuming readiness probe that defers
// until satisfiable (net-6b-2a). Invoked by p9_client_submit_async under c->lock.
static int dev9p_poll_build(struct p9_session *s, u8 *out, size_t cap, void *ctx) {
    struct dev9p_poll_op *op = (struct dev9p_poll_op *)ctx;
    return p9_session_send_read(s, out, cap, op->fid, (u64)op->mask, 4u);
}

// Parse the readiness reply into a poll revents bitmap. netd returns the satisfied
// revents as a u32 LE (net-6b-2a build_rread, 4 bytes). A 9P error (the connection
// / netd died) -> POLLERR (a poll-ready condition the poller must observe). A
// malformed / short reply -> 0 (not ready; the probe simply did not satisfy).
static u16 dev9p_poll_revents_of(int status, struct p9_dispatch_result *dr) {
    if (status < 0) return POLLERR;
    if (!dr || !dr->read_data || dr->read_count < 4u) return 0;
    u32 bits = (u32)dr->read_data[0]
             | ((u32)dr->read_data[1] << 8)
             | ((u32)dr->read_data[2] << 16)
             | ((u32)dr->read_data[3] << 24);
    return (u16)(bits & DEV9P_POLL_RV_MASK);
}

// The async completion callback (NetdReplyDemux). Runs UNDER the 9P client's
// c->lock (invoked from demux_frame_locked / client_mark_dead_locked), so it MUST
// NOT sleep, MUST NOT take the poll-state lock (-> no lock-order cycle), and MUST
// NOT re-enter the p9_client_* API (the 9p_client.h seam contract). It records the
// bitmap into the poll-state (atomic), marks the op terminal (atomic), and wakes
// the kthread (the kthread does the poll_list walk + the reap in process context).
static void dev9p_poll_complete(struct p9_rpc *rpc, int status,
                                struct p9_dispatch_result *dr) {
    struct dev9p_poll_op *op = (struct dev9p_poll_op *)rpc;   // rpc at offset 0
    u16 revents = dev9p_poll_revents_of(status, dr);
    // Record into the poll-state. RELEASE so the poller's post-wake re-sample (an
    // ACQUIRE load) observes the bitmap. A completion only ever lands a non-zero
    // bitmap (netd defers until satisfiable) OR POLLERR -- both are "ready".
    __atomic_store_n(&op->ps->cached_revents,
                     DEV9P_POLL_VALID | (u32)revents, __ATOMIC_RELEASE);
    __atomic_store_n(&op->terminal, true, __ATOMIC_RELEASE);
    (void)wakeup(&g_dev9p_poll_rendez);   // c->lock -> rendez (leaf; no cycle)
}

// =============================================================================
// dev9p_poll -- the Dev `.poll` slot (PollerRegister).
// =============================================================================

// Submit a fresh readiness op for `ps` with mask `wanted`. Allocated OUTSIDE
// g_lock by the caller (`op_in`); this links it + publishes ps->op + invalidates
// the stale cached_revents, then submits. MUST be called under g_dev9p_poll_lock.
// On a synchronous submit failure p9_client_submit_async fires dev9p_poll_complete
// (which sets op->terminal + records POLLERR) before returning -- the op is
// already linked, so the kthread reaps it. Returns nothing; ps->op is always set
// to op_in (the reaper clears it).
static void dev9p_poll_submit_locked(struct dev9p_poll_state *ps,
                                     struct dev9p_poll_op *op_in,
                                     struct p9_client *client, u32 fid, u16 wanted,
                                     struct p9_attached *attached_owner) {
    // op_in is freshly kmalloc(KP_ZERO)'d; set the non-zero fields. #294: the op
    // pins ps + the session (NOT the Spoor), so it outlives the user's fd without
    // deferring dev9p_close. Both refs are taken HERE, under g_lock, atomically
    // with linking the op into the registry below -- so an op reachable from the
    // registry always holds them (and on a synchronous submit failure the
    // already-linked op is reaped by the kthread, which drops them).
    op_in->rpc.on_complete = dev9p_poll_complete;
    op_in->ps             = ps;
    op_in->client         = client;
    op_in->fid            = fid;
    op_in->mask           = wanted;
    op_in->attached_owner = attached_owner;
    dev9p_poll_state_ref(ps);                            // the op's ps ref
    if (attached_owner) p9_attached_ref(attached_owner); // the op's session ref (=> client alive)
    // Link into the registry + publish as ps's live op BEFORE submitting, so a
    // synchronous submit failure's on_complete (which sets op->terminal) finds the
    // op linked -> the kthread reaps it. A previous op (terminal, pending reap) is
    // NOT cleared from ps->op here -- it is overwritten; the kthread still reaps it
    // from the registry (it walks the chain, not ps->op).
    op_in->next = g_dev9p_poll_ops;
    g_dev9p_poll_ops = op_in;
    __atomic_fetch_add(&g_dev9p_poll_op_count, 1u, __ATOMIC_RELEASE);
    ps->op = op_in;
    ps->wanted_mask = wanted;
    // Invalidate any stale cached bitmap: the fresh probe's completion is the only
    // value a parked poller may act on (level-triggered: a poller must not be
    // satisfied by a pre-submit reading the socket may have moved past).
    __atomic_store_n(&ps->cached_revents, 0u, __ATOMIC_RELEASE);
    // Submit the readiness Tread (offset = the event mask). Takes c->lock
    // (g_lock -> c->lock; verified acyclic). On a synchronous build/send failure
    // it fires dev9p_poll_complete(-errno, NULL) -> POLLERR cached + op terminal
    // before returning; on success the completion fires later from the demux. Owns
    // op_in either way -- the caller must not touch it after this.
    (void)p9_client_submit_async(client, &op_in->rpc, dev9p_poll_build, op_in);
}

short dev9p_poll(struct Spoor *c, short events, struct poll_waiter *pw) {
    struct dev9p_priv *p = dev9p_priv_of(c);
    // QTPOLL gate: a regular dev9p file (no QTPOLL on its cached qid) is POSIX
    // always-ready -- a regular file is never read by poll(). Only a netd
    // readiness file (which sets P9_QTPOLL on its qid) is probed. Fail-safe.
    if (!p || !(c->qid.type & QTPOLL)) {
        return (short)(events & POLL_REQUESTABLE);
    }
    // The poll-pump kthread drives the readiness probe with a frame-boundary recv
    // DEADLINE (so a deferred reply -- POLLIN with no data -- lets it re-check its
    // work + GC a stranded op, never blocking forever). That REQUIRES a
    // deadline-capable transport. netd's /net mount is srvconn (deadline-capable),
    // so this gate always passes in v1.0; a hypothetical non-deadline-capable
    // QTPOLL server would hang the kthread on a deferred poll, so degrade to POSIX
    // always-ready (fail-safe -- the same gate the Loom SQPOLL register applies).
    if (!p9_client_recv_is_deadline_capable(p->client)) {
        return (short)(events & POLL_REQUESTABLE);
    }

    // Lazily allocate the poll-state. Alloc the candidate OUTSIDE g_lock (no
    // kmalloc under the lock); publish under g_lock, freeing the loser of a race.
    // ACQUIRE-load the lazily-published poll-state (paired with the RELEASE store
    // at publish below): the fast-path read is lockless, so it must synchronize-with
    // the publish to observe the initialized poll_list (F5). Pointer loads are
    // tear-free on aarch64 and poll_waiter_list_init is KP_ZERO-equivalent, so this
    // is hygiene + future-proofing of the double-checked lock, not a live bug today.
    struct dev9p_poll_state *ps = __atomic_load_n(&p->poll, __ATOMIC_ACQUIRE);
    if (!ps) {
        struct dev9p_poll_state *cand = kmalloc(sizeof(*cand), KP_ZERO);
        if (!cand) {
            // OOM: degrade to always-ready (fail-safe; a poll-state alloc failure
            // can never make poll() wrong, only non-blocking-on-this-fd).
            return (short)(events & POLL_REQUESTABLE);
        }
        poll_waiter_list_init(&cand->poll_list);
        cand->refs = 1;   // #294: the priv's poll-state ref (dropped at dev9p_close
                          // via dev9p_poll_priv_release). Each outstanding op adds
                          // one more; ps frees when both the priv + all ops drop.
                          // MUST be set before publish, or the first op's teardown
                          // takes refs 1->0 and frees ps out from under p->poll.
        spin_lock(&g_dev9p_poll_lock);
        // RELEASE-publish so the lockless fast-path ACQUIRE-load above observes the
        // initialized poll_list (F5). The re-reads here are serialized by g_lock.
        if (!__atomic_load_n(&p->poll, __ATOMIC_RELAXED))
            __atomic_store_n(&p->poll, cand, __ATOMIC_RELEASE);   // win: publish
        ps = __atomic_load_n(&p->poll, __ATOMIC_RELAXED);
        spin_unlock(&g_dev9p_poll_lock);
        if (ps != cand) kfree(cand);       // lost the race: free the candidate
    }

    // Register the hook FIRST (before ensuring the probe + sampling) so any
    // completion of the probe we are about to ensure finds it -- the I-9
    // register-then-observe (specs/net_poll.tla PollerRegister).
    if (pw) poll_waiter_list_register(&ps->poll_list, pw);

    // Pre-allocate a candidate op OUTSIDE g_lock in case we must submit. Freed if
    // unused (the common reuse path). The spoor_ref pin is taken only if we commit.
    struct dev9p_poll_op *cand_op = kmalloc(sizeof(*cand_op), KP_ZERO);

    bool woke = false;
    struct dev9p_poll_op *abandon = NULL;   // an op to abandon+reap (widen path)
    u16 ready_now = 0;
    // The revents bits this poller may observe: its requested POLLIN/POLLOUT, plus
    // the always-reported POLLERR/POLLHUP.
    const u32 want = (u32)((events & POLL_REQUESTABLE) | POLL_OUTPUT_ONLY);

    spin_lock(&g_dev9p_poll_lock);
    ps->wanted_mask |= (u16)events;
    u32 cached = __atomic_load_n(&ps->cached_revents, __ATOMIC_ACQUIRE);
    ready_now = (cached & DEV9P_POLL_VALID)
              ? (u16)(cached & DEV9P_POLL_RV_MASK & want) : 0u;

    if (ready_now == 0u) {
        // PROBE-then-observe (I-9): ensure a non-terminal probe is outstanding
        // BEFORE we return not-ready + the caller parks. Reuse a covering live op;
        // widen (abandon + resubmit the union) for a broader poller; else submit fresh.
        struct dev9p_poll_op *live = ps->op;
        bool live_usable = live && !__atomic_load_n(&live->terminal, __ATOMIC_ACQUIRE);
        if (live_usable && ((ps->wanted_mask & ~live->mask) == 0)) {
            // The outstanding probe already covers this poller's events -- reuse it.
        } else if (cand_op) {
            // Widen = abandon the live op + submit the union (the Tflush is
            // non-blocking; reaping its pin is deferred outside g_lock).
            if (live_usable) {
                struct dev9p_poll_op **pp = &g_dev9p_poll_ops;
                while (*pp && *pp != live) pp = &(*pp)->next;
                if (*pp == live) {
                    *pp = live->next;
                    __atomic_fetch_sub(&g_dev9p_poll_op_count, 1u, __ATOMIC_RELEASE);
                }
                abandon = live;            // unlinked -> no concurrent reaper/completer
            }
            // #294: the op pins ps + the session (in submit_locked), NOT the Spoor,
            // so dev9p_close runs at fd-close and cancels it -- the leak fix.
            dev9p_poll_submit_locked(ps, cand_op, p->client, p->fid,
                                     ps->wanted_mask, p->attached_owner);
            cand_op = NULL;                // consumed
            woke = true;
            // A synchronous submit failure fired the completion (POLLERR) under
            // c->lock during submit_async -- re-read to surface it without a park.
            cached = __atomic_load_n(&ps->cached_revents, __ATOMIC_ACQUIRE);
            ready_now = (cached & DEV9P_POLL_VALID)
                      ? (u16)(cached & DEV9P_POLL_RV_MASK & want) : 0u;
        }
        // cand_op == NULL on OOM: no fresh probe. The hook is still registered.
        //
        // F2 (+ R2-F2): degrade to always-ready when this poll has NO path to a
        // COVERING completion -- OOM left no fresh probe (cand_op was NULL, so !woke)
        // AND either there is no live op (live_usable false) OR the live op does not
        // cover this poller's events ((wanted_mask & ~live->mask) != 0, so its
        // completion carries bits this poller masks back to 0). Without the degrade an
        // infinite-timeout poll parks UNWAKEABLY (no live op) or, with a NARROWER live
        // op under SUSTAINED OOM, spurious-wakes-and-reparks forever -- never a hang of
        // the machine, but no forward progress. always-ready is a safe spurious wake
        // the app re-checks. A COVERING live op (or a submit, woke) does make progress
        // -> no degrade there. The `||` short-circuits, guarding the live->mask deref.
        if (!woke && ready_now == 0u &&
            (!live_usable || (ps->wanted_mask & ~live->mask) != 0u))
            ready_now = (u16)(events & POLL_REQUESTABLE);
    }
    // Consume: returning ready invalidates the bitmap so the NEXT poll() re-probes
    // CURRENT readiness -- the cache is a one-shot bridge between the async
    // completion and the poller's re-sample, NOT a persistent readiness state
    // (level-triggered: a stale "readable" after the app drained the data would
    // busy-loop the app).
    if (ready_now != 0u)
        __atomic_store_n(&ps->cached_revents, 0u, __ATOMIC_RELEASE);
    spin_unlock(&g_dev9p_poll_lock);

    if (cand_op) kfree(cand_op);   // unused candidate

    if (abandon) {
        // Cancel the widened-away op at the client (Tflush; #845) THEN free it
        // (drop its session + ps refs) -- outside g_lock (abandon takes c->lock;
        // the session unref may sleep).
        p9_client_abandon_async(abandon->client, &abandon->rpc);
        dev9p_poll_op_free(abandon);
    }
    if (woke) (void)wakeup(&g_dev9p_poll_rendez);

    return (short)ready_now;
}

// =============================================================================
// The global poll-pump kthread (KthreadWalk).
// =============================================================================

// Collect the DISTINCT clients of the non-terminal ops -- the elected readers the
// kthread must drive this cycle (the Loom loom_first_inflight_client pattern, fanned
// out across clients). F1: pumping only ONE client per cycle (the head's) would
// starve a SECOND client's pending reply -- a perpetually-parked op on client CX
// (non-terminal, non-empty poll_list -> never reaped/GC'd) would pin the pump to CX
// forever, so client CY's ready socket never gets demuxed and CY's poller hangs. The
// global pump must be FAIR. Dedup by client pointer; take an EXTRA spoor_ref on each
// client's pin (the borrow-guard: the Spoor => client stays alive past the unlock +
// across the blocking pump, even if a concurrent reaper frees the op). out_cl[i] /
// out_pin[i] are paired; returns the count (<= max). Under g_dev9p_poll_lock.
//
// v1.0 has exactly ONE QTPOLL client (the single netd /net mount) -> collects 1; the
// per-user-netd v1.x config a handful. With MORE than `max` distinct clients this
// head-anchored LIFO scan STARVES the tail (R2-F1; see the DEV9P_POLL_MAX_PUMP note)
// -- the v1.x per-client-work-queue close must use a fair start, not this scan.
static u32 dev9p_poll_collect_clients(struct p9_client **out_cl,
                                      struct p9_attached **out_pin, u32 max) {
    u32 n = 0;
    spin_lock(&g_dev9p_poll_lock);
    for (struct dev9p_poll_op *op = g_dev9p_poll_ops; op && n < max; op = op->next) {
        if (__atomic_load_n(&op->terminal, __ATOMIC_ACQUIRE))
            continue;
        bool seen = false;
        for (u32 i = 0; i < n; i++)
            if (out_cl[i] == op->client) { seen = true; break; }
        if (seen) continue;
        out_cl[n]  = op->client;
        // Borrow-guard: take an EXTRA session ref so the client stays alive past
        // the unlock + across the blocking pump, even if a concurrent reaper frees
        // this op (#294: was spoor_ref(op->pinned)). NULL only on the test path
        // (the client is externally owned) -- store NULL + skip the unref
        // symmetrically.
        out_pin[n] = op->attached_owner;
        if (op->attached_owner) p9_attached_ref(op->attached_owner);
        n++;
    }
    spin_unlock(&g_dev9p_poll_lock);
    return n;
}

static int dev9p_poll_park_cond(void *arg) {
    (void)arg;
    // Wake if the registry is non-empty (a fresh submit links one + wakes us; a
    // completion sets a registry op terminal + wakes us). The count is bumped under
    // g_lock and the wake follows the bump -> register-then-observe via the rendez
    // lock (the cons_mgr_pending discipline).
    return __atomic_load_n(&g_dev9p_poll_op_count, __ATOMIC_ACQUIRE) != 0u ? 1 : 0;
}

// One service+pump cycle. Reap terminal ops (walk poll_list + release pin), GC
// stranded ops (empty poll_list -> abandon), pump a client with a live op, or park.
static void dev9p_poll_service_once(void) {
    struct dev9p_poll_op *reap = NULL;       // terminal -> walk poll_list + reap
    struct dev9p_poll_op *abandon = NULL;    // stranded -> Tflush + reap

    // Phase 1 (under g_lock): collect terminal + stranded ops. The empty-check is
    // NESTED under g_lock (g_lock -> poll_list lock) so it is atomic with the
    // unlink + ps->op clear vs a concurrent dev9p_poll reuse (which registers its
    // pw BEFORE taking g_lock, then reuses ps->op under g_lock): a poller already
    // on the list defeats the GC here; one that registers after we GC sees ps->op
    // cleared and submits fresh. No lost wake either way.
    spin_lock(&g_dev9p_poll_lock);
    struct dev9p_poll_op **pp = &g_dev9p_poll_ops;
    while (*pp) {
        struct dev9p_poll_op *op = *pp;
        bool term = __atomic_load_n(&op->terminal, __ATOMIC_ACQUIRE);
        bool stranded = !term && poll_waiter_list_empty(&op->ps->poll_list);
        if (term || stranded) {
            *pp = op->next;
            __atomic_fetch_sub(&g_dev9p_poll_op_count, 1u, __ATOMIC_RELEASE);
            if (op->ps->op == op) { op->ps->op = NULL; op->ps->wanted_mask = 0; }
            if (term) { op->next = reap;    reap = op; }
            else      { op->next = abandon; abandon = op; }
            continue;   // *pp already advanced
        }
        pp = &op->next;
    }
    spin_unlock(&g_dev9p_poll_lock);

    // Phase 2 (outside g_lock): wake pollers + release pins for terminal ops. The
    // pin keeps the Spoor (=> ps) alive across the poll_list walk; clunk LAST (it
    // may drop the last ref -> dev9p_close -> free the poll-state).
    while (reap) {
        struct dev9p_poll_op *next = reap->next;
        // Wake pollers BEFORE freeing the op (the wake touches reap->ps, kept alive
        // by the op's ps ref). #294: op_free drops the session + ps refs (was
        // spoor_clunk(pinned)); the ps ref drop may free ps -- the last touch of
        // reap->ps is the wake above.
        poll_waiter_list_wake(&reap->ps->poll_list);   // process context
        dev9p_poll_op_free(reap);
        reap = next;
    }

    // Phase 2b (outside g_lock): cancel stranded ops at the client (Tflush) + free
    // them (drop the session + ps refs). No poller cares, so no poll_list walk.
    while (abandon) {
        struct dev9p_poll_op *next = abandon->next;
        p9_client_abandon_async(abandon->client, &abandon->rpc);
        dev9p_poll_op_free(abandon);
        abandon = next;
    }

    // Phase 3: pump the elected reader of EVERY distinct client with a remaining
    // live op (F1 -- fairness; pumping only one would starve a second client), else
    // park. Each pin (a borrowed session ref) keeps its client alive past the
    // unlock + across the blocking pump; unref each after its pump.
    struct p9_client   *clients[DEV9P_POLL_MAX_PUMP];
    struct p9_attached *pins[DEV9P_POLL_MAX_PUMP];
    u32 npump = dev9p_poll_collect_clients(clients, pins, DEV9P_POLL_MAX_PUMP);
    if (npump == 0) {
        // Nothing outstanding -> park. The cond re-checks the registry count under
        // the rendez lock (register-then-observe vs a concurrent submit's wake).
        // kproc never group-terminates, so SLEEP_INTR (a defensive death-interrupt)
        // just re-loops -- there is no caller state to unwind.
        (void)sleep(&g_dev9p_poll_rendez, dev9p_poll_park_cond, NULL);
        return;
    }
    bool any_busy = false;
    for (u32 i = 0; i < npump; i++) {
        u64 deadline = timer_now_ns() + DEV9P_POLL_IDLE_NS;
        int rc = p9_client_reader_pump_once_deadline(clients[i], deadline);
        if (pins[i]) p9_attached_unref(pins[i]);   // release this client's borrow-guard
        if (rc == P9_PUMP_BUSY) any_busy = true;
        // PROGRESS: demuxed a frame (a completion may have set an op terminal).
        // IDLE: the boundary deadline lapsed, stream synced. DEAD:
        // client_mark_dead_locked already error-completed every async op of THIS
        // client (each dev9p_poll_complete fired -P9_E_IO -> POLLERR cached +
        // terminal), so the next cycle reaps them (waking pollers POLLERR). A DEAD
        // client's ops drop out of the next collect (terminal). All cases: loop.
    }
    if (any_busy) sched();   // some client's reader role is held by a sync reader -- yield
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
    // sweep), so poll_list is empty here -- but an outstanding readiness OP may
    // still be live: it pins ps + the session, NOT the Spoor, so it does NOT defer
    // this close. Grab it from the registry (whoever removes it owns the teardown;
    // the kthread may have collected it first), cancel it at the client, and free
    // it. The caller (dev9p_close) then clunks the `ready` fid -- delivered
    // DETERMINISTICALLY now, not hinged on the kthread GC firing (the permanent-
    // slot-leak root). The cancel (abandon_async) runs BEFORE that Tclunk so netd
    // releases the held readiness Tread and the kernel op does not strand awaiting
    // a reply that will never come.
    struct dev9p_poll_state *ps = p->poll;
    struct dev9p_poll_op *grabbed = NULL;

    spin_lock(&g_dev9p_poll_lock);
    struct dev9p_poll_op *op = ps->op;
    if (op) {
        // ps->op is consistent with the registry under g_lock (submit links + sets
        // ps->op; the kthread Phase 1 unlinks + clears ps->op; the widen swaps both
        // atomically). So ps->op != NULL => op is in the registry unless the kthread
        // already collected it -- search, and own it only if still present.
        struct dev9p_poll_op **pp = &g_dev9p_poll_ops;
        while (*pp && *pp != op) pp = &(*pp)->next;
        if (*pp == op) {
            *pp = op->next;
            __atomic_fetch_sub(&g_dev9p_poll_op_count, 1u, __ATOMIC_RELEASE);
            grabbed = op;                  // we removed it -> we own its teardown
        }
        ps->op = NULL;
        ps->wanted_mask = 0;
    }
    spin_unlock(&g_dev9p_poll_lock);

    if (grabbed) {
        // Cancel at the client (clear c->inflight[tag] + Tflush; #845) so no late
        // dev9p_poll_complete fires on the freed op and the kernel op does not
        // strand awaiting a reply. Then free it (drop the session + ps refs).
        // Outside g_lock (abandon takes c->lock; the unrefs may sleep). The client
        // is alive: the priv still holds its session ref (dropped last, in
        // dev9p_close, after the Tclunk) and `grabbed` holds its own.
        p9_client_abandon_async(grabbed->client, &grabbed->rpc);
        dev9p_poll_op_free(grabbed);
    }

    // Drop the priv's ps ref. If the kthread still owns an op (we did not grab it),
    // that op's ps ref keeps ps alive until the kthread tears it down; else this is
    // the last ref and frees ps. Either way no UAF: the kthread only derefs ps via
    // a live op->ps, never via the now-cleared p->poll.
    p->poll = NULL;
    dev9p_poll_state_unref(ps);
}

// Test accessor: the live readiness-op registry length (g_dev9p_poll_op_count).
// Lets test_dev9p assert the cancel-at-close teardown (op count back to baseline)
// without exposing the static registry. Reads the atomic count; no lock needed.
u32 dev9p_poll_op_count_for_test(void) {
    return __atomic_load_n(&g_dev9p_poll_op_count, __ATOMIC_ACQUIRE);
}
