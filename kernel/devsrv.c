// devsrv — the /srv service registry Dev + per-connection layer.
//
// Per ARCHITECTURE.md §9.4 + CORVUS-DESIGN.md §6. `/srv` is a kernel Dev
// (Plan 9's `#s`) by which a userspace 9P server registers a name with
// SYS_POST_SERVICE; the kernel mints + mediates per-connection client
// access. `devsrv` is deliberately distinct from `dev9p` so a Spoor
// walked out of `/srv` is structurally a KObj_Srv kernel object —
// non-transferable, which keeps the kernel-stamped peer identity behind
// it unforgeable.
//
// P5-corvus-srv-impl-a2 landed the service registry + the devsrv Dev's
// attach + SYS_POST_SERVICE. P5-corvus-srv-impl-a3b adds the
// per-connection layer:
//   - the accept backlog on struct SrvService;
//   - srv_conn_open_for_proc — the client-connect path: mint a SrvConn,
//     enqueue it, install the client's KObj_Srv connection handle;
//   - srv_accept_blocking — the poster's blocking accept (SYS_SRV_ACCEPT);
//   - the devsrv walk op + the connection-Spoor read/write/close ops;
//   - srv_proc_exit_notify draining a dead poster's backlog.
//
// Spec: specs/corvus.tla — MarkMayPost / PostService / ServiceTombstone /
// SrvBind / SrvAccept / ProcExit; specs/handles.tla — KObj_Srv.

#include <thylacine/dev.h>
#include <thylacine/devsrv.h>
#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/page.h>
#include <thylacine/poll.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/srvconn.h>
#include <thylacine/types.h>

#include "../mm/slub.h"

// The registry. Static storage zero-initializes every entry's `state` to
// SRV_STATE_FREE (== 0), the lock to the all-zero SPIN_LOCK_INIT form,
// and each accept Rendez to {unlocked, no waiter}.
struct SrvRegistry {
    spin_lock_t       lock;
    struct SrvService entries[SRV_MAX_SERVICES];
};
static struct SrvRegistry g_srv_registry;

// =============================================================================
// Registry internals.
// =============================================================================

