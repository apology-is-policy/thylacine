// 9P Spoor-pair transport adapter (P5-spoor-transport).
//
// Plumbing: routes `struct p9_transport_ops` send / recv / close calls
// to `Spoor->dev->write` / `Spoor->dev->read` / spoor_clunk on the
// adapter's tx / rx Spoor pair. See header for layering + lifecycle.
//
// No state mutation beyond what tx_spoor's / rx_spoor's Devs do
// internally. The adapter holds pointers; the Devs own buffers, cursors,
// and any backing-state semantics.

#include <thylacine/9p_spoor_transport.h>
#include <thylacine/dev.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

// =============================================================================
// Vtable implementations.
// =============================================================================

static int spoor_transport_send(void *ctx, const u8 *buf, size_t len) {
    struct p9_spoor_transport *st = (struct p9_spoor_transport *)ctx;
    if (!st)                                   return -1;
    if (st->magic != P9_SPOOR_TRANSPORT_MAGIC) return -1;
    if (!st->tx_spoor || !st->tx_spoor->dev)   return -1;
    if (!st->tx_spoor->dev->write)             return -1;
    if (!buf && len > 0)                       return -1;

    // Loop on short writes — byte-pipe backends may accept partial
    // writes (matches the loopback discipline). Returns total bytes
    // written on success, 0 on closed pipe, -1 on error.
    //
    // Offset = 0 throughout: 9P framing is stream-style; the byte pipe
    // doesn't have a meaningful seek position. Devs that use offset
    // (e.g., a future block-style backend) will accept 0 + track their
    // own cursor.
    size_t total = 0;
    while (total < len) {
        long n = st->tx_spoor->dev->write(st->tx_spoor,
                                          buf + total,
                                          (long)(len - total),
                                          0);
        if (n < 0) return -1;
        if (n == 0) {
            // Pipe closed mid-write. Surface 0 if nothing was written;
            // otherwise the partial-write count communicates progress
            // to the transport core (which then sinks to ERROR).
            return (int)total;
        }
        total += (size_t)n;
    }
    return (int)total;
}

static int spoor_transport_recv(void *ctx, u8 *buf, size_t cap) {
    struct p9_spoor_transport *st = (struct p9_spoor_transport *)ctx;
    if (!st)                                   return -1;
    if (st->magic != P9_SPOOR_TRANSPORT_MAGIC) return -1;
    if (!st->rx_spoor || !st->rx_spoor->dev)   return -1;
    if (!st->rx_spoor->dev->read)              return -1;
    if (!buf || cap == 0)                      return -1;

    // Single read. Transport core handles partial-read aggregation by
    // calling us repeatedly until a complete frame is in hand.
    long n = st->rx_spoor->dev->read(st->rx_spoor,
                                     buf,
                                     (long)cap,
                                     0);
    if (n < 0) return -1;
    return (int)n;     // 0 means EOF (transport core surfaces error)
}

static int spoor_transport_close(void *ctx) {
    struct p9_spoor_transport *st = (struct p9_spoor_transport *)ctx;
    if (!st)                                   return -1;
    if (st->magic != P9_SPOOR_TRANSPORT_MAGIC) return -1;

    if (st->owns_spoors) {
        // Clunk both Spoors. Order: tx first, then rx (only if distinct
        // from tx). spoor_clunk handles dev->close → spoor_unref;
        // refcount drop frees the Spoor at ref 0.
        struct Spoor *tx = st->tx_spoor;
        struct Spoor *rx = st->rx_spoor;
        st->tx_spoor = NULL;
        st->rx_spoor = NULL;
        if (tx) spoor_clunk(tx);
        if (rx && rx != tx) spoor_clunk(rx);
    }
    // Magic stays valid post-close — destroy clobbers it. close is
    // idempotent under owns=false (no state to clear); under owns=true
    // a second close is a no-op (tx/rx are NULL).
    return 0;
}

// =============================================================================
// Public API.
// =============================================================================

int p9_spoor_transport_init(struct p9_spoor_transport *st,
                            struct Spoor *tx, struct Spoor *rx,
                            bool owns_spoors) {
    if (!st || !tx || !rx) return -1;
    st->magic       = P9_SPOOR_TRANSPORT_MAGIC;
    st->tx_spoor    = tx;
    st->rx_spoor    = rx;
    st->owns_spoors = owns_spoors;
    return 0;
}

void p9_spoor_transport_destroy(struct p9_spoor_transport *st) {
    if (!st) return;
    if (st->magic != P9_SPOOR_TRANSPORT_MAGIC) return;
    // Clobber magic FIRST so a concurrent observer fast-fails.
    st->magic       = 0;
    st->tx_spoor    = NULL;
    st->rx_spoor    = NULL;
    st->owns_spoors = false;
}

struct p9_transport_ops p9_spoor_transport_ops(struct p9_spoor_transport *st) {
    struct p9_transport_ops ops;
    ops.send  = spoor_transport_send;
    ops.recv  = spoor_transport_recv;
    ops.close = spoor_transport_close;
    // No deadline mechanism: a Spoor read blocks until data / EOF. The
    // deadline-aware reader pump (Loom SQPOLL) over a Spoor-backed client
    // simply blocks (never observes the idle return). NULL-permitted.
    ops.set_recv_deadline = NULL;
    ops.recv_timed_out    = NULL;
    ops.ctx               = (void *)st;
    return ops;
}

bool p9_spoor_transport_is_open(const struct p9_spoor_transport *st) {
    if (!st)                                   return false;
    if (st->magic != P9_SPOOR_TRANSPORT_MAGIC) return false;
    return st->tx_spoor != NULL;
}
