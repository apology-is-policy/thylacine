// srvconn — a kernel-minted /srv connection (P5-corvus-srv-impl-a3a).
//
// Per CORVUS-DESIGN.md §6.2 + §6.3. See <thylacine/srvconn.h> for the
// design: the bidirectional byte transport, the tsleep-bounded client
// recv, by-value peer identity, and teardown ordering.
//
// This chunk lands the SrvConn object + its transport + lifecycle. The
// devsrv walk/open path, the bounded accept queue, and SYS_SRV_ACCEPT /
// SYS_SRV_PEER are P5-corvus-srv-impl-a3b / -a3c.

#include <thylacine/9p_client.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/rendez.h>
#include <thylacine/spinlock.h>
#include <thylacine/srvconn.h>
#include <thylacine/types.h>

#include "../mm/slub.h"

// The connection's 9P root fid. The kernel 9P client binds this at
// handshake (driven by the open path, a3b); the value is a transport-
// internal detail, never crosses a syscall boundary.
#define SRVCONN_ROOT_FID  1u

static u64 g_srvconn_created;
static u64 g_srvconn_freed;

// =============================================================================
// Ring buffer ops. PRECONDITION: the caller holds the channel's lock.
// =============================================================================

static long chan_ring_write(struct srvconn_chan *ch, const u8 *buf, long n) {
    if (n <= 0) return 0;

    u32 avail = SRVCONN_RING_CAP - ch->count;
    u32 want  = ((long)avail < n) ? avail : (u32)n;
    if (want == 0) return 0;

    // Two-segment copy: from head to end-of-buf, then wrap.
    u32 first = SRVCONN_RING_CAP - ch->head;
    if (first > want) first = want;
    for (u32 i = 0; i < first; i++) {
        ch->buf[ch->head + i] = buf[i];
    }
    u32 second = want - first;
    for (u32 i = 0; i < second; i++) {
        ch->buf[i] = buf[first + i];
    }

    ch->head   = (ch->head + want) % SRVCONN_RING_CAP;
    ch->count += want;
    return (long)want;
}

static long chan_ring_read(struct srvconn_chan *ch, u8 *buf, long n) {
    if (n <= 0) return 0;

    u32 want = ((long)ch->count < n) ? ch->count : (u32)n;
    if (want == 0) return 0;

    u32 first = SRVCONN_RING_CAP - ch->tail;
    if (first > want) first = want;
    for (u32 i = 0; i < first; i++) {
        buf[i] = ch->buf[ch->tail + i];
    }
    u32 second = want - first;
    for (u32 i = 0; i < second; i++) {
        buf[first + i] = ch->buf[i];
    }

    ch->tail   = (ch->tail + want) % SRVCONN_RING_CAP;
    ch->count -= want;
    return (long)want;
}

// =============================================================================
// Channel helpers.
// =============================================================================

static void chan_init(struct srvconn_chan *ch) {
    // kmalloc(KP_ZERO) already zeroed the channel — a zeroed spin_lock_t
    // is the unlocked form, a zeroed Rendez is {unlocked, no waiter}.
    // The explicit init is the documented contract (mirrors pipe.c).
    spin_lock_init(&ch->lock);
    ch->count = 0;
    ch->head  = 0;
    ch->tail  = 0;
    ch->eof   = false;
    rendez_init(&ch->rendez);
}

// chan_cond_readable — tsleep's wait predicate for the blocking client
// recv. Reads count / eof WITHOUT the channel lock: tsleep evaluates it
// under rendez->lock, and the producer's path (chan_produce / chan_set_
// eof) mutates the channel under ch->lock and then calls wakeup(), whose
// rendez->lock acquisition provides the happens-before with this read.
// The discipline matches devpipe's cond_can_read (specs/pipe.tla).
static int chan_cond_readable(void *arg) {
    struct srvconn_chan *ch = (struct srvconn_chan *)arg;
    return (ch->count > 0) || ch->eof;
}

// chan_produce — append `n` bytes; wake a blocked consumer. Returns
// bytes accepted (0..n), or -1 if this direction has latched EOF.
static long chan_produce(struct srvconn_chan *ch, const u8 *buf, long n) {
    spin_lock(&ch->lock);
    if (ch->eof) {
        spin_unlock(&ch->lock);
        return -1;
    }
    long put = chan_ring_write(ch, buf, n);
    spin_unlock(&ch->lock);
    // Wake outside the channel lock — wakeup takes the rendez's own
    // lock; the discipline is devpipe_write's (specs/pipe.tla).
    if (put > 0) wakeup(&ch->rendez);
    return put;
}