// Length-bounded name equality. Service names are NOT NUL-terminated in
// the registry — name_len is authoritative.
static bool srv_name_eq(const char *a, u8 alen, const char *b, u8 blen) {
    if (alen != blen) return false;
    for (u8 i = 0; i < alen; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

// Find a non-FREE entry by name. PRECONDITION: caller holds the registry
// lock. Returns the entry (LIVE / RESERVING / TOMBSTONED) or NULL.
static struct SrvService *srv_find_locked(const char *name, u8 name_len) {
    for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
        struct SrvService *e = &g_srv_registry.entries[i];
        if (e->state == SRV_STATE_FREE) continue;
        if (srv_name_eq(e->name, e->name_len, name, name_len)) return e;
    }
    return NULL;
}

// Wipe an entry back to FREE. PRECONDITION: caller holds the registry lock.
//
// `magic` is NOT cleared — a SrvService's magic is a permanent struct
// type tag (stamped once by devsrv_init), never a liveness bit: `state`
// tracks FREE/RESERVING/LIVE/TOMBSTONED. A KObj_Srv service handle may
// outlive its entry's LIVE state, and handle_release_obj must still read
// SRV_SERVICE_MAGIC at offset 0 to discriminate it from a connection.
//
// The backlog ARRAY is left as-is — callers (srv_abort on a RESERVING
// entry, which never reached LIVE so never enqueued a connection;
// srv_registry_reset after draining) guarantee no live SrvConn reference
// is dropped here. The accept Rendez is left untouched: sleep / wakeup
// own its `waiter` field, and clobbering it would strand a sleeper.
static void srv_clear_locked(struct SrvService *e) {
    e->state          = SRV_STATE_FREE;
    e->name_len       = 0;
    for (u32 i = 0; i < SRV_NAME_MAX; i++) e->name[i] = 0;
    e->poster_stripes = 0;
    e->poster_pid     = 0;
    e->backlog_head   = 0;
    e->backlog_tail   = 0;
    e->backlog_count  = 0;
}

// Push a connection onto an entry's accept backlog. PRECONDITION: caller
// holds the registry lock. The caller transfers ownership of one
// srvconn_ref to the backlog slot. Returns 0 on success, -1 if the entry
// is no longer LIVE (re-checked here atomically with the push) or the
// backlog is full.
static int srv_backlog_push_locked(struct SrvService *e, struct SrvConn *cn) {
    if (e->state != SRV_STATE_LIVE)                 return -1;
    if (e->backlog_count >= SRV_ACCEPT_BACKLOG)     return -1;
    e->backlog[e->backlog_head] = cn;
    e->backlog_head = (e->backlog_head + 1) % SRV_ACCEPT_BACKLOG;
    e->backlog_count++;
    return 0;
}

// Pop the oldest connection off an entry's accept backlog. PRECONDITION:
// caller holds the registry lock. Returns the SrvConn (ownership of its
// backlog ref passes to the caller) or NULL if the backlog is empty.
static struct SrvConn *srv_backlog_pop_locked(struct SrvService *e) {
    if (e->backlog_count == 0) return NULL;
    struct SrvConn *cn = e->backlog[e->backlog_tail];
    e->backlog[e->backlog_tail] = NULL;
    e->backlog_tail = (e->backlog_tail + 1) % SRV_ACCEPT_BACKLOG;
    e->backlog_count--;
    return cn;
}

// =============================================================================
// Registry API — the reserve / commit / abort two-phase post.
// =============================================================================

int srv_reserve(const char *name, u8 name_len, struct Proc *poster,
                struct SrvService **svc_out, enum srv_state *prior_out) {
    if (!name || name_len == 0 || name_len > SRV_NAME_MAX) return -1;
    if (!svc_out || !prior_out)                            return -1;

    // proc_stripes fail-closes to 0 for a NULL / corrupted poster; a Proc
    // the kernel cannot identify cannot post a service (and 0 would alias
    // the fail-closed sentinel srv_proc_exit_notify matches against).
    u64 stripes = proc_stripes(poster);
    if (stripes == 0) return -1;

    irq_state_t s = spin_lock_irqsave(&g_srv_registry.lock);

    struct SrvService *e = srv_find_locked(name, name_len);
    if (e) {
        // The name exists. A LIVE or RESERVING entry must not be displaced
        // — no stealing a running or in-flight server. Only a TOMBSTONED
        // name (prior poster exited) is re-postable.
        if (e->state != SRV_STATE_TOMBSTONED) {
            spin_unlock_irqrestore(&g_srv_registry.lock, s);
            return -1;
        }
        *prior_out = SRV_STATE_TOMBSTONED;
    } else {
        // Fresh post — claim a FREE slot.
        for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
            if (g_srv_registry.entries[i].state == SRV_STATE_FREE) {
                e = &g_srv_registry.entries[i];
                break;
            }
        }
        if (!e) {
            spin_unlock_irqrestore(&g_srv_registry.lock, s);
            return -1;   // registry full
        }
        *prior_out = SRV_STATE_FREE;
    }

    // e->magic is already SRV_SERVICE_MAGIC — devsrv_init stamped every
    // registry entry once; it is a permanent type tag, never cleared.
    e->state          = SRV_STATE_RESERVING;
    e->name_len       = name_len;
    for (u8 i = 0; i < name_len; i++) e->name[i] = name[i];
    e->poster_stripes = stripes;
    e->poster_pid     = poster->pid;
    *svc_out = e;

    spin_unlock_irqrestore(&g_srv_registry.lock, s);
    return 0;
}

void srv_commit(struct SrvService *svc) {
    if (!svc)                          extinction("srv_commit(NULL)");
    if (svc->magic != SRV_SERVICE_MAGIC)
        extinction("srv_commit: bad service magic (wild pointer / corruption)");

    irq_state_t s = spin_lock_irqsave(&g_srv_registry.lock);
    if (svc->state != SRV_STATE_RESERVING)
        extinction("srv_commit: entry not RESERVING (double commit / lifecycle bug)");
    svc->state = SRV_STATE_LIVE;
    spin_unlock_irqrestore(&g_srv_registry.lock, s);
}

void srv_abort(struct SrvService *svc, enum srv_state prior) {
    if (!svc)                          extinction("srv_abort(NULL)");
    if (svc->magic != SRV_SERVICE_MAGIC)
        extinction("srv_abort: bad service magic (wild pointer / corruption)");
    if (prior != SRV_STATE_FREE && prior != SRV_STATE_TOMBSTONED)
        extinction("srv_abort: prior state must be FREE or TOMBSTONED");

    irq_state_t s = spin_lock_irqsave(&g_srv_registry.lock);
    if (svc->state != SRV_STATE_RESERVING)
        extinction("srv_abort: entry not RESERVING (lifecycle bug)");
    if (prior == SRV_STATE_FREE) {
        // A fresh post that failed leaves no trace. A RESERVING entry never
        // reached LIVE, so it never enqueued a connection — the backlog is
        // empty and srv_clear_locked is safe.
        srv_clear_locked(svc);
    } else {
        // A rebind that failed — restore the tombstone. The name stays
        // reserved; a tombstone carries no live poster identity.
        svc->state          = SRV_STATE_TOMBSTONED;
        svc->poster_stripes = 0;
        svc->poster_pid     = 0;
    }
    spin_unlock_irqrestore(&g_srv_registry.lock, s);
}

