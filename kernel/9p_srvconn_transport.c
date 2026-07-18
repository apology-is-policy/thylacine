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

#include <thylacine/9p_client.h>
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
    if (len == 0)                                return 0;

    // #841: NO per-op deadline arming here. The pipelined client (ARCH §21.10)
    // decouples send from recv -- a different Thread may read the reply -- and
    // a per-op recv deadline that abandons one in-flight op desyncs the shared
    // byte stream (the stalk-3c bug §21.10 restores §21 to fix). The deadline
    // is now caller-set: HANDSHAKE_DEADLINE during the serial handshake, then 0
    // (block until reply / EOF / death; death-interruptible via #811) for
    // steady-state ops -- srvconn_attach_dev9p_root sets each.
    //
    // ALL-OR-NOTHING send (#841): srvconn_client_send_frame writes the WHOLE
    // frame or nothing (0 = ring full). With pipelining, c2s can transiently
    // hold a prior undrained frame, so a partial write would leave a fragment
    // on the wire + desync the shared stream; all-or-nothing avoids the fragment.
    // #349: a full-but-ALIVE ring (n == 0) is transient back-pressure, NOT a
    // death -- surface it as P9_TRANSPORT_EAGAIN so client_run drains the reply
    // path + retries instead of marking the whole session dead. A genuine break
    // (n < 0 = c2s eof / framing bug) stays fatal.
    long n = srvconn_client_send_frame(st->cn, buf, (long)len);
    if (n < 0)                return -1;                  // eof / framing -> fatal
    if (n == 0)               return P9_TRANSPORT_EAGAIN; // ring full but alive
    if ((size_t)n != len)     return -1;                 // partial (cannot happen)
    return (int)n;
}

static int srvconn_transport_recv(void *ctx, u8 *buf, size_t cap) {
    struct p9_srvconn_transport *st = (struct p9_srvconn_transport *)ctx;
    if (!st)                                     return -1;
    if (st->magic != P9_SRVCONN_TRANSPORT_MAGIC) return -1;
    if (!st->cn)                                 return -1;
    if (!buf || cap == 0)                        return -1;

    // #841: NO recv-side deadline auto-arm. The pipelined client (ARCH §21.10)
    // runs the elected reader's recv with NO per-op timeout for steady-state
    // ops -- the deadline is caller-set (srvconn_attach_dev9p_root arms
    // HANDSHAKE_DEADLINE for the serial handshake, then resets it to 0 = "block
    // until reply / EOF / death" for everything after). Auto-arming OP_DEADLINE
    // here would re-impose a per-op timeout whose expiry abandons one in-flight
    // op and DESYNCS the shared byte stream -- the stalk-3c bug §21.10 restores
    // §21 to fix. A genuinely-hung server now blocks the caller (death-
    // interruptible via #811), as Plan 9 / local Linux-9p do, rather than
    // corrupting a session shared across Procs.

    // BLOCKING. Returns:
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

// #841 + Loom-4: the steady-state deadline is caller-set, NOT auto-armed (see
// srvconn_transport_recv). These NULL-permitted ops let the Loom SQPOLL reader
// arm a frame-boundary idle deadline (LOOM.md §8.6): the deadline-aware pump
// arms before the FIRST recv of a frame (a timeout there consumes no bytes ->
// no desync) and disarms for the rest of the frame.
static void srvconn_transport_set_recv_deadline(void *ctx, u64 deadline_ns) {
    struct p9_srvconn_transport *st = (struct p9_srvconn_transport *)ctx;
    if (!st)                                     return;
    if (st->magic != P9_SRVCONN_TRANSPORT_MAGIC) return;
    if (!st->cn)                                 return;
    srvconn_set_client_deadline(st->cn, deadline_ns);
}

static bool srvconn_transport_recv_timed_out(void *ctx) {
    struct p9_srvconn_transport *st = (struct p9_srvconn_transport *)ctx;
    if (!st)                                     return false;
    if (st->magic != P9_SRVCONN_TRANSPORT_MAGIC) return false;
    if (!st->cn)                                 return false;
    return srvconn_client_timed_out(st->cn);
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
    ops.send              = srvconn_transport_send;
    ops.recv              = srvconn_transport_recv;
    ops.close             = srvconn_transport_close;
    ops.set_recv_deadline = srvconn_transport_set_recv_deadline;
    ops.recv_timed_out    = srvconn_transport_recv_timed_out;
    ops.ctx               = (void *)st;
    return ops;
}

bool p9_srvconn_transport_is_open(const struct p9_srvconn_transport *st) {
    if (!st)                                     return false;
    if (st->magic != P9_SRVCONN_TRANSPORT_MAGIC) return false;
    return st->cn != NULL;
}

struct SrvConn *p9_srvconn_transport_conn(const struct p9_client *c) {
    if (!c) return NULL;
    // The backend downcast (header contract): every transport ctx struct
    // leads with a distinct u32 magic, so a non-srvconn backend fails here
    // instead of mis-casting. Reading 4 bytes of any live ctx is safe; the
    // pointer returned is for identity comparison only.
    struct p9_srvconn_transport *st =
        (struct p9_srvconn_transport *)c->transport.ops.ctx;
    if (!st || st->magic != P9_SRVCONN_TRANSPORT_MAGIC) return NULL;
    return st->cn;
}
