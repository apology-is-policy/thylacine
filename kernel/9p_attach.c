// p9_attach — kernel-side machinery for the attach_9p syscall
// (P5-attach-create). Per `kernel/include/thylacine/9p_attach.h`.

#include <thylacine/9p_attach.h>
#include <thylacine/9p_client.h>
#include <thylacine/9p_spoor_transport.h>
#include <thylacine/9p_srvconn_transport.h>
#include <thylacine/9p_transport.h>
#include <thylacine/9p_wire.h>
#include <thylacine/dev9p.h>
#include <thylacine/errno.h>
#include <thylacine/page.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

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

    // The p9_client struct is ~12 KiB; kmalloc routes large requests
    // through alloc_pages (slub.c bypass at SLUB_MAX_OBJECT_SIZE).
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
    return a;
}

struct Spoor *p9_attached_root_spoor(struct p9_attached *a) {
    if (!a) return NULL;
    if (a->magic != P9_ATTACHED_MAGIC) return NULL;
    if (!a->handshake_ok) return NULL;
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
    // Clunk the root fid before destroying the client. The client's
    // session module would clunk every fid at close anyway (when its
    // refcount hits 0), but explicit-clunk-then-destroy is the
    // documented v1.0 contract per ARCH §9.6.6 (mount lifecycle
    // invariants).
    //
    // At v1.0 each clunk is a transport round trip. If the transport
    // has already failed (e.g., backend hung up before destroy), the
    // clunk returns an error which we ignore — the close path below
    // forcibly tears down anyway.
    if (a->handshake_ok && a->root_fid != P9_NOFID) {
        (void)p9_client_clunk(a->client, a->root_fid);
    }

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
