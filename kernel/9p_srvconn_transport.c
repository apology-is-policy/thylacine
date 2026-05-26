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

#include "../arch/arm64/timer.h"

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

    // R1 F9 close: simplified. `srvconn_client_send` returns either
    // the full `len` (success), 0 (only when len == 0, handled above),
    // or -1 (torn rings / bad args). The "short write" loop the
    // pre-fix code carried was dead -- SRVCONN_RING_CAP is sized via a
    // _Static_assert to hold a full msize frame, so chan_produce
    // never returns a partial count for a kernel-client send.
    long n = srvconn_client_send(st->cn, buf, (long)len);
    if (n < 0)                return -1;
    if ((size_t)n != len)     return -1;   // unreachable per the assert
    return (int)n;
}

static int srvconn_transport_recv(void *ctx, u8 *buf, size_t cap) {
    struct p9_srvconn_transport *st = (struct p9_srvconn_transport *)ctx;
    if (!st)                                     return -1;
    if (st->magic != P9_SRVCONN_TRANSPORT_MAGIC) return -1;
    if (!st->cn)                                 return -1;
    if (!buf || cap == 0)                        return -1;

    // R1 F2 close (R2 F1R2 refinement): defense-in-depth deadline.
    //
    // The upper layer is RESPONSIBLE for setting the SrvConn's
    // `client_deadline_ns` before initiating a blocking op:
    //   - sys_attach_9p_srv_handler sets HANDSHAKE_DEADLINE before
    //     p9_attached_create's Tversion/Tattach (R1 F1 close);
    //   - the post-attach dev9p path (sys_walk_open / sys_read /
    //     sys_write KOBJ_SPOOR) is supposed to set OP_DEADLINE before
    //     each Twalk/Tread/Twrite, but at v1.0 those handlers do NOT
    //     (the R1 F2 finding).
    //
    // Rather than touch every dev9p call site, we auto-arm OP_DEADLINE
    // here when the deadline is missing OR already lapsed. The
    // missing/lapsed gate matters because:
    //
    //   - "Missing" (deadline == 0) is the fresh SrvConn case
    //     pre-handshake (srvconn_create initializes to 0).
    //
    //   - "Lapsed" (now >= deadline) is the post-handshake case
    //     R2 F1R2 surfaced: the R1 F1 setter armed HANDSHAKE_DEADLINE
    //     (now+5s) ONCE before the handshake. Post-handshake the
    //     value lingers; for any op past T_attach+5s the dev9p path
    //     would see an already-lapsed deadline and tsleep would
    //     return TIMEDOUT immediately even when the peer is healthy.
    //     The auto-arm re-fires per-op when the prior deadline has
    //     lapsed, restoring the documented "every recv bounded by a
    //     fresh OP_DEADLINE" backstop.
    //
    // Within a single op (header read + body read), the first recv
    // arms; subsequent recvs see a non-zero non-lapsed deadline and
    // skip the auto-arm (using whatever budget remains from the
    // first call). Between ops, the budget either still applies (if
    // within OP_DEADLINE wallclock) or is freshly re-armed (if past).
    //
    // A future dev9p refactor that arms a per-op deadline at the
    // call site BEFORE entering the adapter takes precedence because
    // it sets a non-zero non-lapsed value before this check.
    //
    // Atomic read on the deadline word is RELAXED: writer ordering
    // is enforced by srvconn_set_client_deadline's single-thread
    // discipline (one upper-layer caller per blocking op); cross-CPU
    // observers in v1.x multi-thread Procs are subsumed by the
    // outer-layer Proc lock / handle-table discipline (see R1 F4
    // dispositioned in the closed list).
    {
        u64 now_ns = timer_now_ns();
        u64 cur_deadline = __atomic_load_n(&st->cn->client_deadline_ns,
                                             __ATOMIC_RELAXED);
        if (cur_deadline == 0u || now_ns >= cur_deadline) {
            srvconn_set_client_deadline(st->cn,
                now_ns + SRVCONN_OP_DEADLINE_NS);
        }
    }

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