struct SrvService *srv_lookup(const char *name, u8 name_len) {
    if (!name || name_len == 0 || name_len > SRV_NAME_MAX) return NULL;
    irq_state_t s = spin_lock_irqsave(&g_srv_registry.lock);
    struct SrvService *e = srv_find_locked(name, name_len);
    spin_unlock_irqrestore(&g_srv_registry.lock, s);
    return e;
}

void srv_proc_exit_notify(struct Proc *p) {
    // proc_stripes fail-closes to 0; a LIVE poster always carries a
    // non-zero stripes tag, so a 0 here matches nothing.
    u64 stripes = proc_stripes(p);
    if (stripes == 0) return;

    // Collect this poster's pending connections + tombstoned services so
    // the heavy work (srvconn_teardown / srvconn_unref take the SrvConn's
    // own locks; wakeup takes a Rendez lock) runs OUTSIDE the registry
    // lock — the srvconn.c teardown discipline keeps those off any path
    // that re-enters the registry lock. A Proc may post more than one
    // service (test_devsrv post_basic posts two), so every matching entry
    // is tombstoned, not just the first.
    struct SrvConn    *drained[SRV_MAX_SERVICES * SRV_ACCEPT_BACKLOG];
    u32                n_drained = 0;
    struct SrvService *tombed[SRV_MAX_SERVICES];
    u32                n_tombed = 0;

    irq_state_t s = spin_lock_irqsave(&g_srv_registry.lock);
    for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
        struct SrvService *e = &g_srv_registry.entries[i];
        if (e->state == SRV_STATE_LIVE && e->poster_stripes == stripes) {
            // Tombstone: the name stays reserved (re-postable only by a
            // joey-marked Proc — CORVUS-DESIGN.md §6.1), the dead poster
            // identity is cleared so a tombstone carries no live poster.
            e->state          = SRV_STATE_TOMBSTONED;
            e->poster_stripes = 0;
            e->poster_pid     = 0;
            // Drain the accept backlog: no live server remains to accept
            // these connections, so each is torn down (its client wakes
            // with EOF rather than hanging on a dead server).
            for (;;) {
                struct SrvConn *cn = srv_backlog_pop_locked(e);
                if (!cn) break;
                drained[n_drained++] = cn;
            }
            tombed[n_tombed++] = e;
        }
    }
    spin_unlock_irqrestore(&g_srv_registry.lock, s);

    for (u32 i = 0; i < n_drained; i++) {
        srvconn_teardown(drained[i]);   // EOF both rings — wakes the client
        srvconn_unref(drained[i]);      // drop the backlog reference
    }
    // Wake any thread blocked accepting on a tombstoned service — it sees
    // state != LIVE and srv_accept_blocking returns NULL. Defensive: the
    // exiting poster is normally its service's only accepter.
    //
    // Also wake every listener poller (P5-poll-b): a tombstone surfaces
    // POLLHUP on the listener. specs/poll.tla MakeReady.
    for (u32 i = 0; i < n_tombed; i++) {
        wakeup(&tombed[i]->accept_rendez);
        poll_waiter_list_wake(&tombed[i]->poll_list);
    }
}

int srv_registry_count(void) {
    int n = 0;
    irq_state_t s = spin_lock_irqsave(&g_srv_registry.lock);
    for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
        if (g_srv_registry.entries[i].state != SRV_STATE_FREE) n++;
    }
    spin_unlock_irqrestore(&g_srv_registry.lock, s);
    return n;
}

int srv_backlog_depth(struct SrvService *svc) {
    if (!svc || svc->magic != SRV_SERVICE_MAGIC) return -1;
    irq_state_t s = spin_lock_irqsave(&g_srv_registry.lock);
    int n = (int)svc->backlog_count;
    spin_unlock_irqrestore(&g_srv_registry.lock, s);
    return n;
}

