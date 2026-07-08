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

// chan_copy — kernel<->ring segment copy, 8 bytes at a time with byte
// tail. Every FS byte crosses these rings twice (produce + consume),
// and at the CF-3 B bulk frame size (128 KiB) a per-byte loop is real
// cost. The ring offsets drift to arbitrary alignment (9P frames have
// odd sizes), so the wide accesses use an aligned(1) may_alias type:
// well-defined C for unaligned access, and ARM64 kernel unaligned
// loads/stores are architecturally fine (SCTLR_EL1.A == 0, the CF-3 A
// bulk-uaccess precedent) — clang emits plain ldur/stur.
typedef u64 __attribute__((aligned(1), may_alias)) u64_unaligned;
static void chan_copy(u8 *dst, const u8 *src, u32 len) {
    u32 i = 0;
    for (; i + 8u <= len; i += 8u) {
        *(u64_unaligned *)(dst + i) = *(const u64_unaligned *)(src + i);
    }
    for (; i < len; i++) dst[i] = src[i];
}

static long chan_ring_write(struct srvconn_chan *ch, const u8 *buf, long n) {
    if (n <= 0) return 0;

    u32 avail = ch->cap - ch->count;
    u32 want  = ((long)avail < n) ? avail : (u32)n;
    if (want == 0) return 0;

    // Two-segment copy: from head to end-of-buf, then wrap.
    u32 first = ch->cap - ch->head;
    if (first > want) first = want;
    chan_copy(ch->buf + ch->head, buf, first);
    chan_copy(ch->buf, buf + first, want - first);

    ch->head   = (ch->head + want) % ch->cap;
    ch->count += want;
    return (long)want;
}

static long chan_ring_read(struct srvconn_chan *ch, u8 *buf, long n) {
    if (n <= 0) return 0;

    u32 want = ((long)ch->count < n) ? ch->count : (u32)n;
    if (want == 0) return 0;

    u32 first = ch->cap - ch->tail;
    if (first > want) first = want;
    chan_copy(buf, ch->buf + ch->tail, first);
    chan_copy(buf + first, ch->buf, want - first);

    ch->tail   = (ch->tail + want) % ch->cap;
    ch->count -= want;
    return (long)want;
}

// =============================================================================
// Channel helpers.
// =============================================================================

static void chan_init(struct srvconn_chan *ch, u8 *buf, u32 cap) {
    // kmalloc(KP_ZERO) already zeroed the channel — a zeroed spin_lock_t
    // is the unlocked form, a zeroed Rendez is {unlocked, no waiter}.
    // The explicit init is the documented contract (mirrors pipe.c).
    // `buf` is the heap ring storage (cap bytes), owned by the SrvConn.
    spin_lock_init(&ch->lock);
    ch->buf     = buf;
    ch->cap     = cap;
    ch->count   = 0;
    ch->head    = 0;
    ch->tail    = 0;
    ch->eof     = false;
    ch->reading = false;
    ch->writing = false;
    rendez_init(&ch->rendez);
    rendez_init(&ch->wrendez);
    poll_waiter_list_init(&ch->role_waiters);
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
    return (ch->count < ch->cap) || ch->eof;
}

// =============================================================================
// Blocking-role acquisition (#354, CF-3 B).
//
// `reading` / `writing` are the single blocking-consumer / -producer ROLES:
// the holder is the only thread that may park on the direction's rendez /
// wrendez (each stays single-waiter — the audited #348/#349 machinery is
// untouched). Pre-#354 a contender was refused -1, which a POSIX server's
// write_full treats as fatal (the #348-audit F1 latent — LIVE once stratumd
// went threaded at CF-2, guarded only by its own write_mu). Now a contender
// PARKS on the chan's role_waiters list — each on its OWN stack Rendez via a
// poll_waiter, the #349 send_waiters_list multi-waiter pattern — until the
// holder releases (or teardown / deadline / death unwinds it), then
// re-contends under ch->lock.
//
// The wait conds read the role flag + eof WITHOUT ch->lock: tsleep evaluates
// them under the waiter's own rendez lock, and the release path mutates the
// flag under ch->lock then wakes through the role list (whose per-waiter
// rendez-lock acquisition provides the happens-before) — the established
// chan_cond_* discipline. `|| eof` means "stop parking" (teardown): a woken
// contender acquires once the holder unwinds, then its op observes eof and
// fails/EOFs normally.
// =============================================================================