// chan_consume_nonblock — drain up to `n` bytes without blocking.
// Returns bytes read (>0), 0 if empty-but-live, or -1 if EOF (the
// channel is torn down and drained).
static long chan_consume_nonblock(struct srvconn_chan *ch, u8 *buf, long n) {
    spin_lock(&ch->lock);
    if (ch->count > 0) {
        long got = chan_ring_read(ch, buf, n);
        spin_unlock(&ch->lock);
        return got;
    }
    bool eof = ch->eof;
    spin_unlock(&ch->lock);
    return eof ? -1 : 0;
}

// chan_set_eof — latch EOF on a direction + wake its blocked consumer.
static void chan_set_eof(struct srvconn_chan *ch) {
    spin_lock(&ch->lock);
    ch->eof = true;
    spin_unlock(&ch->lock);
    wakeup(&ch->rendez);
}

// =============================================================================
// Transport vtable — the kernel 9P client's view of the connection.
// =============================================================================

static int srvconn_xport_send(void *ctx, const u8 *buf, size_t len) {
    if (len > (size_t)SRVCONN_RING_CAP) return -1;     // frame can't fit
    long r = srvconn_client_send((struct SrvConn *)ctx, buf, (long)len);
    return (int)r;
}

static int srvconn_xport_recv(void *ctx, u8 *buf, size_t cap) {
    if (cap > (size_t)SRVCONN_RING_CAP) cap = SRVCONN_RING_CAP;
    long r = srvconn_client_recv((struct SrvConn *)ctx, buf, (long)cap);
    return (int)r;
}

static int srvconn_xport_close(void *ctx) {
    srvconn_teardown((struct SrvConn *)ctx);
    return 0;
}

static struct p9_transport_ops srvconn_transport_ops(struct SrvConn *cn) {
    struct p9_transport_ops ops;
    ops.send  = srvconn_xport_send;
    ops.recv  = srvconn_xport_recv;
    ops.close = srvconn_xport_close;
    ops.ctx   = (void *)cn;
    return ops;
}

// =============================================================================
// Lifecycle.
// =============================================================================

struct SrvConn *srvconn_create(u64 peer_stripes, int peer_pid,
                               bool peer_console) {
    struct SrvConn *cn = kmalloc(sizeof(*cn), KP_ZERO);
    if (!cn) return NULL;

    cn->recv_buf = kmalloc(SRVCONN_MSIZE, KP_ZERO);
    if (!cn->recv_buf) {
        kfree(cn);
        return NULL;
    }
    cn->client = kmalloc(sizeof(struct p9_client), KP_ZERO);
    if (!cn->client) {
        kfree(cn->recv_buf);
        kfree(cn);
        return NULL;
    }

    chan_init(&cn->c2s);
    chan_init(&cn->s2c);

    // Configure the dedicated synchronous 9P client over this
    // connection's transport. p9_client_init performs no I/O — the
    // Tversion/Tattach handshake is driven later by the open path.
    int rc = p9_client_init(cn->client, SRVCONN_ROOT_FID, SRVCONN_MSIZE,
                            srvconn_transport_ops(cn),
                            cn->recv_buf, SRVCONN_MSIZE);
    if (rc != 0) {
        kfree(cn->client);
        kfree(cn->recv_buf);
        kfree(cn);
        return NULL;
    }

    spin_lock_init(&cn->lock);
    cn->state              = SRVCONN_STATE_LIVE;
    cn->peer_stripes       = peer_stripes;
    cn->peer_pid           = peer_pid;
    cn->peer_console       = peer_console;
    cn->client_deadline_ns = 0;
    cn->client_timed_out   = false;
    __atomic_store_n(&cn->ref, 1, __ATOMIC_RELAXED);

    // Publish the magic LAST — until this store the struct is not a
    // valid SrvConn, so a torn observer fast-fails the magic check.
    cn->magic = SRV_CONN_MAGIC;

    __atomic_fetch_add(&g_srvconn_created, 1u, __ATOMIC_RELAXED);
    return cn;
}

void srvconn_ref(struct SrvConn *cn) {
    if (!cn || cn->magic != SRV_CONN_MAGIC)
        extinction("srvconn_ref: NULL or corrupted SrvConn");
    int pre = __atomic_fetch_add(&cn->ref, 1, __ATOMIC_RELAXED);
    if (pre <= 0)
        extinction("srvconn_ref: refcount was <= 0 (use-after-free?)");
}

void srvconn_unref(struct SrvConn *cn) {
    if (!cn) return;
    if (cn->magic != SRV_CONN_MAGIC)
        extinction("srvconn_unref: corrupted SrvConn (double-free / UAF?)");

    // ACQ_REL so the last unref's frees are ordered after every prior
    // holder's writes (mirrors pipe.c's ring refcount discipline).
    int pre = __atomic_fetch_sub(&cn->ref, 1, __ATOMIC_ACQ_REL);
    if (pre <= 0)
        extinction("srvconn_unref: refcount underflow");
    if (pre != 1) return;

    // Last reference. Tear down (idempotent — a prior teardown is fine),
    // destroy the 9P client, then free all storage. Clobber the magic
    // before the kfrees so a racing observer of the freed object fast-
    // fails rather than dereferencing freed memory.
    srvconn_teardown(cn);
    p9_client_destroy(cn->client);
    cn->magic = 0;
    kfree(cn->client);
    kfree(cn->recv_buf);
    kfree(cn);
    __atomic_fetch_add(&g_srvconn_freed, 1u, __ATOMIC_RELAXED);
}

