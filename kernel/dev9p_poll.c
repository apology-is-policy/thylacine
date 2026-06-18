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
//     - pump the elected reader of a client with a remaining outstanding op
//       (borrow the client from the live op's pin -- the Loom borrow-guard,
//       NEVER owning the client lifetime);
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

#include <thylacine/9p_client.h>
#include <thylacine/9p_session.h>
#include <thylacine/extinction.h>
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
// the container with a single cast (the audited Loom offset-0 idiom). Holds an
// INDEPENDENT spoor_ref on `pinned` (the readiness Spoor) -- the kthread borrows
// the client from it, never owning the client lifetime (a live dev9p Spoor
// implies a live client; the Loom I-30 pin / borrow-guard). `terminal` is set by
// complete (under c->lock, atomic) and read by the kthread reap.
struct dev9p_poll_op {
    struct p9_rpc            rpc;       // OFFSET 0 -- the completion casts (op *)rpc
    struct dev9p_poll_state *ps;        // the poll-state this op probes for
    struct Spoor            *pinned;    // spoor_ref on the readiness Spoor (=> client alive)
    struct p9_client        *client;    // borrowed (valid while `pinned` held)
    u32                      fid;        // the readiness file's 9P fid
    u16                      mask;       // the event mask = the Tread offset
    bool                     terminal;   // completion fired (atomic; reaped by the kthread)
    struct dev9p_poll_op    *next;       // g_dev9p_poll_ops chain (under g_dev9p_poll_lock)
};

_Static_assert(__builtin_offsetof(struct dev9p_poll_op, rpc) == 0,
               "p9_rpc must be at offset 0 -- dev9p_poll_complete recovers the container");

// Per-readiness-Spoor poll state. Lazily allocated by dev9p_poll the first time a
// QTPOLL Spoor is polled; hung off dev9p_priv->poll; freed at dev9p_close.
// Multi-thread-Proc-reachable (handle_dup shares the Spoor -> the same priv -> the
// same poll-state): poll_list has its own lock; cached_revents is atomic; `op` +
// `wanted_mask` are under g_dev9p_poll_lock.
struct dev9p_poll_state {
    struct poll_waiter_list  poll_list;     // pollers' hooks (own lock)
    u32                      cached_revents; // atomic: DEV9P_POLL_VALID | revents
    struct dev9p_poll_op    *op;            // the outstanding op (under g_lock); NULL = none
    u16                      wanted_mask;    // union of pollers' events for the live op (g_lock)
};

#define DEV9P_POLL_VALID    0x00010000u   // cached_revents bit 16: a completion recorded
#define DEV9P_POLL_RV_MASK  0x0000ffffu   // the revents bits (POLLIN/OUT/ERR/HUP fit in 16)
#define DEV9P_POLL_IDLE_NS  (20ull * 1000ull * 1000ull)   // 20ms reader-pump idle deadline

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
                                     struct p9_client *client, u32 fid, u16 wanted) {
    // op_in is freshly kmalloc(KP_ZERO)'d and op_in->pinned was already set by the
    // caller (spoor_ref(c)); set only the non-zero fields.
    op_in->rpc.on_complete = dev9p_poll_complete;
    op_in->ps       = ps;
    op_in->client   = client;
    op_in->fid      = fid;
    op_in->mask     = wanted;
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
    struct dev9p_poll_state *ps = p->poll;
    if (!ps) {
        struct dev9p_poll_state *cand = kmalloc(sizeof(*cand), KP_ZERO);
        if (!cand) {
            // OOM: degrade to always-ready (fail-safe; a poll-state alloc failure
            // can never make poll() wrong, only non-blocking-on-this-fd).
            return (short)(events & POLL_REQUESTABLE);
        }
        poll_waiter_list_init(&cand->poll_list);
        spin_lock(&g_dev9p_poll_lock);
        if (!p->poll) p->poll = cand;     // win: publish
        ps = p->poll;
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
            spoor_ref(c);                  // the op's I-30 pin (=> Spoor + client alive)
            cand_op->pinned = c;
            dev9p_poll_submit_locked(ps, cand_op, p->client, p->fid, ps->wanted_mask);
            cand_op = NULL;                // consumed
            woke = true;
            // A synchronous submit failure fired the completion (POLLERR) under
            // c->lock during submit_async -- re-read to surface it without a park.
            cached = __atomic_load_n(&ps->cached_revents, __ATOMIC_ACQUIRE);
            ready_now = (cached & DEV9P_POLL_VALID)
                      ? (u16)(cached & DEV9P_POLL_RV_MASK & want) : 0u;
        }
        // cand_op == NULL on OOM: no fresh probe. The hook is still registered; a
        // live op (if any) still wakes us; else this poll falls through not-ready
        // and the caller re-polls (degraded, never unsound).
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
        // Cancel the widened-away op at the client (Tflush; #845) THEN release its
        // pin -- outside g_lock (abandon takes c->lock; spoor_clunk may sleep).
        p9_client_abandon_async(abandon->client, &abandon->rpc);
        spoor_clunk(abandon->pinned);
        kfree(abandon);
    }
    if (woke) (void)wakeup(&g_dev9p_poll_rendez);

    return (short)ready_now;
}

