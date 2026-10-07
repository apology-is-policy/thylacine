// p9_attach — kernel-side machinery for the attach_9p syscall
// (P5-attach-create). Per `kernel/include/thylacine/9p_attach.h`.

#include <thylacine/9p_attach.h>
#include <thylacine/9p_client.h>
#include <thylacine/9p_spoor_transport.h>
#include <thylacine/9p_srvconn_transport.h>
#include <thylacine/9p_transport.h>
#include <thylacine/9p_wire.h>
#include <thylacine/cons.h>
#include <thylacine/dev9p.h>
#include <thylacine/errno.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/srvconn.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../arch/arm64/timer.h"
#include "../mm/slub.h"

_Static_assert(P9_ATTACHED_MAGIC == 0x50394154u, "attach magic drift");

// R2 F5R2 close: pins the dual-destroy discipline in attached_destroy_
// inner. Both p9_spoor_transport_destroy and p9_srvconn_transport_
// destroy are called on the same `adp` pointer (one will magic-mismatch
// and no-op; the other will clobber its magic + clear its inner ref).
// The correctness depends on the two magic constants being DISTINCT --
// a future adapter type that accidentally reused a magic would create
// double-destroy via the wrong-type path. Pin at compile time.
_Static_assert(P9_SPOOR_TRANSPORT_MAGIC != P9_SRVCONN_TRANSPORT_MAGIC,
               "transport magic constants must be distinct -- the "
               "dual-destroy discipline in attached_destroy_inner "
               "relies on at most one of the two destroy paths matching "
               "the adapter's magic");
// R2 F4R2 close: the dual-destroy's strict-aliasing defense relies on
// `magic` being at offset 0 in BOTH transport types -- the magic read
// at the start of each destroy is offset-correct regardless of the
// declared pointer type. Pin the offset.
_Static_assert(__builtin_offsetof(struct p9_spoor_transport, magic) == 0,
               "p9_spoor_transport.magic must be at offset 0 for the "
               "dual-destroy offset-0 read invariant");
_Static_assert(__builtin_offsetof(struct p9_srvconn_transport, magic) == 0,
               "p9_srvconn_transport.magic must be at offset 0 for the "
               "dual-destroy offset-0 read invariant");

// =============================================================================
// #210: the /ctl/9p-sessions registry — every live attached session,
// singly linked through a->ctl_next. Only p9_attached sessions register
// (the sole production p9_client funnel); loopback test clients never
// appear. Lock order: registry -> c->lock (the walker snapshots each
// client under its own lock); link/unlink take ONLY the registry lock,
// so there is no inversion. Unlink runs at the top of the last-unref
// destroy — the walker holds the registry lock across its whole walk, so
// it can never reach an attached whose teardown has begun.
// =============================================================================

static spin_lock_t         g_p9_ctl_lock;
static struct p9_attached *g_p9_ctl_head;

static void attached_ctl_link(struct p9_attached *a,
                              const u8 *aname, size_t aname_len) {
    // Default label: the attach aname (printable-ASCII-sanitized,
    // truncated). srvconn_attach_dev9p_root relabels /srv sessions.
    size_t n = 0;
    for (; n < sizeof(a->ctl_label) - 1 && n < aname_len; n++) {
        u8 ch = aname[n];
        a->ctl_label[n] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
    }
    a->ctl_label[n] = 0;
    if (n == 0) { a->ctl_label[0] = '-'; a->ctl_label[1] = 0; }
    a->ctl_id = -1;
    a->ctl_owner  = PRINCIPAL_INVALID;
    a->ctl_server = PRINCIPAL_INVALID;
    spin_lock(&g_p9_ctl_lock);
    a->ctl_next   = g_p9_ctl_head;
    g_p9_ctl_head = a;
    spin_unlock(&g_p9_ctl_lock);
}

static void attached_ctl_unlink(struct p9_attached *a) {
    spin_lock(&g_p9_ctl_lock);
    struct p9_attached **pp = &g_p9_ctl_head;
    while (*pp && *pp != a) pp = &(*pp)->ctl_next;
    if (*pp) *pp = a->ctl_next;
    spin_unlock(&g_p9_ctl_lock);
    a->ctl_next = NULL;
}

void p9_attached_set_ctl_ident(struct p9_attached *a, const char *label,
                               int id) {
    if (!a || a->magic != P9_ATTACHED_MAGIC) return;
    // Audit F6: the store runs under the registry lock so a concurrent
    // walker never reads a torn label, and an empty label maps to "-" —
    // fmt_str("") is devctl's overflow sentinel, so an empty label would
    // abort the whole sessions listing (the 923235a3 class, reintroduced
    // by data instead of by literal).
    spin_lock(&g_p9_ctl_lock);
    if (label) {
        size_t n = 0;
        for (; n < sizeof(a->ctl_label) - 1 && label[n]; n++)
            a->ctl_label[n] = label[n];
        a->ctl_label[n] = 0;
        if (n == 0) { a->ctl_label[0] = '-'; a->ctl_label[1] = 0; }
    }
    a->ctl_id = id;
    spin_unlock(&g_p9_ctl_lock);
}

