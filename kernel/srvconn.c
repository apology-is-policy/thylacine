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
#include <thylacine/poll.h>
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
                               bool peer_console, u64 server_stripes) {
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
    poll_waiter_list_init(&cn->poll_list);

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
    cn->server_stripes     = server_stripes;
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

    // Latch EOF on BOTH directions inside ONE dual-lock critical section,
    // so a poller (srvconn_poll holds c2s.lock + s2c.lock simultaneously
    // — same fixed order) cannot observe c2s.eof=true with s2c.eof=false:
    // a mid-teardown poll otherwise sees POLLHUP without POLLERR, which
    // the documentation forbids. Lock order c2s → s2c matches srvconn_poll
    // — no other path takes both, so the nested acquire is safe.
    spin_lock(&cn->c2s.lock);
    spin_lock(&cn->s2c.lock);
    cn->c2s.eof = true;
    cn->s2c.eof = true;
    spin_unlock(&cn->s2c.lock);
    spin_unlock(&cn->c2s.lock);

    // Wake every blocked consumer + every registered poller. Wakes happen
    // outside the channel locks (the wakeup / poll-list paths take the
    // rendez / list locks; producer-mutates-then-wakes is the chan_produce
    // discipline). Both EOFs are visible to any wake observer.
    wakeup(&cn->c2s.rendez);
    wakeup(&cn->s2c.rendez);

    // Teardown is a single readiness edge for every server-endpoint
    // poller: POLLHUP latches off c2s.eof, POLLERR off s2c.eof. ONE wake
    // walks every registered hook. specs/poll.tla MakeReady.
    poll_waiter_list_wake(&cn->poll_list);
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
// Peer identity accessors (SYS_SRV_PEER).
// =============================================================================
//
// The identity fields are immutable for the SrvConn's life — set once by
// srvconn_create, never rewritten. Each accessor revalidates the magic so
// a torn / freed-object read fail-closes (0 / false) rather than yielding
// a stale tag.

u64 srvconn_peer_stripes(const struct SrvConn *cn) {
    if (!cn || cn->magic != SRV_CONN_MAGIC) return 0;
    return cn->peer_stripes;
}

bool srvconn_peer_console(const struct SrvConn *cn) {
    if (!cn || cn->magic != SRV_CONN_MAGIC) return false;
    return cn->peer_console;
}

u64 srvconn_server_stripes(const struct SrvConn *cn) {
    if (!cn || cn->magic != SRV_CONN_MAGIC) return 0;
    return cn->server_stripes;
}

// =============================================================================
// Client-side 9P session (P5-corvus-srv-impl-b2).
//
// SrvConn's embedded p9_client connects this client's bytes to the 9P
// server (corvus); these helpers drive the handshake + the per-op
// Tread/Twrite on behalf of SYS_READ / SYS_WRITE.
// =============================================================================

// Linux open flags. p9_client_lopen takes a u32 of Linux O_* bits (the
// dev9p mapping uses these directly; CORVUS-DESIGN's /ctl + /ops/* are
// bidirectional request/response, so the client opens RDWR).
#define SRVCONN_OPEN_RDWR  2u

// The handshake-derived client fid. Root is SRVCONN_ROOT_FID = 1; a
// Twalk lands the child fid at 2 (no concurrent walks at v1.0 — single-
// thread-per-Proc means one walked fid per SrvConn).
#define SRVCONN_CLIENT_WALK_FID  2u

// uname for the kernel-driven Tattach. Identifies the kernel-side
// originator; corvus does NOT trust this for authorization (the
// authoritative peer identity is SYS_SRV_PEER), but it's the wire-
// visible name. "thylacine" matches the OS identity.
static const u8 SRVCONN_TATTACH_UNAME[] = "thylacine";

int srvconn_drive_client_handshake(struct SrvConn *cn, int peer_pid,
                                    const u8 *path, size_t path_len) {
    if (!cn || cn->magic != SRV_CONN_MAGIC)             return -1;
    if (cn->client_handshake_done)                       return -1;
    if (!cn->client)                                     return -1;
    if (!srvconn_is_live(cn))                            return -1;
    if (path_len > 0 && !path)                           return -1;

    // n_uname conveys the peer Proc's pid in Tattach. negative pid maps
    // to ~0u sentinel (corvus is expected to ignore n_uname for auth).
    u32 n_uname = (peer_pid >= 0) ? (u32)peer_pid : ~0u;

    int rc = p9_client_handshake(cn->client,
                                  SRVCONN_TATTACH_UNAME,
                                  sizeof(SRVCONN_TATTACH_UNAME) - 1,
                                  NULL, 0,
                                  n_uname);
    if (rc != 0) return -1;

    u32 open_fid = SRVCONN_ROOT_FID;
    if (path_len > 0) {
        // Walk root → walk_fid along `path` (one component), then Tlopen
        // it for RDWR — the wire-frame request/response on /ctl + /ops/*.
        struct p9_qid qid;
        rc = p9_client_walk_one(cn->client,
                                 SRVCONN_ROOT_FID, SRVCONN_CLIENT_WALK_FID,
                                 path, path_len, &qid);
        if (rc != 0) return -1;
        u32 iounit;
        rc = p9_client_lopen(cn->client, SRVCONN_CLIENT_WALK_FID,
                              SRVCONN_OPEN_RDWR, &qid, &iounit);
        if (rc != 0) return -1;
        open_fid = SRVCONN_CLIENT_WALK_FID;
    } else {
        // No path: leave the open at root_fid. Useful for tests + for
        // future syscalls that walk explicitly after connect.
        struct p9_qid qid;
        u32 iounit;
        rc = p9_client_lopen(cn->client, SRVCONN_ROOT_FID,
                              SRVCONN_OPEN_RDWR, &qid, &iounit);
        if (rc != 0) return -1;
    }

    cn->client_fid             = open_fid;
    cn->client_offset          = 0;
    cn->client_handshake_done  = true;
    return 0;
}

long srvconn_client_read(struct SrvConn *cn, u8 *buf, long n) {
    if (!cn || cn->magic != SRV_CONN_MAGIC)             return -1;
    if (!buf || n < 0)                                   return -1;
    if (n == 0)                                          return 0;
    if (!cn->client_handshake_done)                      return -1;
    if (!srvconn_is_live(cn))                            return -1;
    // p9_client_read takes a u32 count; cap before passing.
    u32 count = (n > 0x7fffffffL) ? 0x7fffffffu : (u32)n;
    u32 got = 0;
    int rc = p9_client_read(cn->client, cn->client_fid,
                             cn->client_offset, count, buf, &got);
    if (rc != 0) return -1;
    cn->client_offset += (u64)got;
    return (long)got;
}

long srvconn_client_write(struct SrvConn *cn, const u8 *buf, long n) {
    if (!cn || cn->magic != SRV_CONN_MAGIC)             return -1;
    if (!buf || n < 0)                                   return -1;
    if (n == 0)                                          return 0;
    if (!cn->client_handshake_done)                      return -1;
    if (!srvconn_is_live(cn))                            return -1;
    u32 count = (n > 0x7fffffffL) ? 0x7fffffffu : (u32)n;
    u32 accepted = 0;
    int rc = p9_client_write(cn->client, cn->client_fid,
                              cn->client_offset, count, buf, &accepted);
    if (rc != 0) return -1;
    cn->client_offset += (u64)accepted;
    return (long)accepted;
}

// =============================================================================
// Raw byte transport.
// =============================================================================

long srvconn_client_send(struct SrvConn *cn, const u8 *buf, long n) {
    if (!cn || cn->magic != SRV_CONN_MAGIC) return -1;
    if (!buf || n < 0) return -1;
    if (n == 0) return 0;
    long put = chan_produce(&cn->c2s, buf, n);
    if (put > 0) {
        // A non-empty c2s ring becomes POLLIN-ready for every poller
        // registered on the server endpoint. chan_produce already woke
        // the (single) blocking-recv Rendez; this wakes the poll list.
        // specs/poll.tla MakeReady.
        poll_waiter_list_wake(&cn->poll_list);
    }
    return put;
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
    // No poll wake here at v1.0. srvconn_poll's server-endpoint POLLOUT is
    // `!s2c.eof && s2c.count < SRVCONN_RING_CAP`: increasing s2c.count can
    // only REDUCE POLLOUT-ready space, never make it more ready. The
    // POLLOUT-becomes-ready edge is the kernel-client drain in
    // srvconn_client_recv — and the kernel client doesn't poll (it
    // tsleep-blocks). Restoring the wake belongs with the future client-
    // side poll path (cn->client_poll_list), which would observe s2c FILL
    // as a POLLIN edge.
    return chan_produce(&cn->s2c, buf, n);
}

long srvconn_server_recv(struct SrvConn *cn, u8 *buf, long n) {
    if (!cn || cn->magic != SRV_CONN_MAGIC) return -1;
    if (!buf || n < 0) return -1;
    if (n == 0) return 0;
    return chan_consume_nonblock(&cn->c2s, buf, n);
}

// =============================================================================
// poll — readiness probe on the server endpoint Spoor (P5-poll-b).
// =============================================================================
//
// Both channel locks are taken in a fixed order (c2s → s2c) across the
// sample-and-register critical section, matching kernel/pipe.c's r->lock-
// across-sample-and-register discipline. No other path takes both locks
// (chan_produce / chan_consume_nonblock / chan_set_eof each take a single
// ch->lock), so this dual-lock acquire cannot deadlock with itself or with
// any producer.
//
// pw == NULL is the post-wake sample-only call (sys_poll_for_proc's
// second scan); pw != NULL atomically registers the hook with the sample
// under the same locks — the register-then-observe step.

short srvconn_poll(struct SrvConn *cn, short events, struct poll_waiter *pw) {
    if (!cn || cn->magic != SRV_CONN_MAGIC) return POLLERR;

    spin_lock(&cn->c2s.lock);
    spin_lock(&cn->s2c.lock);

    short revents = 0;
    if (cn->c2s.count > 0) revents |= POLLIN;     // bytes for corvus to read
    if (cn->c2s.eof)       revents |= POLLHUP;    // teardown latched it
    if (!cn->s2c.eof && cn->s2c.count < SRVCONN_RING_CAP) {
        revents |= POLLOUT;                       // room for corvus to write
    }
    if (cn->s2c.eof)       revents |= POLLERR;    // server-side writes EPIPE

    if (pw) {
        poll_waiter_list_register(&cn->poll_list, pw);
    }

    spin_unlock(&cn->s2c.lock);
    spin_unlock(&cn->c2s.lock);

    // POSIX: POLLIN/POLLOUT only when requested; POLLHUP/POLLERR always.
    return (short)(revents & (events | POLL_OUTPUT_ONLY));
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