// =============================================================================
// The global poll-pump kthread (KthreadWalk).
// =============================================================================

// Borrow the client of the first non-terminal op (the Loom loom_first_inflight_client
// pattern): take an EXTRA spoor_ref on its pin so the Spoor (=> client) stays alive
// past the unlock + across the blocking pump, even if a concurrent reaper frees the
// op. *pin_out is always written (NULL when none). Under g_dev9p_poll_lock.
static struct p9_client *dev9p_poll_borrow_client(struct Spoor **pin_out) {
    struct p9_client *cl = NULL;
    *pin_out = NULL;
    spin_lock(&g_dev9p_poll_lock);
    for (struct dev9p_poll_op *op = g_dev9p_poll_ops; op; op = op->next) {
        if (!__atomic_load_n(&op->terminal, __ATOMIC_ACQUIRE)) {
            cl = op->client;
            *pin_out = op->pinned;
            spoor_ref(op->pinned);   // borrow-guard
            break;
        }
    }
    spin_unlock(&g_dev9p_poll_lock);
    return cl;
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
        poll_waiter_list_wake(&reap->ps->poll_list);   // process context
        spoor_clunk(reap->pinned);
        kfree(reap);
        reap = next;
    }

    // Phase 2b (outside g_lock): cancel stranded ops at the client (Tflush) +
    // release pins. No poller cares, so no poll_list walk.
    while (abandon) {
        struct dev9p_poll_op *next = abandon->next;
        p9_client_abandon_async(abandon->client, &abandon->rpc);
        spoor_clunk(abandon->pinned);
        kfree(abandon);
        abandon = next;
    }

    // Phase 3: pump the elected reader of a client with a remaining live op, else
    // park. Borrow the client from the live op (the Spoor pin keeps it alive past
    // the unlock + across the blocking pump).
    struct Spoor *pin = NULL;
    struct p9_client *cl = dev9p_poll_borrow_client(&pin);
    if (cl) {
        u64 deadline = timer_now_ns() + DEV9P_POLL_IDLE_NS;
        int rc = p9_client_reader_pump_once_deadline(cl, deadline);
        spoor_clunk(pin);                  // release the borrow-guard
        if (rc == P9_PUMP_BUSY) sched();    // a sync reader holds the role -- yield
        // PROGRESS: demuxed a frame (a completion may have set an op terminal).
        // IDLE: the boundary deadline lapsed, stream synced. DEAD:
        // client_mark_dead_locked already error-completed every async op (each
        // dev9p_poll_complete fired -P9_E_IO -> POLLERR cached + terminal), so the
        // next cycle reaps them (waking pollers POLLERR) + parks. All cases: loop.
    } else {
        // Nothing outstanding -> park. The cond re-checks the registry count under
        // the rendez lock (register-then-observe vs a concurrent submit's wake).
        // kproc never group-terminates, so SLEEP_INTR (a defensive death-interrupt)
        // just re-loops -- there is no caller state to unwind.
        (void)sleep(&g_dev9p_poll_rendez, dev9p_poll_park_cond, NULL);
    }
}

void dev9p_poll_pump_main(void) {
    for (;;) dev9p_poll_service_once();
}

// =============================================================================
// Teardown.
// =============================================================================

void dev9p_poll_priv_release(struct dev9p_priv *p) {
    if (!p || !p->poll) return;
    // dev9p_close runs only at the Spoor's LAST ref. An outstanding op holds a pin
    // (a ref) on the Spoor, and a registered poller holds the handle's obj ref, so
    // neither can be live here -> ps->op == NULL + poll_list empty. Free the state.
    struct dev9p_poll_state *ps = p->poll;
    if (ps->op != NULL) {
        // Defense in depth: a live op at close would be a lifetime-invariant
        // violation (the pin should have deferred this close). Loudly fail rather
        // than leak/UAF -- this can only fire on a refcount bug.
        extinction("dev9p_poll_priv_release: poll-state freed with a live op");
    }
    p->poll = NULL;
    kfree(ps);
}
