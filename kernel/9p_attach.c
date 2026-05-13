// p9_attach — kernel-side machinery for the attach_9p syscall
// (P5-attach-create). Per `kernel/include/thylacine/9p_attach.h`.

#include <thylacine/9p_attach.h>
#include <thylacine/9p_client.h>
#include <thylacine/9p_transport.h>
#include <thylacine/9p_wire.h>
#include <thylacine/dev9p.h>
#include <thylacine/page.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

#include "../mm/slub.h"

_Static_assert(P9_ATTACHED_MAGIC == 0x50394154u, "attach magic drift");

struct p9_attached *p9_attached_create(
        struct p9_transport_ops transport_ops,
        size_t                  recv_cap,
        u32                     root_fid,
        u32                     msize,
        const u8               *uname, size_t uname_len,
        const u8               *aname, size_t aname_len,
        u32                     n_uname) {
    if (recv_cap < P9_HDR_LEN) return NULL;
    if (msize == 0) return NULL;

    struct p9_attached *a = kmalloc(sizeof(*a), KP_ZERO);
    if (!a) return NULL;

    // The p9_client struct is ~12 KiB; kmalloc routes large requests
    // through alloc_pages (slub.c bypass at SLUB_MAX_OBJECT_SIZE).
    a->client = kmalloc(sizeof(*a->client), KP_ZERO);
    if (!a->client) {
        kfree(a);
        return NULL;
    }
    a->recv_buf = kmalloc(recv_cap, KP_ZERO);
    if (!a->recv_buf) {
        kfree(a->client);
        kfree(a);
        return NULL;
    }

    int rc = p9_client_init(a->client, root_fid, msize,
                              transport_ops, a->recv_buf, recv_cap);
    if (rc != 0) {
        kfree(a->recv_buf);
        kfree(a->client);
        kfree(a);
        return NULL;
    }

    rc = p9_client_handshake(a->client, uname, uname_len,
                                aname, aname_len, n_uname);
    if (rc != 0) {
        // Handshake failed; destroy the client + free buffers. The
        // client's transport may have closed via map_error's -EIO
        // path; destroy is safe either way (no-op on already-closed
        // transport).
        p9_client_destroy(a->client);
        kfree(a->recv_buf);
        kfree(a->client);
        kfree(a);
        return NULL;
    }

    a->magic        = P9_ATTACHED_MAGIC;
    a->recv_cap     = recv_cap;
    a->root_fid     = root_fid;
    a->msize        = msize;
    a->handshake_ok = true;
    return a;
}

struct Spoor *p9_attached_root_spoor(struct p9_attached *a) {
    if (!a) return NULL;
    if (a->magic != P9_ATTACHED_MAGIC) return NULL;
    if (!a->handshake_ok) return NULL;
    return dev9p_attach_client(a->client, a->root_fid);
}

void p9_attached_destroy(struct p9_attached *a) {
    if (!a) return;
    if (a->magic != P9_ATTACHED_MAGIC) return;

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
    kfree(a);
}

bool p9_attached_is_open(const struct p9_attached *a) {
    if (!a) return false;
    if (a->magic != P9_ATTACHED_MAGIC) return false;
    if (!a->handshake_ok) return false;
    return p9_client_is_open(a->client);
}