// srv_registry_reset — TEST SUPPORT ONLY. Wipe the registry to all-FREE,
// draining (tearing down) every pending connection first. Deliberately
// NOT declared in devsrv.h: no production caller exists; the in-kernel
// test harness extern-declares it so each devsrv test starts from an
// empty registry.
void srv_registry_reset(void) {
    struct SrvConn *drained[SRV_MAX_SERVICES * SRV_ACCEPT_BACKLOG];
    u32             n_drained = 0;
    // Snapshot the per-entry accept Rendez + poll-list pointers BEFORE
    // clearing — a thread blocked in srv_accept_blocking or polling the
    // listener must be woken so it observes state != LIVE and returns.
    struct Rendez           *waked[SRV_MAX_SERVICES];
    struct poll_waiter_list *poll_waked[SRV_MAX_SERVICES];

    irq_state_t s = spin_lock_irqsave(&g_srv_registry.lock);
    for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
        struct SrvService *e = &g_srv_registry.entries[i];
        for (;;) {
            struct SrvConn *cn = srv_backlog_pop_locked(e);
            if (!cn) break;
            drained[n_drained++] = cn;
        }
        srv_clear_locked(e);
        waked[i]      = &e->accept_rendez;
        poll_waked[i] = &e->poll_list;
    }
    spin_unlock_irqrestore(&g_srv_registry.lock, s);

    for (u32 i = 0; i < n_drained; i++) {
        srvconn_teardown(drained[i]);
        srvconn_unref(drained[i]);
    }
    for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
        wakeup(waked[i]);
        poll_waiter_list_wake(poll_waked[i]);
    }
}

// =============================================================================
// Per-connection layer (P5-corvus-srv-impl-a3b).
// =============================================================================

// Per-Proc cap on LIVE `/srv` client connections (CORVUS-DESIGN.md §6.2 —
// "One connection per Proc"). At v1.0 = 1; future v1.x may lift to per-
// service counters if multiple `/srv` services exist.
#define SRV_CONN_PER_PROC_MAX  1u

int srv_conn_open_for_proc(struct Proc *p, const char *name, u8 name_len) {
    if (!p)                                                return -1;
    if (!name || name_len == 0 || name_len > SRV_NAME_MAX)  return -1;

    // Per-Proc one-connection cap (P5-corvus-srv-impl-b2). Fail FIRST,
    // before any allocation, so a Proc that already holds a connection
    // can't burn create/teardown cycles trying to mint a second. At v1.0
    // single-thread-per-Proc makes the read + increment safe without
    // atomicity (no concurrent mutator on the same Proc); multi-thread-
    // per-Proc would need an atomic CAS.
    if (p->srv_conn_count >= SRV_CONN_PER_PROC_MAX)         return -1;

    // Global live-connection cap. (created - freed) is the live SrvConn
    // count; the read is racy against concurrent create/free, which is
    // fine for a soft resource cap — the hard, per-service bound is the
    // accept backlog, enforced under the registry lock below.
    if (srvconn_total_created() - srvconn_total_freed() >= SRV_MAX_CONNS)
        return -1;

    // Resolve the service. Only a LIVE service has a server to accept the
    // connection; a missing / RESERVING / TOMBSTONED name fails fast (and
    // spares the heavy mint below). The push under the registry lock
    // re-checks LIVE, so this is an optimization, not the correctness
    // gate.
    struct SrvService *svc = srv_lookup(name, name_len);
    if (!svc) return -1;

    // Capture the poster's stripes under the registry lock, atomically
    // with the LIVE check: poster_stripes is stable while LIVE (set at
    // reserve, zeroed only by the tombstone). It becomes the connection's
    // server identity — SYS_SRV_PEER's poster gate (a3c).
    u64 poster_stripes;
    {
        irq_state_t s = spin_lock_irqsave(&g_srv_registry.lock);
        bool live      = (svc->state == SRV_STATE_LIVE);
        poster_stripes = svc->poster_stripes;
        spin_unlock_irqrestore(&g_srv_registry.lock, s);
        if (!live) return -1;
    }

    // Mint the connection — the peer AND server identity are captured BY
    // VALUE here (CORVUS-DESIGN.md §6.3): the SrvConn holds no raw Proc*
    // for p and no SrvService* for the poster, so neither a p that exits
    // and is reaped nor a tombstone-then-rebind turns a later connection
    // read into a UAF.
    struct SrvConn *cn = srvconn_create(proc_stripes(p), p->pid,
                                        proc_is_console_attached(p),
                                        poster_stripes);
    if (!cn) return -1;

    // Install the client's KObj_Srv connection handle. handle_alloc does
    // not take a reference (only handle_dup does) — so the SrvConn's
    // create-reference IS this handle's reference; handle_release_obj's
    // KOBJ_SRV case srvconn_unref's it on close. KObj_Srv is
    // non-transferable: handle_dup rejects it (NoSrvDup), so the
    // connection is pinned to p (handles.tla SrvHandlesAtOrigin).
    hidx_t h = handle_alloc(p, KOBJ_SRV, RIGHT_READ | RIGHT_WRITE, cn);
    if (h < 0) {
        srvconn_unref(cn);          // create-ref → 0 → teardown + free
        return -1;
    }

    // Per-Proc cap counter — count this live SrvConn handle. The matching
    // decrement is in handle_close's KOBJ_SRV SRV_CONN_MAGIC arm; proc
    // exit's bulk-close path (handle_release_obj only — no Proc) doesn't
    // decrement, which is fine because the Proc itself is being torn down.
    p->srv_conn_count++;

    // A second reference for the accept-backlog slot, then enqueue. The
    // push re-validates LIVE atomically with the slot write.
    srvconn_ref(cn);
    irq_state_t s = spin_lock_irqsave(&g_srv_registry.lock);
    int rc = srv_backlog_push_locked(svc, cn);
    spin_unlock_irqrestore(&g_srv_registry.lock, s);
    if (rc != 0) {
        // Service died between the pre-check and the push, or the backlog
        // is full. Drop the backlog ref, then close the handle (whose
        // release drops the create-ref → teardown + free, AND decrements
        // p->srv_conn_count via the SRV_CONN_MAGIC arm of handle_close).
        srvconn_unref(cn);
        handle_close(p, h);
        return -1;
    }

    // Wake a poster blocked in SYS_SRV_ACCEPT. Outside the registry lock —
    // wakeup takes the Rendez lock; the producer-mutates-then-wakeup
    // ordering is the srvconn.c chan_produce discipline.
    wakeup(&svc->accept_rendez);

    // Also wake every listener poller (P5-poll-b): backlog_count went 0→1+
    // (or stayed >0). specs/poll.tla MakeReady — the push committed under
    // the registry lock above, the wake walks the poll list under its own
    // lock, the lock chain registry → poll_list is acyclic.
    poll_waiter_list_wake(&svc->poll_list);
    return (int)h;
}

