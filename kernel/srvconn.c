// srvconn — a kernel-minted /srv connection (P5-corvus-srv-impl-a3a).
//
// Per CORVUS-DESIGN.md §6.2 + §6.3. See <thylacine/srvconn.h> for the
// design: the bidirectional byte transport, the tsleep-bounded client
// recv, by-value peer identity, and teardown ordering.
//
// This chunk lands the SrvConn object + its transport + lifecycle. The
// devsrv walk/open path, the bounded accept queue, and SYS_SRV_ACCEPT /
// SYS_SRV_PEER are P5-corvus-srv-impl-a3b / -a3c.

#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/poll.h>
#include <thylacine/rendez.h>
#include <thylacine/spinlock.h>
#include <thylacine/srvconn.h>
#include <thylacine/types.h>
#include <atomic_lse.h>   // t_atomic_fetch_{add_relaxed,sub_acqrel}_int (W1.5 LSE-patchable refcount)

#include "../mm/slub.h"

// SRVCONN_ROOT_FID moved to <thylacine/srvconn.h> (stalk-3b-β) so the shared
// srvconn_attach_dev9p_root helper (9p_attach.c) can pass the same root fid.

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
    ch->count   = 0;
    ch->head    = 0;
    ch->tail    = 0;
    ch->eof     = false;
    ch->writing = false;
    rendez_init(&ch->rendez);
    rendez_init(&ch->wrendez);
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