// The conds deliberately do NOT carry an `|| eof` term (audit F2): a
// contender woken by teardown while the unwinding holder still holds the
// role would find the cond instantly true again and busy-spin
// register->tsleep(AWOKEN)->re-check until the holder got scheduled.
// Liveness rests on the holder's GUARANTEED release instead: teardown
// wakes the holder (rendez/wrendez), every holder exit path releases,
// and the release wakes this list -- the contender then acquires and its
// op observes eof normally (residual drain / EOF / -1). The teardown-time
// role_waiters wake stays as defense in depth (a no-op sleep re-check).
static int role_cond_read_free(void *arg) {
    struct srvconn_chan *ch = (struct srvconn_chan *)arg;
    return !ch->reading;
}

static int role_cond_write_free(void *arg) {
    struct srvconn_chan *ch = (struct srvconn_chan *)arg;
    return !ch->writing;
}

// chan_role_acquire — claim the blocking read (writer=false) or write
// (writer=true) role, parking until it frees. `deadline_ns` bounds the
// park (0 = indefinite; the absolute timer_now_ns timebase, same contract
// as tsleep). Returns:
//   0                — role held; the caller MUST chan_role_release.
//   TSLEEP_TIMEDOUT  — the deadline passed while parked (role NOT held).
//   TSLEEP_INTR      — #811 death-interrupt while parked (role NOT held).
static int chan_role_acquire(struct srvconn_chan *ch, bool writer,
                             u64 deadline_ns) {
    for (;;) {
        spin_lock(&ch->lock);
        bool *role = writer ? &ch->writing : &ch->reading;
        if (!*role) {
            *role = true;
            spin_unlock(&ch->lock);
            return 0;
        }
        // Held — park on the role list. register-then-observe: the hook is
        // registered under ch->lock BEFORE the flag is re-sampled by tsleep
        // under the waiter's rendez lock, so a concurrent release's
        // clear-then-wake is either captured by the cond re-check or
        // delivered to the registered hook — no lost wake (I-9; poll.tla).
        // The stack Rendez/hook outlive the sleep (unregistered below
        // before this frame pops — poll.tla NoStaleHook).
        struct Rendez      pr;
        struct poll_waiter pw;
        rendez_init(&pr);
        poll_waiter_init(&pw, &pr);
        poll_waiter_list_register(&ch->role_waiters, &pw);
        spin_unlock(&ch->lock);

        int ts = tsleep(&pr,
                        writer ? role_cond_write_free : role_cond_read_free,
                        ch, deadline_ns);

        poll_waiter_list_unregister(&pw);
        if (ts == TSLEEP_TIMEDOUT || ts == TSLEEP_INTR) return ts;
        // AWOKEN — loop and re-contend (another contender may have won).
    }
}