// accept_cond_is_ready — sleep()'s wait predicate for srv_accept_blocking.
// Reads backlog_count / state WITHOUT the registry lock: sleep evaluates
// it under the accept Rendez lock, and every producer (srv_conn_open_for_
// proc's push, srv_proc_exit_notify's tombstone) mutates these fields
// under the registry lock and then calls wakeup(), whose Rendez-lock
// acquisition provides the happens-before. The discipline matches
// srvconn.c's chan_cond_readable.
static int accept_cond_is_ready(void *arg) {
    struct SrvService *svc = (struct SrvService *)arg;
    return (svc->backlog_count > 0) || (svc->state != SRV_STATE_LIVE);
}

struct SrvConn *srv_accept_blocking(struct SrvService *svc) {
    if (!svc || svc->magic != SRV_SERVICE_MAGIC) return NULL;

    for (;;) {
        irq_state_t s = spin_lock_irqsave(&g_srv_registry.lock);
        struct SrvConn *cn  = srv_backlog_pop_locked(svc);
        enum srv_state  st  = svc->state;
        spin_unlock_irqrestore(&g_srv_registry.lock, s);

        if (cn) return cn;                 // dequeued — ownership to caller
        if (st != SRV_STATE_LIVE) return NULL;   // service gone; give up

        // Empty + LIVE: block until a client opens (a wakeup from the
        // push) or the service stops being LIVE (a wakeup from the
        // tombstone). sleep re-checks accept_cond under the Rendez lock —
        // a wakeup between the unlock above and the sleep transition is
        // not lost (specs/scheduler.tla NoMissedWakeup).
        sleep(&svc->accept_rendez, accept_cond_is_ready, svc);
    }
}

struct Spoor *devsrv_make_conn_spoor(struct SrvConn *cn) {
    if (!cn) return NULL;
    struct Spoor *c = spoor_alloc(&devsrv);
    if (!c) return NULL;
    // The SrvConn itself is the Spoor's aux — SRV_CONN_MAGIC at its
    // offset 0 self-identifies it (devsrv_close discriminates on it). The
    // caller's SrvConn reference becomes the Spoor's; devsrv_close drops
    // it. An accepted connection is pre-opened — corvus reads Tmsg / writes
    // Rmsg on it directly through SYS_READ / SYS_WRITE.
    c->aux      = cn;
    c->qid.type = QTFILE;
    c->qid.path = 0;
    c->qid.vers = 0;
    c->flag    |= COPEN;
    c->mode     = 2;            // ORDWR
    c->offset   = 0;
    return c;
}