// chan_cond_writable — tsleep's wait predicate for the blocking server
// send (#348): the ring has room (count < cap) OR has latched EOF. Reads
// count / eof WITHOUT the channel lock for the same reason chan_cond_
// readable does — tsleep evaluates it under wrendez->lock, and the
// consumer's drain (srvconn_client_recv) decrements count under ch->lock
// then wakeup(wrendez), whose lock acquisition provides the happens-
// before with this read. The mirror of chan_cond_readable (specs/tsleep
// .tla register-then-observe). The `|| eof` means "stop blocking"
// (teardown), NOT "room available": a producer woken on eof re-runs
// chan_produce, observes eof, and fails -- it never writes a full+torn
// ring (#348 audit F3).
static int chan_cond_writable(void *arg) {
    struct srvconn_chan *ch = (struct srvconn_chan *)arg;
    return (ch->count < SRVCONN_RING_CAP) || ch->eof;
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
// Lifecycle.
// =============================================================================

struct SrvConn *srvconn_create(u64 peer_stripes, int peer_pid,
                               bool peer_console, u64 server_stripes) {
    struct SrvConn *cn = kmalloc(sizeof(*cn), KP_ZERO);
    if (!cn) return NULL;

    chan_init(&cn->c2s);
    chan_init(&cn->s2c);
    poll_waiter_list_init(&cn->poll_list);

    spin_lock_init(&cn->lock);
    cn->state              = SRVCONN_STATE_LIVE;
    cn->peer_stripes       = peer_stripes;
    cn->peer_pid           = peer_pid;
    cn->peer_console       = peer_console;
    cn->server_stripes     = server_stripes;
    cn->client_deadline_ns = 0;
    cn->client_timed_out   = false;
    /* byte_mode = false by KP_ZERO; srvconn_set_byte_mode flips on after
     * mint if the service is SRV_MODE_BYTE (P6-pouch-sockets). */
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
    int pre = t_atomic_fetch_add_relaxed_int(&cn->ref, 1);
    if (pre <= 0)
        extinction("srvconn_ref: refcount was <= 0 (use-after-free?)");
}

void srvconn_set_byte_mode(struct SrvConn *cn) {
    if (!cn || cn->magic != SRV_CONN_MAGIC)
        extinction("srvconn_set_byte_mode: NULL or corrupted SrvConn");
    // F6 close (P6-pouch-sockets audit): idempotency assertion. The
    // setter's contract is one-way (false -> true) and runs at mint,
    // BEFORE the conn is published / enqueued / kernel-attached. A buggy
    // caller that flipped byte_mode on a conn already wrapped by the
    // kernel 9P client (srvconn_attach_dev9p_root) would corrupt a live
    // 9P session; catching it here turns a quiet corruption into a clear
    // extinction. (stalk-3b-β: the prior guard read client_handshake_done
    // on the now-retired embedded 9P client; kernel_attached is the
    // post-retirement "conn is already in 9P use" signal.)
    if (srvconn_is_kernel_attached(cn))
        extinction("srvconn_set_byte_mode: kernel-attached "
                   "(mode flip on a published SrvConn)");
    // F5 close (P6-pouch-sockets audit): ATOMIC_RELEASE so cross-CPU
    // observers (sys_read/write_for_proc's KOBJ_SRV arm, devsrv_read)
    // doing ATOMIC_ACQUIRE see the byte_mode write in the correct
    // ordering vs the SrvConn publication (handle_alloc + backlog
    // push). Pre-publication AND release-ordered is belt-and-braces:
    // current observers are protected by the publication-as-barrier;
    // future observers that bypass the publication barrier (a planned
    // cross-CPU peek) still get the right value.
    __atomic_store_n(&cn->byte_mode, true, __ATOMIC_RELEASE);
}

void srvconn_set_kernel_attached(struct SrvConn *cn) {
    if (!cn || cn->magic != SRV_CONN_MAGIC)
        extinction("srvconn_set_kernel_attached: NULL or corrupted SrvConn");
    // Atomic release pairs with srvconn_is_kernel_attached's acquire on
    // the userspace-close path (handle_close on KOBJ_SRV). Once flipped,
    // teardown is the kernel-side adapter's responsibility, not the
    // userspace close.
    __atomic_store_n(&cn->kernel_attached, true, __ATOMIC_RELEASE);
}

bool srvconn_is_kernel_attached(const struct SrvConn *cn) {
    if (!cn || cn->magic != SRV_CONN_MAGIC) return false;
    return __atomic_load_n(&cn->kernel_attached, __ATOMIC_ACQUIRE);
}

void srvconn_unref(struct SrvConn *cn) {
    if (!cn) return;
    if (cn->magic != SRV_CONN_MAGIC)
        extinction("srvconn_unref: corrupted SrvConn (double-free / UAF?)");

    // ACQ_REL so the last unref's frees are ordered after every prior
    // holder's writes (mirrors pipe.c's ring refcount discipline).
    int pre = t_atomic_fetch_sub_acqrel_int(&cn->ref, 1);
    if (pre <= 0)
        extinction("srvconn_unref: refcount underflow");
    if (pre != 1) return;

    // Last reference. Tear down (idempotent — a prior teardown is fine),
    // then free the connection. Clobber the magic before the kfree so a
    // racing observer of the freed object fast-fails rather than
    // dereferencing freed memory.
    srvconn_teardown(cn);
    cn->magic = 0;
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

    // Wake every blocked consumer + every blocked producer + every
    // registered poller. Wakes happen outside the channel locks (the
    // wakeup / poll-list paths take the rendez / list locks; producer-
    // mutates-then-wakes is the chan_produce discipline). Both EOFs are
    // visible to any wake observer. The wrendez wakes (#348) release a
    // blocking server send parked on a full s2c; c2s.wrendez never has a
    // waiter (the c2s producer is non-blocking) but is woken for symmetry
    // — a wake with no sleeper is a no-op.
    wakeup(&cn->c2s.rendez);
    wakeup(&cn->s2c.rendez);
    wakeup(&cn->c2s.wrendez);
    wakeup(&cn->s2c.wrendez);

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
// Client-side blocking-recv deadline.
// =============================================================================

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

long srvconn_client_send_frame(struct SrvConn *cn, const u8 *buf, long n) {
    if (!cn || cn->magic != SRV_CONN_MAGIC) return -1;
    if (!buf || n < 0) return -1;
    if (n == 0) return 0;
    // A whole 9P frame is <= the negotiated msize <= SRVCONN_RING_CAP (the
    // _Static_assert), so a frame can ALWAYS fit an empty ring; n past the ring
    // is a framing-layer bug, not back-pressure.
    if (n > (long)SRVCONN_RING_CAP) return -1;
    struct srvconn_chan *ch = &cn->c2s;
    spin_lock(&ch->lock);
    if (ch->eof) {
        spin_unlock(&ch->lock);
        return -1;
    }
    // #841 ALL-OR-NOTHING. The pipelined kernel 9P client can have several
    // frames in flight, so c2s may transiently hold a prior undrained frame; a
    // partial write (which chan_produce / chan_ring_write would do on a nearly-
    // full ring) leaves a fragment on the wire and DESYNCS the shared stream.
    // Write the WHOLE frame iff it fits; else write NOTHING + return 0 (the
    // caller fails the op + marks the session dead -- no fragment on the wire).
    // Frame-atomicity vs concurrent senders is the caller's (the client holds
    // c->lock across the send).
    u32 freeb = SRVCONN_RING_CAP - ch->count;
    if ((u32)n > freeb) {
        spin_unlock(&ch->lock);
        return 0;                       // no room -- all-or-nothing back-pressure
    }
    long put = chan_ring_write(ch, buf, n);   // room guaranteed -> writes all n
    spin_unlock(&ch->lock);
    if (put > 0) {
        wakeup(&ch->rendez);
        poll_waiter_list_wake(&cn->poll_list);
    }
    return put;
}

long srvconn_client_recv(struct SrvConn *cn, u8 *buf, long n) {
    if (!cn || cn->magic != SRV_CONN_MAGIC) return -1;
    if (!buf || n < 0) return -1;
    if (n == 0) return 0;

    struct srvconn_chan *ch = &cn->s2c;

    // RW-4 R2-F1: claim the single-reader role. ch->rendez is single-waiter; a
    // 2nd concurrent blocking consumer would trip its single-waiter extinction.
    // The 9P-mode path is serialized by the kernel p9_client lock (one drainer),
    // but the byte-mode userspace read() path is not, and peer Threads share the
    // fd -- refuse the 2nd reader (-1) rather than register a 2nd waiter (the
    // devcons single-reader pattern). Released on every exit below.
    spin_lock(&ch->lock);
    if (ch->reading) { spin_unlock(&ch->lock); return -1; }
    ch->reading = true;
    spin_unlock(&ch->lock);

    long ret;
    for (;;) {
        spin_lock(&ch->lock);
        if (ch->count > 0) {
            ret = chan_ring_read(ch, buf, n);
            spin_unlock(&ch->lock);
            // #348: draining s2c made room -> wake a blocked server send
            // (srvconn_server_send_blocking parks on wrendez when s2c
            // fills). Wake OUTSIDE ch->lock (wakeup takes wrendez->lock --
            // the consumer-mutates-then-wakes discipline, mirroring how
            // chan_produce wakes ch->rendez after a write). A no-op when no
            // producer is parked.
            wakeup(&ch->wrendez);
            break;
        }
        if (ch->eof) {
            spin_unlock(&ch->lock);
            ret = 0;                          // EOF — torn down, drained
            break;
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
            ret = -1;                         // corvus hung past the deadline
            break;
        }
        // #811 (ARCH §8.8.1): death-interrupted -> Proc group-terminating;
        // return so the Thread unwinds to its EL0-return die-check.
        if (ts == TSLEEP_INTR) {
            ret = -1;
            break;
        }
        // TSLEEP_AWOKEN — loop, re-check the channel under the lock.
    }

    spin_lock(&ch->lock);
    ch->reading = false;
    spin_unlock(&ch->lock);
    return ret;
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

// srvconn_server_send_blocking — #348. The BLOCKING s2c producer: deliver
// the WHOLE buffer, parking on s2c.wrendez when the ring is full until the
// kernel client drains it (srvconn_client_recv wakes wrendez) or teardown
// latches eof. The s2c twin of #349's c2s client_send_flow. See srvconn.h
// for the contract + the EPIPE-close hazard it closes.
//
// The `writing` busy-guard mirrors srvconn_client_recv's `reading`: only
// ONE blocking producer parks on wrendez (stratumd is thread-per-conn; a
// 2nd concurrent producer is refused -1 -> fail-closed, never a 2nd
// single-waiter waiter). The guard is held across the WHOLE multi-chunk
// write, so the n bytes enter s2c contiguously vs any other producer
// (frame-atomicity). A mid-fill reader drain is safe: the kernel s2c
// reader already reassembles partial frames by size — it MUST, since the
// non-blocking srvconn_server_send already short-writes today.
//
// Deadlock-free composed with the #349 c2s blocking send: the kernel
// client is the guaranteed s2c drainer (its elected reader, or #349's
// self-pump when it is itself back-pressured on c2s), so a full s2c always
// has a draining party. No lock is held across the park (chan_produce
// releases ch->lock; `writing` is a plain bool guard, not a lock, and the
// reader uses the separate `reading` flag).
long srvconn_server_send_blocking(struct SrvConn *cn, const u8 *buf, long n) {
    if (!cn || cn->magic != SRV_CONN_MAGIC) return -1;
    if (!buf || n < 0) return -1;
    if (n == 0) return 0;

    struct srvconn_chan *ch = &cn->s2c;

    // V1.0 SINGLE-WRITER PRE-CONDITION (#348 audit F1, task #354). Claim the
    // single-writer role (mirror srvconn_client_recv's `reading`). A 2nd
    // concurrent blocking writer on one s2c is refused -1 -- which a 9P-server
    // Proc's write_full treats as fatal -> mount close (the very #348 cascade).
    // This is LATENT-only: stratumd is one-thread-per-conn, so one s2c has one
    // writer; the -1 mirrors the SHIPPED `reading` guard (a 2nd reader also
    // gets -1, relying on the #841 single-reader election). A threaded-server /
    // per-user-netd lift on THIS transport MUST first serialize per-conn s2c
    // writes server-side OR go multi-waiter on BOTH wrendez + rendez (the #349
    // c2s send_waiters_list precedent). Not built at v1.0 -- it is unverifiable
    // without the threaded server (no driving workload, no deterministic test).
    spin_lock(&ch->lock);
    if (ch->writing) { spin_unlock(&ch->lock); return -1; }
    ch->writing = true;
    spin_unlock(&ch->lock);

    long done = 0;
    long ret;
    for (;;) {
        // chan_produce writes what fits (eof-checked) + wakes the blocked
        // client reader (s2c.rendez). put < (n-done) means the ring filled
        // (chan_ring_write writes min(avail, n), so a short write exhausted
        // avail). put == 0 with bytes left likewise means a full ring.
        long put = chan_produce(ch, buf + done, n - done);
        if (put < 0) {                        // eof latched (teardown)
            ret = (done > 0) ? done : -1;
            break;
        }
        done += put;
        if (done >= n) { ret = done; break; } // whole buffer delivered

        // Ring full -> park until the kernel client drains s2c (wakes
        // wrendez) or teardown latches eof. tsleep re-checks chan_cond_
        // writable under wrendez->lock, so a drain between chan_produce and
        // here is not lost (specs/tsleep.tla register-then-observe).
        // deadline 0 = indefinite (the kernel client is the guaranteed
        // drainer; see the function header on deadlock-freedom).
        int ts = tsleep(&ch->wrendez, chan_cond_writable, ch, 0u);
        if (ts == TSLEEP_TIMEDOUT) {          // unreachable with deadline 0; defense in depth
            // #348 audit F4: a future non-zero server deadline here would need a
            // caller-visible `server_timed_out` signal (analog to client_timed_
            // out) so write_full distinguishes a deadline expiry from a short
            // write -- else the retry loop just re-parks. Add it WITH any deadline.
            ret = (done > 0) ? done : -1;
            break;
        }
        // #811 (ARCH §8.8.1): death-interrupted -> the Proc is group-
        // terminating; return so the Thread unwinds to its die-check.
        if (ts == TSLEEP_INTR) {
            ret = (done > 0) ? done : -1;
            break;
        }
        // TSLEEP_AWOKEN — loop, chan_produce again.
    }

    spin_lock(&ch->lock);
    ch->writing = false;
    spin_unlock(&ch->lock);
    return ret;
}

long srvconn_server_recv(struct SrvConn *cn, u8 *buf, long n) {
    if (!cn || cn->magic != SRV_CONN_MAGIC) return -1;
    if (!buf || n < 0) return -1;
    if (n == 0) return 0;
    return chan_consume_nonblock(&cn->c2s, buf, n);
}

// srvconn_server_recv_blocking — F1 close (P6-pouch-sockets audit).
//
// The non-blocking srvconn_server_recv above is designed for corvus's
// 9P-server pattern (poll-then-read; the userspace 9P responder uses
// t_poll on the listener + per-conn POLLIN signals before pulling
// frames off c2s). A POSIX userspace server reading SOCK_STREAM bytes
// — pouch's AF_UNIX consumer — expects blocking semantics: a read on
// an empty-but-live connection should block until data arrives, not
// return 0 (POSIX EOF). Returning 0 to a POSIX server is a spurious
// EOF that breaks the round trip when accept-wake races the client's
// first write across SMP CPUs.
//
// This blocking variant mirrors srvconn_client_recv's discipline
// against c2s instead of s2c: spin_lock the channel, check count > 0
// (read) / eof (return 0), else tsleep on the channel's Rendez until
// chan_produce wakes it or chan_set_eof latches eof. Deadline = 0
// blocks indefinitely — a future per-connection idle timer would
// extend by setting a server_deadline_ns analog to client_deadline_ns.
long srvconn_server_recv_blocking(struct SrvConn *cn, u8 *buf, long n) {
    if (!cn || cn->magic != SRV_CONN_MAGIC) return -1;
    if (!buf || n < 0) return -1;
    if (n == 0) return 0;

    struct srvconn_chan *ch = &cn->c2s;

    // RW-4 R2-F1: same single-reader busy-guard as srvconn_client_recv (this is
    // the byte-mode POSIX-server read() path, the most exposed concurrent-reader
    // case). A 2nd concurrent blocking reader is refused (-1), never a 2nd
    // single-waiter Rendez waiter.
    spin_lock(&ch->lock);
    if (ch->reading) { spin_unlock(&ch->lock); return -1; }
    ch->reading = true;
    spin_unlock(&ch->lock);

    long ret;
    for (;;) {
        spin_lock(&ch->lock);
        if (ch->count > 0) {
            ret = chan_ring_read(ch, buf, n);
            spin_unlock(&ch->lock);
            break;
        }
        if (ch->eof) {
            spin_unlock(&ch->lock);
            ret = 0;                          // EOF — connection torn down
            break;
        }
        spin_unlock(&ch->lock);

        // tsleep with deadline=0 — block indefinitely. wake fires from
        // srvconn_client_send's chan_produce (data arrived) or
        // srvconn_teardown's chan_set_eof on c2s (peer closed).
        int ts = tsleep(&ch->rendez, chan_cond_readable, ch, 0u);
        if (ts == TSLEEP_TIMEDOUT) {
            ret = -1;                         // Unreachable with deadline=0; defense in depth.
            break;
        }
        // #811 (ARCH §8.8.1): death-interrupted -> Proc group-terminating;
        // return so the Thread unwinds to its EL0-return die-check.
        if (ts == TSLEEP_INTR) {
            ret = -1;
            break;
        }
        // TSLEEP_AWOKEN — loop, re-check the channel under the lock.
    }

    spin_lock(&ch->lock);
    ch->reading = false;
    spin_unlock(&ch->lock);
    return ret;
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