void srvconn_teardown(struct SrvConn *cn) {
    if (!cn) return;
    if (cn->magic != SRV_CONN_MAGIC)
        extinction("srvconn_teardown: corrupted SrvConn");

    spin_lock(&cn->lock);
    if (cn->state == SRVCONN_STATE_TORN) {
        spin_unlock(&cn->lock);
        return;                              // idempotent
    }
    cn->state = SRVCONN_STATE_TORN;
    spin_unlock(&cn->lock);

    // Latch EOF on both directions + wake any blocked consumer. cn->lock
    // is already released; the two channels are independent leaves, so
    // there is no lock-ordering relation to honor.
    chan_set_eof(&cn->c2s);
    chan_set_eof(&cn->s2c);
}

bool srvconn_is_live(const struct SrvConn *cn) {
    if (!cn || cn->magic != SRV_CONN_MAGIC) return false;
    // `state` is a one-way LIVE→TORN transition; an unlocked read sees
    // one or the other, both honest observations.
    return cn->state == SRVCONN_STATE_LIVE;
}

// =============================================================================
// Kernel 9P client accessors.
// =============================================================================

struct p9_client *srvconn_client(struct SrvConn *cn) {
    if (!cn || cn->magic != SRV_CONN_MAGIC) return NULL;
    return cn->client;
}

void srvconn_set_client_deadline(struct SrvConn *cn, u64 deadline_ns) {
    if (!cn || cn->magic != SRV_CONN_MAGIC)
        extinction("srvconn_set_client_deadline: NULL or corrupted SrvConn");
    cn->client_deadline_ns = deadline_ns;
    cn->client_timed_out   = false;
}

bool srvconn_client_timed_out(const struct SrvConn *cn) {
    if (!cn || cn->magic != SRV_CONN_MAGIC) return false;
    return cn->client_timed_out;
}

// =============================================================================
// Raw byte transport.
// =============================================================================

long srvconn_client_send(struct SrvConn *cn, const u8 *buf, long n) {
    if (!cn || cn->magic != SRV_CONN_MAGIC) return -1;
    if (!buf || n < 0) return -1;
    if (n == 0) return 0;
    return chan_produce(&cn->c2s, buf, n);
}

long srvconn_client_recv(struct SrvConn *cn, u8 *buf, long n) {
    if (!cn || cn->magic != SRV_CONN_MAGIC) return -1;
    if (!buf || n < 0) return -1;
    if (n == 0) return 0;

    struct srvconn_chan *ch = &cn->s2c;
    for (;;) {
        spin_lock(&ch->lock);
        if (ch->count > 0) {
            long got = chan_ring_read(ch, buf, n);
            spin_unlock(&ch->lock);
            return got;
        }
        if (ch->eof) {
            spin_unlock(&ch->lock);
            return 0;                        // EOF — torn down, drained
        }
        spin_unlock(&ch->lock);

        // Block until data, EOF, or the deadline. tsleep re-checks the
        // condition under rendez->lock; a wakeup that arrives between
        // the unlock above and the sleep transition is not lost
        // (specs/scheduler.tla NoMissedWakeup, specs/tsleep.tla).
        int ts = tsleep(&ch->rendez, chan_cond_readable, ch,
                        cn->client_deadline_ns);
        if (ts == TSLEEP_TIMEDOUT) {
            cn->client_timed_out = true;
            return -1;                       // corvus hung past the deadline
        }
        // TSLEEP_AWOKEN — loop, re-check the channel under the lock.
    }
}

long srvconn_server_send(struct SrvConn *cn, const u8 *buf, long n) {
    if (!cn || cn->magic != SRV_CONN_MAGIC) return -1;
    if (!buf || n < 0) return -1;
    if (n == 0) return 0;
    return chan_produce(&cn->s2c, buf, n);
}

long srvconn_server_recv(struct SrvConn *cn, u8 *buf, long n) {
    if (!cn || cn->magic != SRV_CONN_MAGIC) return -1;
    if (!buf || n < 0) return -1;
    if (n == 0) return 0;
    return chan_consume_nonblock(&cn->c2s, buf, n);
}

// =============================================================================
// Diagnostics.
// =============================================================================

u64 srvconn_total_created(void) {
    return __atomic_load_n(&g_srvconn_created, __ATOMIC_RELAXED);
}

u64 srvconn_total_freed(void) {
    return __atomic_load_n(&g_srvconn_freed, __ATOMIC_RELAXED);
}