// =============================================================================
// The devsrv Dev.
// =============================================================================

static void devsrv_reset(void)    { /* no-op */ }

// init — stamp the type tag + initialize the listener poll list on every
// (static, zero-initialized) registry entry. A SrvService's magic is
// permanent: it identifies the struct for the KObj_Srv handle-release
// discriminator and is never cleared (see srv_clear_locked). poll_list
// is similarly permanent (the list lock is the all-zero SPIN_LOCK_INIT
// from BSS so the explicit init is the documented contract, mirroring
// kernel/pipe.c::pipe_create's poll_waiter_list_init). dev_init calls
// this once at boot.
static void devsrv_init(void) {
    for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
        g_srv_registry.entries[i].magic = SRV_SERVICE_MAGIC;
        poll_waiter_list_init(&g_srv_registry.entries[i].poll_list);
    }
}

static void devsrv_shutdown(void) { /* no-op */ }

// attach — produce the /srv root directory Spoor. `spec` is unused: /srv
// is a single root, not a spec-selected namespace.
static struct Spoor *devsrv_attach(const char *spec) {
    (void)spec;
    return dev_simple_attach(&devsrv, QTDIR);
}

// devsrv_conn_of — the SrvConn behind a connection Spoor, or NULL if `c`
// is not one. A devsrv Spoor's aux discriminates its flavor: NULL for the
// /srv root, a struct devsrv_svc_ref (DEVSRV_SVC_MAGIC) for a service
// Spoor, or a struct SrvConn (SRV_CONN_MAGIC) for an accepted connection.
// Non-static — SYS_SRV_PEER (kernel/syscall.c) resolves a connection
// endpoint handle through here; declared in <thylacine/devsrv.h>.
struct SrvConn *devsrv_conn_of(struct Spoor *c) {
    if (!c || c->dc != 's' || !c->aux)         return NULL;
    if (*(const u64 *)c->aux != SRV_CONN_MAGIC) return NULL;
    return (struct SrvConn *)c->aux;
}

// walk — only the /srv root walks, and only one component deep: a walk of
// /srv/<name> yields a service Spoor naming a LIVE posted service. The
// service Spoor is structurally KObj_Srv (it carries devsrv's dc='s').
// Walking out of a service or connection Spoor (the client-side
// /srv/<name>/<path> walk) is P5-corvus-srv-impl-b.
static struct Walkqid *devsrv_walk(struct Spoor *c, struct Spoor *nc,
                                   const char **name, int nname) {
    if (!c || c->dc != 's' || c->aux != NULL) return NULL;   // root only
    if (!nc || nname < 0)                     return NULL;

    if (nname == 0) {
        // Clone — nc is the caller's shallow copy of the root (aux NULL,
        // so nc->aux is NULL too: nc stays a root Spoor).
        struct Walkqid *w = walkqid_alloc(1);
        if (!w) return NULL;
        w->nqid  = 0;
        w->spoor = nc;
        return w;
    }
    if (nname != 1) return NULL;        // no /srv/<name>/<path> nesting yet

    // name[i] is NUL-terminated (the Dev walk contract); the explicit cap
    // bounds the scan.
    const char *s = name[0];
    if (!s) return NULL;
    u32 len = 0;
    while (len < SRV_NAME_MAX && s[len] != '\0') len++;
    if (len == 0 || s[len] != '\0') return NULL;   // empty or over-long

    struct SrvService *svc = srv_lookup(s, (u8)len);
    if (!svc) return NULL;
    {
        irq_state_t st = spin_lock_irqsave(&g_srv_registry.lock);
        bool live = (svc->state == SRV_STATE_LIVE);
        spin_unlock_irqrestore(&g_srv_registry.lock, st);
        if (!live) return NULL;        // only a LIVE service is walkable
    }

    struct devsrv_svc_ref *ref = kmalloc(sizeof(*ref), KP_ZERO);
    if (!ref) return NULL;
    ref->magic    = DEVSRV_SVC_MAGIC;
    ref->name_len = (u8)len;
    for (u32 i = 0; i < len; i++) ref->name[i] = s[i];

    struct Walkqid *w = walkqid_alloc(1);
    if (!w) {
        kfree(ref);
        return NULL;
    }

    nc->aux      = ref;        // nc's shallow-copied root aux was NULL
    nc->qid.type = QTFILE;
    nc->qid.path = 0;
    nc->qid.vers = 0;
    w->nqid    = 1;
    w->qid[0]  = nc->qid;
    w->spoor   = nc;
    return w;
}