void p9_attached_set_ctl_owners(struct p9_attached *a, u32 attacher, u32 server) {
    if (!a || a->magic != P9_ATTACHED_MAGIC) return;
    spin_lock(&g_p9_ctl_lock);
    a->ctl_owner  = attacher;
    a->ctl_server = server;
    spin_unlock(&g_p9_ctl_lock);
}

void p9_attached_ctl_iterate(p9_attached_ctl_cb cb, void *arg) {
    if (!cb) return;
    spin_lock(&g_p9_ctl_lock);
    for (struct p9_attached *a = g_p9_ctl_head; a; a = a->ctl_next) {
        struct p9_client_ctl snap;
        p9_client_ctl_snapshot(a->client, &snap);
        if (!cb(a->ctl_label, a->ctl_id, a->msize, a->ctl_owner, a->ctl_server,
                &snap, arg)) break;
    }
    spin_unlock(&g_p9_ctl_lock);
}

struct p9_attached *p9_attached_create(
        struct p9_transport_ops transport_ops,
        size_t                  recv_cap,
        u32                     root_fid,
        u32                     msize,
        const u8               *uname, size_t uname_len,
        const u8               *aname, size_t aname_len,
        u32                     n_uname,
        int                    *out_err) {
    // out_err carries a negative POSIX errno on every NULL-return path so the
    // SYS_ATTACH_9P* handlers surface it (A-3c / M6) -- most importantly the
    // Tattach Rlerror ecode (-T_E_ACCES on a dataset-scope refusal) rather than
    // collapsing every failure to a bare -1.
    if (out_err) *out_err = 0;
    if (recv_cap < P9_HDR_LEN) { if (out_err) *out_err = -T_E_INVAL; return NULL; }
    if (msize == 0)            { if (out_err) *out_err = -T_E_INVAL; return NULL; }

    struct p9_attached *a = kmalloc(sizeof(*a), KP_ZERO);
    if (!a) { if (out_err) *out_err = -T_E_NOMEM; return NULL; }

    // The p9_client struct is ~36 KiB (it inlines the DEFAULT-tier
    // out_buf_inline = P9_CLIENT_OUT_BUF_MAX = 32 KiB; a bulk-msize session
    // additionally kmallocs an msize-sized out_buf at init -- CF-3 B);
    // kmalloc routes large requests through alloc_pages (slub.c bypass at
    // SLUB_MAX_OBJECT_SIZE).
    a->client = kmalloc(sizeof(*a->client), KP_ZERO);
    if (!a->client) {
        kfree(a);
        if (out_err) *out_err = -T_E_NOMEM;
        return NULL;
    }
    a->recv_buf = kmalloc(recv_cap, KP_ZERO);
    if (!a->recv_buf) {
        kfree(a->client);
        kfree(a);
        if (out_err) *out_err = -T_E_NOMEM;
        return NULL;
    }

    int rc = p9_client_init(a->client, root_fid, msize,
                              transport_ops, a->recv_buf, recv_cap);
    if (rc != 0) {
        kfree(a->recv_buf);
        kfree(a->client);
        kfree(a);
        if (out_err) *out_err = rc;   // -P9_E_INVAL (== -T_E_INVAL)
        return NULL;
    }

    rc = p9_client_handshake(a->client, uname, uname_len,
                                aname, aname_len, n_uname);
    if (rc != 0) {
        // Handshake failed; destroy the client + free buffers. The
        // client's transport may have closed via map_error's -EIO
        // path; destroy is safe either way (no-op on already-closed
        // transport). rc is the negated server errno -- a Tattach
        // Rlerror ecode (e.g. -T_E_ACCES for a per-user-stratumd
        // dataset-scope refusal) or -P9_E_IO on a transport drop.
        p9_client_destroy(a->client);
        kfree(a->recv_buf);
        kfree(a->client);
        kfree(a);
        if (out_err) *out_err = rc;
        return NULL;
    }

    a->magic        = P9_ATTACHED_MAGIC;
    a->ref          = 1;           // F2: construction reference (caller's hold)
    a->recv_cap     = recv_cap;
    a->root_fid     = root_fid;
    a->msize        = msize;
    a->handshake_ok = true;
    // F2: adapter / transport_tx / transport_rx already NULL via KP_ZERO;
    // sys_attach_9p_handler installs them via p9_attached_install_transport
    // after this returns. Test-loopback paths never install.
    attached_ctl_link(a, aname, aname_len);   // #210: visible to /ctl
    return a;
}

static int attached_orphan_sink(void *arg, u32 fid);

struct Spoor *p9_attached_root_spoor(struct p9_attached *a) {
    if (!a) return NULL;
    if (a->magic != P9_ATTACHED_MAGIC) return NULL;
    if (!a->handshake_ok) return NULL;
    // Before the root publishes, so no walk runs on this client without a
    // place for its orphan fids (FID-LIFECYCLE section 9). Idempotent.
    p9_client_set_orphan_sink(a->client, attached_orphan_sink, a);
    return dev9p_attach_client(a->client, a->root_fid);
}

// F2 refcount API.