// chan_role_release — drop the role + wake every parked contender. The
// wake runs OUTSIDE ch->lock (poll_waiter_list_wake takes the list + each
// waiter's rendez lock — the chan_produce mutate-then-wake discipline).
static void chan_role_release(struct srvconn_chan *ch, bool writer) {
    spin_lock(&ch->lock);
    if (writer) ch->writing = false;
    else        ch->reading = false;
    spin_unlock(&ch->lock);
    poll_waiter_list_wake(&ch->role_waiters);
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
        // CF-3 B: draining made room -> wake a producer parked on wrendez
        // (srvconn_client_send_blocking on c2s; the #348 drain-wake
        // discipline, outside ch->lock). A no-op when nothing is parked.
        wakeup(&ch->wrendez);
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
                               bool peer_console, u64 server_stripes,
                               u32 msize) {
    // Exactly two ring classes at v1.0 (CF-3 B): the default and the
    // DMSRVBULK bulk class. Rejecting everything else keeps the ring
    // memory a two-point policy, not an arbitrary-size demand.
    if (msize != SRVCONN_MSIZE && msize != SRVCONN_BULK_MSIZE) return NULL;

    struct SrvConn *cn = kmalloc(sizeof(*cn), KP_ZERO);
    if (!cn) return NULL;

    // Heap ring storage, 2x msize per direction (holds one whole msize
    // frame with a second in flight -- the #841 pipeline headroom the
    // inline SRVCONN_RING_CAP array provided at the default class).
    // KP_ZERO is hygiene, not correctness: chan_ring_read bounds every
    // copy-out by `count`, so uninitialized ring bytes never leave --
    // but conn creation is cold path and the zeroing is cheap insurance.
    u32 cap = 2u * msize;
    u8 *b_c2s = kmalloc(cap, KP_ZERO);
    u8 *b_s2c = b_c2s ? kmalloc(cap, KP_ZERO) : NULL;
    if (!b_c2s || !b_s2c) {
        kfree(b_c2s);
        kfree(cn);
        return NULL;
    }

    chan_init(&cn->c2s, b_c2s, cap);
    chan_init(&cn->s2c, b_s2c, cap);
    poll_waiter_list_init(&cn->poll_list);

    spin_lock_init(&cn->lock);
    cn->msize              = msize;
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
    // dereferencing freed memory. The ring buffers are the SrvConn's own
    // heap storage (CF-3 B): freed here, after teardown has unparked
    // every blocked party (no thread can be inside a ring copy at the
    // last unref — each blocking op holds a conn ref across its park).
    srvconn_teardown(cn);
    cn->magic = 0;
    kfree(cn->c2s.buf);
    kfree(cn->s2c.buf);
    cn->c2s.buf = NULL;
    cn->s2c.buf = NULL;
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

    // Wake every blocked consumer + every blocked producer + every parked
    // role contender + every registered poller. Wakes happen outside the
    // channel locks (the wakeup / poll-list paths take the rendez / list
    // locks; producer-mutates-then-wakes is the chan_produce discipline).
    // Both EOFs are visible to any wake observer. The wrendez wakes (#348
    // s2c; CF-3 B c2s — srvconn_client_send_blocking parks there too)
    // release a blocking send parked on a full ring; the role_waiters
    // wakes (#354) unpark contenders so they observe eof and unwind once
    // the unwinding holder releases.
    wakeup(&cn->c2s.rendez);
    wakeup(&cn->s2c.rendez);
    wakeup(&cn->c2s.wrendez);
    wakeup(&cn->s2c.wrendez);
    poll_waiter_list_wake(&cn->c2s.role_waiters);
    poll_waiter_list_wake(&cn->s2c.role_waiters);

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

u32 srvconn_msize(const struct SrvConn *cn) {
    // Fail-close to the DEFAULT class: a defensive caller must never
    // propose an msize past what the rings carry.
    if (!cn || cn->magic != SRV_CONN_MAGIC) return SRVCONN_MSIZE;
    return cn->msize;
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
    // A whole 9P frame is <= the negotiated msize <= the ring cap (2x msize
    // by construction, CF-3 B), so a frame can ALWAYS fit an empty ring; n
    // past the ring is a framing-layer bug, not back-pressure.
    if (n > (long)cn->c2s.cap) return -1;
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
    u32 freeb = ch->cap - ch->count;
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

    // RW-4 R2-F1 + #354: claim the single-reader role. ch->rendez is
    // single-waiter (only the role holder parks on it); a CONTENDING
    // blocking consumer now parks on the role list until release (#354 --
    // pre-fix it was refused -1). The role wait honors the same absolute
    // deadline as the data wait below (the WHOLE recv is deadline-bounded);
    // a death-interrupt unwinds it (#811). Released on every exit below.
    int ra = chan_role_acquire(ch, /*writer=*/false, cn->client_deadline_ns);
    if (ra != 0) {
        if (ra == TSLEEP_TIMEDOUT) cn->client_timed_out = true;
        return -1;
    }

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

    chan_role_release(ch, /*writer=*/false);
    return ret;
}

long srvconn_server_send(struct SrvConn *cn, const u8 *buf, long n) {
    if (!cn || cn->magic != SRV_CONN_MAGIC) return -1;
    if (!buf || n < 0) return -1;
    if (n == 0) return 0;
    // No poll wake here at v1.0. srvconn_poll's server-endpoint POLLOUT is
    // `!s2c.eof && s2c.count < s2c.cap`: increasing s2c.count can
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

    // #354 (CF-3 B): claim the single-writer role, PARKING on contention.
    // Pre-#354 a 2nd concurrent blocking writer on one s2c was refused -1
    // -- which a 9P-server Proc's write_full treats as fatal -> mount close
    // (the very #348 cascade). That was recorded latent-only while stratumd
    // was one-thread-per-conn; CF-2's threaded request processing made the
    // kernel's soundness rest on stratumd's own write_mu, a cross-project
    // pre-condition this role-park retires. The role is held across the
    // WHOLE multi-chunk delivery (frame-atomicity vs any other producer);
    // deadline 0 = indefinite (the kernel client is the guaranteed s2c
    // drainer); a death-interrupt unwinds (#811).
    int ra = chan_role_acquire(ch, /*writer=*/true, 0u);
    if (ra != 0) return -1;

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

    chan_role_release(ch, /*writer=*/true);
    return ret;
}

// srvconn_client_send_blocking — CF-3 B: the c2s twin of the #348
// blocking server send above (see srvconn.h for the contract + the
// third-producer EPIPE hazard it closes). Byte-mode-client path only:
// a kernel-attached conn's c2s is driven by the kernel 9P client's
// all-or-nothing frame send with #349 flow control, and devsrv_write's
// CSRVCLIENT arm refuses kernel-attached conns before routing here.
//
// Deadlock-freedom: the c2s drainer is the SERVER's read loop
// (srvconn_server_recv / _blocking, which wake c2s.wrendez on every
// drain). A server that never reads while the client never reads s2c is
// the classic full-duplex application deadlock POSIX AF_UNIX shares --
// teardown / #811 death still unwinds both parked parties.
long srvconn_client_send_blocking(struct SrvConn *cn, const u8 *buf, long n) {
    if (!cn || cn->magic != SRV_CONN_MAGIC) return -1;
    if (!buf || n < 0) return -1;
    if (n == 0) return 0;

    struct srvconn_chan *ch = &cn->c2s;

    // #354 role-park: one blocking producer at a time; the whole buffer
    // enters c2s contiguously (call atomicity vs peer-thread writers on a
    // shared pouch socket fd). Deadline 0 = indefinite; death unwinds.
    int ra = chan_role_acquire(ch, /*writer=*/true, 0u);
    if (ra != 0) return -1;

    long done = 0;
    long ret;
    for (;;) {
        long put = chan_produce(ch, buf + done, n - done);
        if (put < 0) {                        // eof latched (teardown)
            ret = (done > 0) ? done : -1;
            break;
        }
        done += put;
        // EVERY accepted chunk is a POLLIN edge for the server's pollers
        // (audit F1): a poll-then-read byte server parked in poll() must
        // see the ring fill NOW -- it is the drainer this send is about
        // to park waiting for. Deferring the wake to end-of-delivery is
        // a circular wait (the send can't finish until the server
        // drains; the server never wakes until the send finishes).
        // chan_produce woke the blocking-recv rendez; this wakes the
        // registered poll hooks (per-write, mirroring the non-blocking
        // srvconn_client_send).
        if (put > 0) poll_waiter_list_wake(&cn->poll_list);
        if (done >= n) { ret = done; break; } // whole buffer delivered

        // Ring full -> park until the server drains c2s (wakes wrendez)
        // or teardown latches eof. Same register-then-observe as the s2c
        // twin (specs/tsleep.tla).
        int ts = tsleep(&ch->wrendez, chan_cond_writable, ch, 0u);
        if (ts == TSLEEP_TIMEDOUT) {          // unreachable with deadline 0
            ret = (done > 0) ? done : -1;
            break;
        }
        if (ts == TSLEEP_INTR) {              // #811 death-interrupt
            ret = (done > 0) ? done : -1;
            break;
        }
        // TSLEEP_AWOKEN — loop, chan_produce again.
    }

    chan_role_release(ch, /*writer=*/true);
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

    // RW-4 R2-F1 + #354: claim the single-reader role, PARKING on
    // contention (this is the byte-mode POSIX-server read() path, the most
    // exposed concurrent-reader case -- peer server Threads may read one
    // accepted fd concurrently, POSIX-legally). Pre-#354 the 2nd reader
    // was refused (-1). Deadline 0 = indefinite; death unwinds (#811).
    int ra = chan_role_acquire(ch, /*writer=*/false, 0u);
    if (ra != 0) return -1;

    long ret;
    for (;;) {
        spin_lock(&ch->lock);
        if (ch->count > 0) {
            ret = chan_ring_read(ch, buf, n);
            spin_unlock(&ch->lock);
            // CF-3 B: draining made room -> wake a client producer parked
            // on c2s.wrendez (srvconn_client_send_blocking); the #348
            // drain-wake discipline, outside ch->lock.
            wakeup(&ch->wrendez);
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

    chan_role_release(ch, /*writer=*/false);
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
    if (!cn->s2c.eof && cn->s2c.count < cn->s2c.cap) {
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