static int devsrv_stat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

// open — a3b leaves this a graceful-fail stub. The /srv client-connect
// (an open of /srv/<name>) yields a KObj_Srv handle whose obj is a
// SrvConn — not a Spoor — so it cannot be expressed through the Dev
// `open` vtable, which returns a Spoor. The client-connect core is
// srv_conn_open_for_proc(); the production client-open syscall that
// routes a namespace open into it lands at P5-corvus-srv-impl-b with the
// joey /srv mount.
static struct Spoor *devsrv_open(struct Spoor *c, int omode) {
    (void)c; (void)omode;
    return NULL;
}

static void devsrv_create(struct Spoor *c, const char *name, int omode, u32 perm) {
    (void)c; (void)name; (void)omode; (void)perm;
    // no-op — /srv entries are posted via SYS_POST_SERVICE, not 9P create.
}

// close — release per-Spoor state. The /srv root holds none; a service
// Spoor holds a kmalloc'd devsrv_svc_ref; a connection Spoor holds a
// SrvConn reference. Closing a connection Spoor is a connection close
// (CORVUS-DESIGN.md §6.2): tear the connection down so the peer wakes,
// then release the reference.
static void devsrv_close(struct Spoor *c) {
    if (!c || c->dc != 's' || !c->aux) return;   // root Spoor — no-op
    u64 m = *(const u64 *)c->aux;
    if (m == SRV_CONN_MAGIC) {
        srvconn_teardown((struct SrvConn *)c->aux);
        srvconn_unref((struct SrvConn *)c->aux);
    } else if (m == DEVSRV_SVC_MAGIC) {
        struct devsrv_svc_ref *ref = (struct devsrv_svc_ref *)c->aux;
        ref->magic = 0;
        kfree(ref);
    } else {
        extinction("devsrv_close: Spoor aux has unknown magic (corruption)");
    }
    c->aux = NULL;
}

// read — a connection Spoor's read drains the c2s ring (the bytes the
// kernel 9P client sent toward corvus). Returns >0 bytes, 0 if the ring
// is empty but the connection is live (corvus polls again), or -1 on EOF
// (the connection is torn down). The /srv root and service Spoors are not
// readable. `off` is ignored — a connection is a byte stream.
static long devsrv_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)off;
    struct SrvConn *cn = devsrv_conn_of(c);
    if (!cn) return -1;
    return srvconn_server_recv(cn, (u8 *)buf, n);
}

static struct Block *devsrv_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

// write — a connection Spoor's write fills the s2c ring (corvus's Rmsg
// bytes toward the kernel 9P client). Returns bytes accepted or -1 if the
// connection is torn down. The /srv root and service Spoors are not
// writable.
static long devsrv_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)off;
    struct SrvConn *cn = devsrv_conn_of(c);
    if (!cn) return -1;
    return srvconn_server_send(cn, (const u8 *)buf, n);
}

static long devsrv_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

static void devsrv_remove(struct Spoor *c) {
    (void)c;
    // no-op
}

static int devsrv_wstat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devsrv_power(struct Spoor *c, int on) {
    (void)c; (void)on;
    return NULL;
}

// =============================================================================
// poll — readiness probe (P5-poll-b).
// =============================================================================
//
// devsrv_poll (the Dev vtable slot) routes a connection Spoor's poll to
// srvconn_poll; the root / service-ref Spoors are not readable directly,
// so they report no readiness state (return 0 — POSIX-equivalent to "this
// fd never has data" so a poller with timeout will block until timeout).
// The corvus listener-poll path goes through srv_handle_poll, NOT here:
// the listener is a KObj_Srv handle whose obj is a SrvService, not a
// Spoor — poll.c's KObj_Srv branch resolves it through srv_handle_poll.