void p9_attached_ref(struct p9_attached *a) {
    if (!a) return;
    if (a->magic != P9_ATTACHED_MAGIC) return;
    // Single-threaded at v1.0 syscall surface (no SMP-shared p9_attached
    // mutation today; each syscall path holds a thread-local view). Use
    // an atomic anyway so future SMP paths don't introduce a race here.
    __atomic_fetch_add(&a->ref, 1, __ATOMIC_RELAXED);
}

int p9_attached_install_transport(struct p9_attached *a,
                                   struct p9_spoor_transport *adapter,
                                   struct Spoor *tx,
                                   struct Spoor *rx) {
    if (!a)                                  return -1;
    if (a->magic != P9_ATTACHED_MAGIC)       return -1;
    if (!adapter)                            return -1;
    // First-call-wins: refuse a second install. The SYS_ATTACH_9P path
    // calls this exactly once after a successful p9_attached_create.
    if (a->adapter)                          return -1;
    a->adapter      = adapter;
    a->transport_tx = tx;
    a->transport_rx = rx;
    return 0;
}

// Real destroy body — runs on the LAST p9_attached_unref. Walked Spoors
// closing AFTER the root holds a ref via attached_owner so this only
// fires when both the root's hold AND every walked priv's hold are gone.
static void attached_destroy_inner(struct p9_attached *a) {
    // #210: leave /ctl visibility FIRST — after unlink returns, no ctl
    // walker can reach this attached (the walker holds the registry lock
    // across its whole walk), so everything below tears down unobserved.
    attached_ctl_unlink(a);
    // No Tclunk for the root fid: p9_session_send_clunk refuses the root, and
    // the transport close below releases it on the server with every other
    // fid of the session (FID-LIFECYCLE section 9). Every Tclunk queued for a
    // closer holds a reference, so none of this session's is still queued.

    // Graceful close (best-effort) then destroy. close() closes the
    // transport via ops->close; destroy() clobbers the magic. Free the
    // buffers + the wrapper afterward.
    (void)p9_client_close(a->client);
    p9_client_destroy(a->client);

    // Clobber wrapper magic FIRST so any concurrent observer fast-fails.
    a->magic = 0;
    kfree(a->recv_buf);
    kfree(a->client);

    // F2: release transport ownership AFTER p9_client_destroy. The
    // p9_client's transport_ops vtable holds the adapter pointer as a
    // by-value `ctx`; p9_client_destroy must run while the adapter is
    // still alive (close → ops->close(ctx)). Only after destroy is done
    // can we kfree the adapter without leaving a dangling ctx.
    if (a->adapter) {
        struct Spoor *tx = a->transport_tx;
        struct Spoor *rx = a->transport_rx;
        struct p9_spoor_transport *adp = a->adapter;
        a->transport_tx = NULL;
        a->transport_rx = NULL;
        a->adapter      = NULL;
        if (tx)               spoor_clunk(tx);
        if (rx && rx != tx)   spoor_clunk(rx);
        // R1 F5 close: clobber the adapter's magic BEFORE kfree so a
        // concurrent observer fast-fails (mirror p9_spoor_transport's +
        // p9_srvconn_transport's documented invariant; the destroys
        // are magic-guarded so each is a no-op if `adp` is the other
        // adapter type -- safe defense-in-depth, harmless cost).
        p9_spoor_transport_destroy(adp);
        p9_srvconn_transport_destroy((struct p9_srvconn_transport *)adp);
        kfree(adp);
    }

    kfree(a);
}

void p9_attached_unref(struct p9_attached *a) {
    if (!a) return;
    if (a->magic != P9_ATTACHED_MAGIC) return;
    int pre = __atomic_fetch_sub(&a->ref, 1, __ATOMIC_ACQ_REL);
    // pre is the value BEFORE the subtraction. pre <= 0 means an extra
    // unref past zero — a refcount bug; extinct is the right response.
    if (pre <= 0) {
        // Don't extinct from kernel utility code in case of corruption;
        // log via magic clobber + early-return discipline. The magic
        // check at the top of every public op then fast-fails subsequent
        // calls. (No good way to surface here without extincting; v1.0
        // accepts the silent-failure shape.)
        return;
    }
    if (pre == 1) {
        attached_destroy_inner(a);
    }
}

// Legacy public name — unref-equivalent semantics for callers that hold
// the single construction ref and never spawned walked privs.
void p9_attached_destroy(struct p9_attached *a) {
    p9_attached_unref(a);
}

bool p9_attached_is_open(const struct p9_attached *a) {
    if (!a) return false;
    if (a->magic != P9_ATTACHED_MAGIC) return false;
    if (!a->handshake_ok) return false;
    return p9_client_is_open(a->client);
}

