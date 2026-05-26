// 9P byte-mode-SrvConn transport adapter (P6-pouch-stratumd-boot 16c).
//
// Plumbing: routes `struct p9_transport_ops` send / recv / close calls
// to `srvconn_client_send` / `srvconn_client_recv` / `srvconn_unref` on
// the adapter's wrapped SrvConn. See header for layering, lifecycle,
// and the byte-mode invariant.
//
// No state mutation beyond what srvconn's c2s/s2c ring ops do
// internally. The adapter holds one pointer + one srvconn_ref; the
// SrvConn owns the byte rings, the deadline, and the lifecycle.

#include <thylacine/9p_srvconn_transport.h>
#include <thylacine/srvconn.h>
#include <thylacine/types.h>

// =============================================================================
// Vtable implementations.
// =============================================================================

static int srvconn_transport_send(void *ctx, const u8 *buf, size_t len) {
    struct p9_srvconn_transport *st = (struct p9_srvconn_transport *)ctx;
    if (!st)                                     return -1;
    if (st->magic != P9_SRVCONN_TRANSPORT_MAGIC) return -1;
    if (!st->cn)                                 return -1;
    if (!buf && len > 0)                         return -1;

    // Loop on short writes. srvconn_client_send is non-blocking and
    // typically returns `n` (the full request) because SRVCONN_RING_CAP
    // is sized to hold a full msize frame. A short write (n < len)
    // would indicate a corrupted ring or a torn connection; the
    // transport core sinks to ERROR on a partial-write return.
    //
    // The 0-return contract: srvconn_client_send returns -1 if torn or
    // bad args. There is no "0 bytes accepted" success case; a
    // returned 0 only fires when len == 0.
    size_t total = 0;
    while (total < len) {
        long n = srvconn_client_send(st->cn,
                                       buf + total,
                                       (long)(len - total));
        if (n < 0) return -1;
        if (n == 0) {
            // Ring saturated mid-send (should not happen for properly
            // sized rings). Return partial-write count to communicate
            // progress; transport core sinks to ERROR.
            return (int)total;
        }
        total += (size_t)n;
    }
    return (int)total;
}

static int srvconn_transport_recv(void *ctx, u8 *buf, size_t cap) {
    struct p9_srvconn_transport *st = (struct p9_srvconn_transport *)ctx;
    if (!st)                                     return -1;
    if (st->magic != P9_SRVCONN_TRANSPORT_MAGIC) return -1;
    if (!st->cn)                                 return -1;
    if (!buf || cap == 0)                        return -1;

    // BLOCKING. The caller is expected to have set the SrvConn's
    // client_deadline_ns before invoking the upper-level p9_client op
    // (SYS_ATTACH_9P_SRV does this for Tversion/Tattach; later
    // Twalk/Tread/Twrite set their own deadlines via the same
    // mechanism in sys_read_for_proc / sys_write_for_proc / sys_walk_
    // open_handler's KOBJ_SPOOR -> dev9p arm).
    //
    // Returns:
    //   >0 -- bytes read
    //    0 -- EOF (the SrvConn is torn and no residual bytes remain);
    //          transport core surfaces this as recv-side ERROR. The
    //          p9_client maps "transport EOF mid-handshake" to -P9_E_IO.
    //   -1 -- deadline lapsed (srvconn_client_timed_out true) or
    //          bad args. The p9_client maps to -P9_E_IO; SYS_ATTACH_
    //          9P_SRV folds that into the syscall's -1 return.
    long n = srvconn_client_recv(st->cn, buf, (long)cap);
    if (n < 0) return -1;
    return (int)n;
}

static int srvconn_transport_close(void *ctx) {
    struct p9_srvconn_transport *st = (struct p9_srvconn_transport *)ctx;
    if (!st)                                     return -1;
    if (st->magic != P9_SRVCONN_TRANSPORT_MAGIC) return -1;

    // Tear down + drop the adapter's srvconn_ref.
    //
    // P6-pouch-stratumd-boot 16c: with kernel_attached=true, the
    // userspace handle_close on the KOBJ_SRV slot skipped teardown
    // (so the rings stayed alive for the kernel client's Twalk /
    // Tread / Twrite). The adapter's close runs when the LAST
    // KOBJ_SPOOR handle referencing the attach session drops via
    // p9_attached_destroy's transport.close. At THAT point teardown
    // is the right move: EOF both rings so the server-side worker
    // (stratumd's per-conn pthread) wakes from any pending read and
    // exits its serve loop cleanly. Without teardown here, stratumd
    // would block indefinitely in srvconn_server_recv_blocking.
    // srvconn_teardown is idempotent (safe if peer already torn).
    //
    // The SrvConn lives on if other holders (the accept-side backlog,
    // a tombstoned service registry entry) keep it alive; the last
    // srvconn_unref is what frees it. srvconn_unref is NULL-safe;
    // safe to re-close (the inner pointer goes NULL after the first
    // close).
    struct SrvConn *cn = st->cn;
    st->cn = NULL;
    if (cn) {
        srvconn_teardown(cn);
        srvconn_unref(cn);
    }

    // Magic stays valid until p9_srvconn_transport_destroy clobbers
    // it. The transport core's CLOSED state machine guards re-entry.
    return 0;
}

// =============================================================================
// Public API.
// =============================================================================

int p9_srvconn_transport_init(struct p9_srvconn_transport *st,
                                struct SrvConn *cn) {
    if (!st || !cn) return -1;
    // Take the adapter's hold. srvconn_ref extincts on a NULL or
    // corrupted SrvConn (matches the spoor_ref discipline).
    srvconn_ref(cn);
    st->magic = P9_SRVCONN_TRANSPORT_MAGIC;
    st->cn    = cn;
    return 0;
}

void p9_srvconn_transport_destroy(struct p9_srvconn_transport *st) {
    if (!st) return;
    if (st->magic != P9_SRVCONN_TRANSPORT_MAGIC) return;
    // Clobber magic FIRST so a concurrent observer fast-fails. The
    // close path is responsible for dropping the srvconn_ref;
    // destroy itself does NOT call srvconn_unref to mirror the
    // p9_spoor_transport_destroy discipline (call close before destroy
    // on the happy path).
    st->magic = 0;
    st->cn    = NULL;
}

struct p9_transport_ops p9_srvconn_transport_ops(struct p9_srvconn_transport *st) {
    struct p9_transport_ops ops;
    ops.send  = srvconn_transport_send;
    ops.recv  = srvconn_transport_recv;
    ops.close = srvconn_transport_close;
    ops.ctx   = (void *)st;
    return ops;
}

bool p9_srvconn_transport_is_open(const struct p9_srvconn_transport *st) {
    if (!st)                                     return false;
    if (st->magic != P9_SRVCONN_TRANSPORT_MAGIC) return false;
    return st->cn != NULL;
}