// svc_listener_poll — listener-readiness probe on a SrvService. POLLIN
// when the accept backlog has one or more waiting connections (corvus
// has something to accept); POLLHUP when the service is no longer LIVE
// (tombstoned by srv_proc_exit_notify or wiped by srv_registry_reset).
//
// The sample-and-register are both atomic under the registry lock; the
// producer side (srv_conn_open_for_proc → srv_backlog_push_locked under
// the registry lock, then poll_waiter_list_wake after release;
// srv_proc_exit_notify under the registry lock, then poll_waiter_list_
// wake after release) commits the readiness change under the same lock.
static short svc_listener_poll(struct SrvService *svc, short events,
                               struct poll_waiter *pw) {
    if (!svc) return POLLERR;
    if (svc->magic != SRV_SERVICE_MAGIC) {
        extinction("svc_listener_poll: bad service magic (UAF / wild ptr?)");
    }

    short revents = 0;

    irq_state_t s = spin_lock_irqsave(&g_srv_registry.lock);
    if (svc->backlog_count > 0)        revents |= POLLIN;
    if (svc->state != SRV_STATE_LIVE)  revents |= POLLHUP;
    if (pw) {
        poll_waiter_list_register(&svc->poll_list, pw);
    }
    spin_unlock_irqrestore(&g_srv_registry.lock, s);

    // POSIX: POLLIN/POLLOUT only when requested; POLLHUP always. POLLOUT
    // is undefined for a listener — never set.
    return (short)(revents & (events | POLL_OUTPUT_ONLY));
}

short srv_handle_poll(void *obj, short events, struct poll_waiter *pw) {
    (void)events; (void)pw;
    if (!obj) return POLLNVAL;
    // The first u64 of a KObj_Srv obj is its struct's magic word — see
    // SRV_SERVICE_MAGIC / SRV_CONN_MAGIC. Read once: a torn UAF would
    // observe 0 here (the freed-object scrubs clear magic before kfree).
    u64 magic = *(const u64 *)obj;
    if (magic == SRV_SERVICE_MAGIC) {
        return svc_listener_poll((struct SrvService *)obj, events, pw);
    }
    if (magic == SRV_CONN_MAGIC) {
        // Client-side connection handle. `srvconn_poll`'s semantics are
        // SERVER-endpoint (POLLIN ↔ c2s.count > 0 — bytes corvus reads;
        // POLLOUT ↔ s2c room — room corvus writes). A client polling its
        // OWN handle expects mirror-image semantics (POLLIN ↔ s2c has a
        // reply; POLLOUT ↔ c2s has room) — delegating here would return
        // misleading revents. No v1.0 caller polls this (joey drives 9P
        // synchronously through the kernel client; corvus polls its
        // accept-side Spoor, not its KObj_Srv). Fail-closed until the
        // client-side poll story lands (its own SrvConn `client_poll`
        // entry with mirrored semantics + a separate hook list).
        return POLLNVAL;
    }
    // Unknown magic — corruption, or a future KObj_Srv flavor.
    return POLLNVAL;
}

// devsrv_poll — the Dev `.poll` slot. Dispatches by Spoor flavor (root /
// service-ref / connection — discriminated by the aux's first u64; see
// devsrv_conn_of's comment).
static short devsrv_poll(struct Spoor *c, short events,
                         struct poll_waiter *pw) {
    if (!c || c->dc != 's') return POLLERR;
    if (!c->aux) {
        // /srv root Spoor — no readiness state. The caller is asking
        // about a directory; a poll on a directory has no real meaning
        // here. Report no events (the caller's poll blocks until timeout).
        return 0;
    }
    u64 m = *(const u64 *)c->aux;
    if (m == SRV_CONN_MAGIC) {
        return srvconn_poll((struct SrvConn *)c->aux, events, pw);
    }
    if (m == DEVSRV_SVC_MAGIC) {
        // Service-ref Spoor — the result of walking /srv/<name>. Used by
        // the client-open path (P5-corvus-srv-impl-b) to mint a SrvConn;
        // not a pollable transport itself. No events.
        return 0;
    }
    extinction("devsrv_poll: Spoor aux has unknown magic (corruption)");
    return POLLERR;   // unreachable
}

struct Dev devsrv = {
    .dc       = 's',          // Plan 9 #s
    .name     = "srv",

    .reset    = devsrv_reset,
    .init     = devsrv_init,
    .shutdown = devsrv_shutdown,

    .attach   = devsrv_attach,
    .walk     = devsrv_walk,
    .stat     = devsrv_stat,

    .open     = devsrv_open,
    .create   = devsrv_create,
    .close    = devsrv_close,

    .read     = devsrv_read,
    .bread    = devsrv_bread,
    .write    = devsrv_write,
    .bwrite   = devsrv_bwrite,

    .poll     = devsrv_poll,        // P5-poll-b

    .remove   = devsrv_remove,
    .wstat    = devsrv_wstat,
    .power    = devsrv_power,
};