struct Spoor *srvconn_attach_dev9p_root(struct SrvConn *cn,
                                        const u8 *aname, size_t aname_len,
                                        const struct Proc *who, u32 flags,
                                        int *out_err) {
    if (out_err) *out_err = 0;
    if (!cn || !who) { if (out_err) *out_err = -T_E_INVAL; return NULL; }
    // The header calls `flags` the VALIDATED word; enforce that here rather than
    // trusting it, so the helper's admissible domain is its own property. What
    // this catches is precisely a word the /srv handler would NOT have admitted:
    // the CAPE bit, whose meaning belongs to the OTHER attach handler, and any
    // unknown bit. It does NOT catch an unvalidated LOOSE -- that bit is legal
    // here, so a raw LOOSE and a validated one are indistinguishable by
    // construction. Fails closed, and never fires for the two current callers
    // (devsrv's literal 0, and a word syscall.c already validated).
    if (!sys_attach_9p_flags_ok(flags, /*srv=*/true)) {
        if (out_err) *out_err = -T_E_INVAL;
        return NULL;
    }
    // A byte conn minted from a DMSRVCAPE service capes EVERY attach over it:
    // its poster, the server's own side, declared the server's ids foreign
    // (IDENTITY-DESIGN 3.2). That mark is the ONLY input: no bit of `flags`
    // capes a /srv session, so the cape stays the poster's decision whatever
    // word a caller hands in. Only a byte conn can carry the mark -- the cape's
    // no-escalation argument rests on the attacher holding the raw transport,
    // which a 9P-mode opener never does.
    bool cape = srvconn_cape(cn) && __atomic_load_n(&cn->byte_mode, __ATOMIC_ACQUIRE);

    // The adapter wraps cn's c2s/s2c byte rings; its init takes ONE srvconn_ref.
    // Pre-init failures leave cn untouched (the caller decides on teardown);
    // post-init failures go through the adapter's close, which tears cn down.
    struct p9_srvconn_transport *adapter = kmalloc(sizeof(*adapter), KP_ZERO);
    if (!adapter) { if (out_err) *out_err = -T_E_NOMEM; return NULL; }
    if (p9_srvconn_transport_init(adapter, cn) != 0) {
        kfree(adapter);
        if (out_err) *out_err = -T_E_IO;
        return NULL;
    }

    // R1 F4 (SYS_ATTACH_9P_SRV) discipline: set kernel_attached as early as the
    // adapter commits, so a userspace close of the conn-endpoint handle skips
    // srvconn_teardown (the rings are load-bearing for this kernel 9P client).
    srvconn_set_kernel_attached(cn);
    // R1 F1 discipline: bound the Tversion + Tattach handshake on the wall clock
    // (a hung server times out rather than wedging the caller indefinitely).
    srvconn_set_client_deadline(cn,
        timer_now_ns() + SRVCONN_HANDSHAKE_DEADLINE_NS);

    struct p9_transport_ops ops = p9_srvconn_transport_ops(adapter);
    // CF-3 B: the msize proposal + recv cap come from the CONNECTION's ring
    // class (set at mint from the service's DMSRVBULK bit) -- a bulk FS
    // service negotiates 128 KiB (stratumd's STM_9P_MSIZE_DEFAULT accepts
    // exactly that), a default service stays at 32 KiB. The proposal can
    // never exceed what the conn's rings carry (cap = 2x msize).
    u32 conn_msize = srvconn_msize(cn);
    int aerr = 0;
    struct p9_attached *att = p9_attached_create(
        ops,
        conn_msize,              // recv_cap (= msize; matches the SrvConn ring)
        SRVCONN_ROOT_FID,        // root_fid
        conn_msize,              // msize (client proposal; negotiated down)
        NULL, 0,                 // uname (empty; SO_PEERCRED is the live channel)
        aname_len > 0 ? aname : NULL, aname_len,
        // A-3 M4: the attacher's kernel-stamped principal; a caped attach names
        // no user at all (IDENTITY-DESIGN 3.2 -- nothing identity-bearing
        // crosses to a server whose ids are foreign).
        cape ? PRINCIPAL_NONE : who->principal_id, &aerr);
    if (!att) {
        // Handshake failed (server unresponsive / deadline / Rlerror / OOM). The
        // adapter still holds its srvconn_ref; its close drops it AND tears cn
        // down (EOF both rings).
        struct p9_transport_ops cops = p9_srvconn_transport_ops(adapter);
        if (cops.close) (void)cops.close(cops.ctx);
        p9_srvconn_transport_destroy(adapter);
        kfree(adapter);
        if (out_err) *out_err = aerr;
        return NULL;
    }

    // #841: the handshake (Tversion + Tattach) is done -> switch to NO
    // steady-state deadline. The elected-reader pipeline (ARCH §21.10) blocks
    // until reply / EOF / death (death-interruptible via #811) -- a per-op
    // timeout would abandon one in-flight op and desync the shared 9P stream.
    // The HANDSHAKE_DEADLINE armed above bounded only the serial, fresh-client
    // handshake, where a timeout tears down the unshared client with no desync.
    srvconn_set_client_deadline(cn, 0);

    // #210: attribute this session in /ctl/9p-sessions by the CONNECTING
    // peer's pid (aname is often empty on the /srv path).
    p9_attached_set_ctl_ident(att, "srv", cn->peer_pid);
    // Its ends: the attaching Proc and the conn's server.
    p9_attached_set_ctl_owners(att, __atomic_load_n(&who->principal_id, __ATOMIC_ACQUIRE),
                               cn->server_principal);

    // B1 per-attach loose mode (I-38 opt-in), the identity cape and the remote
    // declaration: stamped on the still-private client BEFORE the root Spoor
    // exists -- the caller's handle publication orders them against every
    // subsequent dev9p op, so the plain fields need no atomics and are never
    // flipped after this point.
    if ((flags & SYS_ATTACH_9P_LOOSE) && att->client)
        att->client->loose = true;
    if (cape && att->client)
        p9_client_set_cape(att->client, who->principal_id, who->primary_gid);
    // The remote declaration (HAUL-DESIGN 4.8) is read off the conn like the
    // cape, but from either mode: it is a label and grants nothing, so the
    // cape's byte-mode argument has nothing to protect here.
    if (srvconn_remote(cn) && att->client)
        p9_client_set_remote(att->client);

    // Transfer adapter ownership into the attached (tx == rx == NULL: the SrvConn
    // lifetime is the adapter's own srvconn_ref, not a transport-Spoor pair).
    if (p9_attached_install_transport(att, (struct p9_spoor_transport *)adapter,
                                       NULL, NULL) != 0) {
        // Defensive (first install on a fresh attached). a->adapter was never
        // set, so attached_destroy_inner's adapter block is skipped -> kfree it
        // here after destroying (which drops the srvconn_ref via close).
        p9_attached_unref(att);
        p9_srvconn_transport_destroy(adapter);
        kfree(adapter);
        if (out_err) *out_err = -T_E_IO;
        return NULL;
    }
    // From here failure paths just unref `att`; its last-ref destroy handles the
    // adapter + srvconn cleanup via the transport close vtable.

    struct Spoor *root = p9_attached_root_spoor(att);
    if (!root) {
        p9_attached_unref(att);
        if (out_err) *out_err = -T_E_IO;
        return NULL;
    }

    struct dev9p_priv *root_priv = (struct dev9p_priv *)root->aux;
    if (!root_priv || root_priv->magic != DEV9P_PRIV_MAGIC) {
        spoor_clunk(root);
        p9_attached_unref(att);
        if (out_err) *out_err = -T_E_IO;
        return NULL;
    }
    root_priv->attached_owner = att;
    p9_attached_ref(att);        // the root's attached_owner hold
    p9_attached_unref(att);      // drop the construction ref; root owns the session
    return root;
}

// =============================================================================
// The closer (docs/FID-LIFECYCLE-DESIGN.md section 9; dec-2026-09-28-tclunk-
// closer). Plan 9's closeproc pool, serialized per session. A session with
// deferred Tclunks waits on the run-queue until a closer takes it; that closer
// sends them all, oldest first, parking on back-pressure like any live thread.
// The closer that takes a session spawns a spare when no other closer is idle,
// and a closer that finds no work retires when another is idle, so one idle
// closer is kept and a server that never answers holds only its own session's
// closer. A hand-off that finds every closer busy and no spare starting --
// the last spawn failed -- spawns one itself. A kernel thread cannot free
// itself, so a retired closer parks terminally and the idle one reaps it
// (loom_free's SQPOLL join, by a peer).
//
// g_closer_lock is a leaf below c->lock: the orphan sink takes it under
// c->lock. Nothing is sent, slept on, freed or unreffed under it; the wakeups
// under it take only rendez and scheduler locks. Every wakeup of a closer's
// Rendez happens under it, because a retired closer's struct is freed by its
// reaper once the closer has left the lists.
// =============================================================================

struct p9_closer_entry {
    struct p9_closer_entry *next;
    u32                     fid;
    struct p9_close_job    *job;     // NULL: a Tclunk alone
};

struct p9_closer {
    struct Thread    *thread;
    struct Rendez     r;        // only this closer sleeps on it
    struct p9_closer *next;     // the retired list
    bool              kicked;   // "look at the pool again"; read by the idle cond
    bool              exited;   // RELEASE-stored in the terminal window
    bool              started;  // its first loop top ran: its spawn is over
};

// Backoff for a closer's own -P9_E_AGAIN: a closer never dies, so that is a
// spill buffer that could not be allocated under back-pressure. Bounded; then
// the fid is reported as a live leak.
#define CLOSER_RETRY_NS_MIN   1000000ull      // 1 ms, doubling
#define CLOSER_RETRIES        10u             // ~1 s in all
// How soon the idle closer looks again for a retired peer that had not yet
// reached its terminal switch when it was kicked.
#define CLOSER_REAP_NS        10000000ull     // 10 ms
// Attempts one spawn makes while a session waits and no closer is idle.
#define CLOSER_SPAWN_TRIES    3u

static spin_lock_t            g_closer_lock;
static struct p9_attached    *g_closer_runq_head;
static struct p9_attached    *g_closer_runq_tail;
static struct p9_closer      *g_closer_idle;       // at most one
static struct p9_closer      *g_closer_retired;    // parked terminally, unreaped
static bool                   g_closer_spawning;   // a spare created, not yet started
static struct p9_closer_stats g_closer_st;
// Tests: allocations to fail, and a spawn held at its end (0 off, 1 armed, 2 held).
static u32                    g_closer_fail_spawns;
static u32                    g_closer_fail_nodes;
static u32                    g_closer_hold;

static bool closer_spawn(void);

static bool closer_knob_take(u32 *knob) {
    u32 n = __atomic_load_n(knob, __ATOMIC_RELAXED);
    while (n > 0) {
        if (__atomic_compare_exchange_n(knob, &n, n - 1, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return true;
    }
    return false;
}

static struct p9_closer_entry *closer_entry_alloc(void) {
    if (closer_knob_take(&g_closer_fail_nodes)) return NULL;
    return kmalloc(sizeof(struct p9_closer_entry), 0);
}

// Under g_closer_lock. A kick sets the flag before the wakeup, so a closer
// between its unlock and its tsleep sees it at the sleep's first cond check.
static void closer_kick_locked(struct p9_closer *k) {
    __atomic_store_n(&k->kicked, true, __ATOMIC_RELEASE);
    (void)wakeup(&k->r);
}

static void closer_enqueue_locked(struct p9_attached *a,
                                  struct p9_closer_entry *e) {
    e->next = NULL;
    if (a->closer_tail) a->closer_tail->next = e;
    else                a->closer_head = e;
    a->closer_tail = e;
    g_closer_st.pending++;
    // A busy session's closer takes the entry before it lets go of the
    // session; a queued one is already waiting for a closer.
    if (a->closer_busy || a->closer_queued) return;
    a->closer_queued = true;
    a->closer_next   = NULL;
    if (g_closer_runq_tail) g_closer_runq_tail->closer_next = a;
    else                    g_closer_runq_head = a;
    g_closer_runq_tail = a;
    if (g_closer_idle) closer_kick_locked(g_closer_idle);
}

int p9_attached_defer_close(struct p9_attached *a, u32 fid,
                            struct p9_close_job *job) {
    if (!a || a->magic != P9_ATTACHED_MAGIC) return -1;
    struct p9_closer_entry *e = closer_entry_alloc();
    if (!e) return -1;
    e->fid = fid;
    e->job = job;
    p9_attached_ref(a);          // the entry's; the caller's own keeps it above 0
    spin_lock(&g_closer_lock);
    closer_enqueue_locked(a, e);
    // A session waits, no closer is idle and none is starting: the spare the
    // last busy closer spawned failed, and that closer may be waiting on a
    // server that never answers. Spawn the spare now, so no session waits on
    // another session's server.
    bool spawn = g_closer_runq_head && !g_closer_idle && !g_closer_spawning;
    if (spawn) g_closer_spawning = true;
    spin_unlock(&g_closer_lock);
    if (spawn) (void)closer_spawn();
    return 0;
}

int p9_attached_defer_clunk(struct p9_attached *a, u32 fid) {
    return p9_attached_defer_close(a, fid, NULL);
}

// A reference for a caller that holds none: fails once the count reached 0,
// when the session is being torn down and its fids die with it.
static bool attached_tryref(struct p9_attached *a) {
    int old = __atomic_load_n(&a->ref, __ATOMIC_RELAXED);
    while (old > 0) {
        if (__atomic_compare_exchange_n(&a->ref, &old, old + 1, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return true;
    }
    return false;
}

// The client's orphan sink: a flushed or abandoned walk's late reply bound a
// new fid nobody owns. Called under c->lock by whichever thread dispatched the
// reply, holding no reference on `a`; c is alive, so `a` is. The allocation
// comes first so a failed tryref never needs an unref, whose last drop would
// tear the session down under c->lock. Under c->lock it only queues, never
// spawns (a thread's stack allocation and the Proc table lock): with every
// closer busy and the last spawn failed, the entry waits for the next
// hand-off, or for a closer to finish its session.
static int attached_orphan_sink(void *arg, u32 fid) {
    struct p9_attached *a = (struct p9_attached *)arg;
    if (!a || a->magic != P9_ATTACHED_MAGIC) return -1;
    struct p9_closer_entry *e = closer_entry_alloc();
    if (!e) {
        // The fid stays bound. On a live session the server keeps it until
        // the session ends: a leak, reported like the hand-off's. A session
        // that died, or is being torn down, takes its fids with it.
        struct p9_client *c = a->client;
        if (!c->dead && p9_session_is_open(&c->session) &&
            __atomic_load_n(&a->ref, __ATOMIC_ACQUIRE) > 0)
            p9_clunk_refused(fid, -T_E_NOMEM);
        return -1;
    }
    if (!attached_tryref(a)) { kfree(e); return -1; }
    e->fid = fid;
    e->job = NULL;
    spin_lock(&g_closer_lock);
    closer_enqueue_locked(a, e);
    spin_unlock(&g_closer_lock);
    return 0;
}

void p9_clunk_refused(u32 fid, int rc) {
    spin_lock(&g_closer_lock);
    g_closer_st.live_refusals++;
    spin_unlock(&g_closer_lock);
    struct cons_diag_line dl;
    cons_diag_line_init(&dl);
    cons_diag_line_puts(&dl, "9p: close: clunk of fid ");
    cons_diag_line_putdec(&dl, (u64)fid);
    cons_diag_line_puts(&dl, " refused rc ");
    cons_diag_line_putdec(&dl, (u64)(rc < 0 ? -rc : rc));
    cons_diag_line_puts(&dl, "\n");
    cons_diag_line_emit(&dl);
}

void p9_close_flush_failed(u32 fid, int rc) {
    struct cons_diag_line dl;
    cons_diag_line_init(&dl);
    cons_diag_line_puts(&dl, "9p: close: flush of fid ");
    cons_diag_line_putdec(&dl, (u64)fid);
    cons_diag_line_puts(&dl, " failed rc ");
    cons_diag_line_putdec(&dl, (u64)(rc < 0 ? -rc : rc));
    cons_diag_line_puts(&dl, "\n");
    cons_diag_line_emit(&dl);
}

static int closer_never_cond(void *arg) {
    (void)arg;
    return 0;
}

// Send one deferred Tclunk. A closer is kproc's, which never dies and never
// stops, so -P9_E_AGAIN here is a spill-OOM under back-pressure, and the fid
// was taken back: wait for memory, a bounded number of times. Nobody wakes
// a busy closer's Rendez (kicks go to the idle closer), so each wait runs to
// its deadline.
static int closer_send(struct p9_closer *self, struct p9_attached *a, u32 fid) {
    u64 backoff = CLOSER_RETRY_NS_MIN;
    for (u32 tries = 0;; tries++) {
        int rc = p9_client_clunk_async(a->client, fid);
        if (rc != -P9_E_AGAIN || tries >= CLOSER_RETRIES) return rc;
        (void)tsleep(&self->r, closer_never_cond, NULL, timer_now_ns() + backoff);
        backoff *= 2;
    }
}

// Send every deferred Tclunk of `a`, each after its close job if it has one,
// which this closer took off the run-queue (closer_busy). Each entry's reference is dropped outside the lock: the last
// drop tears the session down, which may close Spoors and queue again. While
// entries remain they hold references, so `a` outlives each unref but the
// last; the closer lets go of `a` (closer_busy = false) before that one.
static void closer_serve(struct p9_closer *self, struct p9_attached *a) {
    for (;;) {
        spin_lock(&g_closer_lock);
        struct p9_closer_entry *e = a->closer_head;
        a->closer_head = e->next;
        if (!a->closer_head) a->closer_tail = NULL;
        spin_unlock(&g_closer_lock);

        // A close job first: its writes need the fid bound. Its failure on a
        // live session loses bytes write() reported written, so it is loud.
        int  jrc   = e->job ? e->job->run(e->job, a->client, e->fid) : 0;
        bool jlost = jrc != 0 && p9_client_fid_held(a->client, e->fid);
        if (jlost) p9_close_flush_failed(e->fid, jrc);

        int  rc   = closer_send(self, a, e->fid);
        bool live = rc != 0 && p9_client_fid_held(a->client, e->fid);
        if (live) p9_clunk_refused(e->fid, rc);

        spin_lock(&g_closer_lock);
        if (rc == 0)   g_closer_st.sent++;
        else if (live) g_closer_st.refused++;
        else           g_closer_st.dropped++;     // the session died: its fids too
        if (e->job)    g_closer_st.jobs++;
        if (jlost)     g_closer_st.job_errors++;
        g_closer_st.pending--;
        bool done = a->closer_head == NULL;
        if (done) a->closer_busy = false;
        spin_unlock(&g_closer_lock);

        if (e->job) e->job->release(e->job);
        kfree(e);
        p9_attached_unref(a);
        if (done) return;
    }
}

// Under g_closer_lock: unlink the retired closers that have reached their
// terminal switch. The caller frees them after dropping the lock.
static struct p9_closer *closer_take_reapable_locked(void) {
    struct p9_closer *out = NULL;
    struct p9_closer **pp = &g_closer_retired;
    while (*pp) {
        struct p9_closer *k = *pp;
        if (__atomic_load_n(&k->exited, __ATOMIC_ACQUIRE)) {
            *pp = k->next;
            k->next = out;
            out = k;
            g_closer_st.retired--;
            g_closer_st.reaped++;
        } else {
            pp = &k->next;
        }
    }
    return out;
}

// exited (ACQUIRE, above) pairs with the terminal RELEASE store, so each
// thread is EXITING and past every use of its struct; thread_free spins on
// on_cpu for the switch-away still in flight.
static void closer_free_reaped(struct p9_closer *list) {
    while (list) {
        struct p9_closer *k = list;
        list = k->next;
        thread_free(k->thread);
        kfree(k);
    }
}

__attribute__((noreturn))
static void closer_retire(struct p9_closer *self) {
    // The loom SQPOLL terminal: IRQs masked across the state write and the
    // RELEASE so no preempt lands between them, then a switch that never
    // returns (EXITING is never re-enqueued).
    (void)spin_lock_irqsave(NULL);
    current_thread()->state = THREAD_EXITING;
    __atomic_store_n(&self->exited, true, __ATOMIC_RELEASE);
    sched();
    extinction("p9 closer: returned from its terminal sched");
}

static int closer_kicked_cond(void *arg) {
    struct p9_closer *k = (struct p9_closer *)arg;
    return __atomic_load_n(&k->kicked, __ATOMIC_ACQUIRE) ? 1 : 0;
}

static void closer_main(void *arg) {
    struct p9_closer *self = (struct p9_closer *)arg;
    for (;;) {
        spin_lock(&g_closer_lock);
        if (!self->started) {
            // This closer's spawn ends here, where it can take work, not at
            // its creation: a hand-off in between would spawn a second spare.
            self->started     = true;
            g_closer_spawning = false;
        }
        if (g_closer_idle == self) g_closer_idle = NULL;
        struct p9_closer   *reap = closer_take_reapable_locked();
        struct p9_attached *a    = g_closer_runq_head;
        if (a) {
            g_closer_runq_head = a->closer_next;
            if (!g_closer_runq_head) g_closer_runq_tail = NULL;
            a->closer_next   = NULL;
            a->closer_queued = false;
            a->closer_busy   = true;
            // A spare, so a session that queues while this one is served --
            // or while its server never answers -- finds a closer.
            bool spawn = !g_closer_idle && !g_closer_spawning;
            if (spawn) g_closer_spawning = true;
            spin_unlock(&g_closer_lock);
            closer_free_reaped(reap);
            if (spawn) (void)closer_spawn();
            closer_serve(self, a);
            continue;
        }
        if (g_closer_idle) {
            // Another closer is idle: retire, and kick it to reap this one.
            self->next       = g_closer_retired;
            g_closer_retired = self;
            g_closer_st.threads--;
            g_closer_st.retired++;
            closer_kick_locked(g_closer_idle);
            spin_unlock(&g_closer_lock);
            closer_free_reaped(reap);
            closer_retire(self);
        }
        g_closer_idle = self;
        __atomic_store_n(&self->kicked, false, __ATOMIC_RELAXED);
        u64 deadline = g_closer_retired ? timer_now_ns() + CLOSER_REAP_NS : 0;
        spin_unlock(&g_closer_lock);
        closer_free_reaped(reap);
        (void)tsleep(&self->r, closer_kicked_cond, self, deadline);
    }
}

u32 p9_closer_fail_spawns_for_test(u32 n) {
    return __atomic_exchange_n(&g_closer_fail_spawns, n, __ATOMIC_RELAXED);
}

u32 p9_closer_fail_nodes_for_test(u32 n) {
    return __atomic_exchange_n(&g_closer_fail_nodes, n, __ATOMIC_RELAXED);
}

void p9_closer_hold_spawn_for_test(bool hold) {
    __atomic_store_n(&g_closer_hold, hold ? 1u : 0u, __ATOMIC_RELEASE);
}

bool p9_closer_spawn_held_for_test(void) {
    return __atomic_load_n(&g_closer_hold, __ATOMIC_ACQUIRE) == 2u;
}

static void closer_spawn_hold_for_test(void) {
    u32 armed = 1u;
    if (!__atomic_compare_exchange_n(&g_closer_hold, &armed, 2u, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return;
    while (__atomic_load_n(&g_closer_hold, __ATOMIC_ACQUIRE) == 2u) sched();
}

// A closer is created with g_closer_spawning set -- by the closer that took
// work, by a hand-off that found every closer busy, or by the boot start --
// and clears it at its first loop top. A hand-off made while the spawn runs
// sees the flag and leaves the spare to it, so a failed attempt tries again
// while a session waits and no closer is idle. After CLOSER_SPAWN_TRIES
// failures (memory is short) it clears the flag, and a waiting session waits
// for the next hand-off, or for a closer to finish its session.
static bool closer_spawn(void) {
    for (u32 tries = 1;; tries++) {
        struct p9_closer *k = closer_knob_take(&g_closer_fail_spawns) ? NULL :
                              kmalloc(sizeof(*k), KP_ZERO);
        struct Thread    *t = NULL;
        if (k) {
            rendez_init(&k->r);
            t = thread_create_with_arg(kproc(), closer_main, k);
            if (t) k->thread = t;
        }
        if (t) {
            spin_lock(&g_closer_lock);
            g_closer_st.threads++;
            g_closer_st.spawned++;
            spin_unlock(&g_closer_lock);
            closer_spawn_hold_for_test();
            ready(t);
            return true;
        }
        kfree(k);
        closer_spawn_hold_for_test();
        spin_lock(&g_closer_lock);
        g_closer_st.spawn_failed++;
        bool again = g_closer_runq_head && !g_closer_idle &&
                     tries < CLOSER_SPAWN_TRIES;
        if (!again) g_closer_spawning = false;
        spin_unlock(&g_closer_lock);
        if (!again) return false;
    }
}

int p9_closer_start(void) {
    spin_lock(&g_closer_lock);
    g_closer_spawning = true;
    spin_unlock(&g_closer_lock);
    return closer_spawn() ? 0 : -1;
}

void p9_closer_stats(struct p9_closer_stats *out) {
    if (!out) return;
    spin_lock(&g_closer_lock);
    *out             = g_closer_st;
    out->idle        = g_closer_idle ? 1u : 0u;
    out->idle_parked = g_closer_idle &&
        __atomic_load_n(&g_closer_idle->thread->state, __ATOMIC_ACQUIRE) ==
            THREAD_SLEEPING ? 1u : 0u;
    out->runq = 0;
    for (struct p9_attached *a = g_closer_runq_head; a; a = a->closer_next)
        out->runq++;
    spin_unlock(&g_closer_lock);
}
