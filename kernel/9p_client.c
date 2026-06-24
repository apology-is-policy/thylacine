// 9P client high-level API — P5-client.
//
// Per `kernel/include/thylacine/9p_client.h`. Each public op composes:
//   session.send_*  →  transport.exchange  →  result extraction  →
//   error mapping.

#include <thylacine/9p_client.h>
#include <thylacine/9p_session.h>
#include <thylacine/9p_transport.h>
#include <thylacine/9p_wire.h>
#include <thylacine/notes.h>    // LS-5c: thread_die_pending (the widened #811 predicate)
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/spinlock.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../mm/slub.h"

_Static_assert(P9_CLIENT_MAGIC == 0x50394354u, "client magic drift");
_Static_assert(P9_CLIENT_OUT_BUF_MAX >= 256u,  "client out buf too small");

// R15-c F230: per-client lock acquired around every public op for the
// full build + transport-exchange + dispatch + bookkeeping window.
// Helpers keep the lock-release boilerplate out of every early-return
// path.
#define CLIENT_UNLOCK_RET(c, rc) do { spin_unlock(&(c)->lock); return (rc); } while (0)

// =============================================================================
// Internal: explicit struct copies. The kernel doesn't link libc, so
// struct assignments that the compiler would otherwise lower to memcpy
// fail at link time. Field-by-field copies sidestep the issue.
// =============================================================================

static void copy_qid(struct p9_qid *dst, const struct p9_qid *src) {
    dst->type    = src->type;
    dst->version = src->version;
    dst->path    = src->path;
}

static void copy_attr(struct p9_attr *dst, const struct p9_attr *src) {
    dst->valid       = src->valid;
    copy_qid(&dst->qid, &src->qid);
    dst->mode        = src->mode;
    dst->uid         = src->uid;
    dst->gid         = src->gid;
    dst->nlink       = src->nlink;
    dst->rdev        = src->rdev;
    dst->size        = src->size;
    dst->blksize     = src->blksize;
    dst->blocks      = src->blocks;
    dst->atime_sec   = src->atime_sec;
    dst->atime_nsec  = src->atime_nsec;
    dst->mtime_sec   = src->mtime_sec;
    dst->mtime_nsec  = src->mtime_nsec;
    dst->ctime_sec   = src->ctime_sec;
    dst->ctime_nsec  = src->ctime_nsec;
    dst->btime_sec   = src->btime_sec;
    dst->btime_nsec  = src->btime_nsec;
    dst->gen         = src->gen;
    dst->data_version = src->data_version;
}

static void copy_statfs(struct p9_statfs *dst, const struct p9_statfs *src) {
    dst->type    = src->type;
    dst->bsize   = src->bsize;
    dst->blocks  = src->blocks;
    dst->bfree   = src->bfree;
    dst->bavail  = src->bavail;
    dst->files   = src->files;
    dst->ffree   = src->ffree;
    dst->fsid    = src->fsid;
    dst->namelen = src->namelen;
}

// =============================================================================
// Internal: error mapping.
//
// `send_rc` and `recv_rc` carry layer-specific failure codes. The
// client surface maps them onto the errno convention documented in
// 9p_client.h.
// =============================================================================

static int map_error(int session_send_rc, int exchange_rc,
                      const struct p9_dispatch_result *r) {
    if (session_send_rc < 0) return -P9_E_IO;
    if (exchange_rc < 0) return -P9_E_IO;
    if (r->is_error) {
        // Rlerror surfaced an errno; map it to the client's signed-errno
        // convention. The wire ecode is an unvalidated u32 a (hostile or
        // buggy) server controls, so bound it before negating: 0 (an error
        // reply MUST carry a nonzero errno) and any value past the Linux
        // MAX_ERRNO collapse to -EIO. Without the bound, -(int)0x80000000 is
        // signed-overflow UB -- it traps under -fsanitize=undefined (a kernel
        // halt reachable by any Rlerror on any op), and an out-of-range ecode
        // would surface a nonsense errno. 4095 is the top of the pouch
        // boundary-line's [-4095,-2] errno passthrough window.
        if (r->ecode == 0 || r->ecode > 4095u) return -P9_E_IO;
        return -(int)r->ecode;
    }
    return 0;
}

// =============================================================================
// #841 elected-reader pipeline (ARCH §21.10).
//
// Restores committed ARCH §21 pipelining from the R15-c F230 serial regression
// (one spinlock held across the blocking recv + a per-op deadline that
// desynced the shared byte stream -- the stalk-3c soundness bug). Each
// steady-state op submits a p9_rpc + runs the Plan 9 `mountio` elected-reader
// loop: a submitter with no reply yet becomes THE reader (one at a time,
// c->lock DROPPED across the blocking recv), demuxes frames by tag to their
// owning rpc, wakes the owner, and hands the reader role off on departure. The
// handshake stays serial (the NOTAG branch of client_run): it runs single-
// threaded on a fresh, unshared client, so it has neither the busy-spin nor
// the shared-desync hazard, and it sidesteps Tversion's NOTAG (which cannot
// index inflight[]).
//
// Invariants: I-10 (tag uniqueness -- the session allocator), I-11 (fid
// lifecycle -- dispatch_rmsg unchanged), I-9 (no wakeup lost between an rpc's
// flag-check and its sleep -- register-then-observe under c->lock; the per-rpc
// flags are read by rpc_wait_cond under rpc->rendez.lock and every mutation is
// followed by wakeup(&rpc->rendez)), flow control (bounded outstanding).
// =============================================================================

#define CLIENT_WAIT_DONE  0     // rpc->done; reply in rpc->reply_buf
#define CLIENT_WAIT_DEAD  1     // session torn down; -P9_E_IO
#define CLIENT_WAIT_DIED  2     // caller's Proc group-terminating; unwind

// Is the calling Thread dying (#811, widened by LS-5c per ARCH 8.8.2)? A
// group-terminating Proc OR a pending terminate-disposition `interrupt` (a
// Ctrl-C'd coreutil blocked in a 9P RPC takes the same unwind). A blocking
// recv / sleep returning the death-interrupt is THIS thread dying -- the
// reader must UNWIND (hand off the reader role + abandon its rpc, the #845
// Tflush), NOT mark the shared session dead: the client is shared across
// Procs (corvus + joey on the Stratum root), so one Proc dying must not
// strand the survivors' in-flight ops (ARCH §21.10).
static bool client_self_dying(void) {
    return thread_die_pending(current_thread());
}

// rpc sleep predicate (evaluated under rpc->rendez.lock inside sleep()). Reads
// ONLY rpc-local flags; each is set under c->lock and followed by
// wakeup(&rpc->rendez), so no wakeup is lost between a flag write and the sleep
// transition (I-9; scheduler.tla NoMissedWakeup).
static int rpc_wait_cond(void *arg) {
    struct p9_rpc *rpc = (struct p9_rpc *)arg;
    return rpc->done || rpc->dead || rpc->be_reader;
}

// Byte copy (the kernel links no libc; matches the file's field-by-field
// idiom). dst (an rpc reply_buf) and src (the transport recv_buf) are distinct.
static void client_copy(u8 *dst, const u8 *src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
}

// Mark the whole session dead: latch c->dead, fail every in-flight rpc, wake
// its waiter. Transport EOF / recv error / send failure. c->lock HELD.
//
// `devgone` is the death REASON (MENAGERIE.md section 10): true when the death
// is a clean peer-gone EOF -- the server/driver endpoint vanished (recv 0 = the
// device/service gone) -- so an in-flight ASYNC (Loom) op completes with the
// device-gone -P9_E_NODEV terminal CQE, distinct from a generic transport -EIO.
// Only the async (POST_CQE) path carries the reason: the sync (WAKE_RENDEZ)
// front-end keeps its audited -P9_E_IO return (client_run's CLIENT_WAIT_DEAD),
// so this change does not touch the #841 synchronous surface or the boot path.
static void client_mark_dead_locked(struct p9_client *c, bool devgone) {
    int async_status = devgone ? -P9_E_NODEV : -P9_E_IO;
    c->dead = true;
    for (u32 tag = 0; tag < P9_SESSION_MAX_OUTSTANDING; tag++) {
        struct p9_rpc *r = c->inflight[tag];
        if (!r) continue;
        r->dead = true;
        if (r->on_complete) {
            // Async (POST_CQE, Loom): there is no submitter to wake. Clear the
            // slot + complete the op with an error CQE carrying the reason. The
            // callback runs under c->lock and MUST NOT sleep (seam contract).
            c->inflight[tag] = NULL;
            r->on_complete(r, async_status, NULL);
        } else {
            wakeup(&r->rendez);
        }
    }
    // #349 SA-1: also wake every sender parked on c2s back-pressure
    // (client_send_flow). They sleep on their OWN rendez, NOT their rpc->rendez,
    // so the per-rpc wake above does not reach them; their cond reads c->dead (now
    // true) -> each returns -EIO. A death from a path with no subsequent
    // reader-clear signal (e.g. the c2s send-break in client_run) would otherwise
    // leave them parked indefinitely. (mark_dead is the SOLE c->dead setter.)
    if (c->send_waiters) poll_waiter_list_wake(&c->send_waiters_list);
}

// Hand the reader role to one still-pending op so a survivor keeps reading
// after the current reader departs (LOAD-BEARING when the departing reader's
// Proc dies). Picks the first inflight rpc that is not the departing one and
// not yet done/dead/flagged, flags it be_reader + wakes it. If none, no reader
// is needed (nothing left awaiting a reply). c->lock HELD.
static void client_handoff_reader_locked(struct p9_client *c,
                                         struct p9_rpc *departing) {
    for (u32 tag = 0; tag < P9_SESSION_MAX_OUTSTANDING; tag++) {
        struct p9_rpc *r = c->inflight[tag];
        if (r && r != departing && !r->done && !r->dead && !r->be_reader &&
            !r->on_complete) {
            // Skip async (POST_CQE) ops: they have no submitter thread to run
            // the reader loop. An async op's reply is demuxed by the
            // SYS_LOOM_ENTER reap / SQPOLL kthread / p9_client_reader_pump_once
            // caller, never by becoming the elected reader (Loom §8.4).
            r->be_reader = true;
            wakeup(&r->rendez);
            return;
        }
    }
}

// #349 send flow control. Signal senders parked on c2s back-pressure that a
// reader made progress (a frame was demuxed -> the server drained a c2s slot,
// or the reader departed -> a sender may itself self-pump). Bump the progress
// generation the parked senders' conds compare against, and wake EVERY parked
// sender iff any is waiting (gating the list-walk off the hot demux path).
// c->lock HELD (lock order c->lock -> send_waiters_list.lock -> rendez.lock).
static void client_send_progress_signal(struct p9_client *c) {
    c->send_progress++;
    if (c->send_waiters) poll_waiter_list_wake(&c->send_waiters_list);
}

// Read ONE complete 9P frame into c->transport.recv_buf, mirroring
// 9p_transport.c::do_recv's framing (header -> peek size -> body) but calling
// ops.recv DIRECTLY: it must NOT latch transport.state=ERROR (a death-interrupt
// has to leave the transport reusable by the next reader). Returns:
//   > 0  the frame length;
//   0    a clean PEER-GONE EOF -- the transport recv returned 0: the server /
//        driver endpoint torn down (the device/service vanished). The caller
//        maps this to the device-gone death reason (MENAGERIE.md section 10);
//   -1   a transport error / idle deadline / malformed-or-oversize frame --
//        the generic transport death (the caller checks *idle for the idle
//        case and client_self_dying() for a death-interrupt unwind).
// The EOF-vs-error split is exactly the transport recv contract (0 = peer
// closed, < 0 = error) -- before, both collapsed to -1; preserving it is what
// lets a device-gone session post -ENODEV instead of a generic -EIO. c->lock
// NOT held (this blocks); the single-reader election guarantees only one thread
// is here at a time, so the shared recv_buf is safe.
//
// Loom-4 (LOOM.md §8.6): when `deadline_ns != 0`, the FIRST recv (the frame
// boundary, got==0) is deadline-bounded; a timeout THERE consumes no bytes, so
// the shared stream stays synced (#841) and *idle (if non-NULL) is set so the
// caller can distinguish the idle case from EOF/error (both return -1). The
// deadline is disarmed for the rest of the frame -- once any byte of the frame
// is in hand, a mid-frame timeout would desync the stream, so the body blocks
// unconditionally. `deadline_ns == 0` + `idle == NULL` is the original
// unbounded behavior. A backend with no set_recv_deadline op (NULL) ignores the
// deadline entirely (the recv just blocks).
static int reader_recv_frame(struct p9_client *c, u64 deadline_ns, bool *idle) {
    struct p9_transport *t = &c->transport;
    u8 *buf = t->recv_buf;
    size_t cap = t->recv_cap;
    size_t got = 0;
    if (idle) *idle = false;
    while (got < P9_HDR_LEN) {
        if (got == 0 && deadline_ns)
            p9_transport_set_recv_deadline(t, deadline_ns);
        int n = t->ops.recv(t->ops.ctx, buf + got, P9_HDR_LEN - got);
        if (got == 0 && deadline_ns) {
            // Read the timeout signal BEFORE disarming (disarm resets it),
            // then disarm so the rest of THIS frame blocks indefinitely.
            if (n <= 0 && idle && p9_transport_recv_timed_out(t)) *idle = true;
            p9_transport_set_recv_deadline(t, 0);
        }
        // A clean EOF (recv 0) is a peer-gone close -> the device/service is
        // gone; a recv error / armed-deadline timeout is < 0. The idle case is
        // < 0 + *idle set (the backends return -1 + timed_out, never 0, on a
        // deadline), so 0 is unambiguously the peer-gone EOF.
        if (n == 0) return 0;              // peer gone (device-gone reason)
        if (n < 0)  return -1;             // transport error / idle deadline
        if ((size_t)n > P9_HDR_LEN - got) return -1;
        got += (size_t)n;
    }
    u32 size; u8 type; u16 tag;
    if (p9_peek_header(buf, got, &size, &type, &tag) < 0) return -1;
    if (size < P9_HDR_LEN) return -1;
    if ((size_t)size > cap) return -1;
    while (got < (size_t)size) {
        int n = t->ops.recv(t->ops.ctx, buf + got, (size_t)size - got);
        if (n == 0) return 0;              // mid-frame EOF: peer vanished mid-reply (device-gone)
        if (n < 0)  return -1;             // transport error
        if ((size_t)n > (size_t)size - got) return -1;
        got += (size_t)n;
    }
    return (int)got;
}

// Demux one received frame (in c->transport.recv_buf, `len` bytes) to its owner.
// c->lock HELD. An OWNED frame is copied into the owner's reply_buf + the owner
// is woken (the owner dispatches it -- extraction stays in the submitter). An
// OWNERLESS frame is dispatched-and-discarded HERE. For an abandoned op (the
// owning Proc died + unwound, #845) this is either the late original reply --
// p9_session_dispatch_rmsg consumes it WITHOUT freeing the tag, which stays
// reserved (awaiting_flush) until its Rflush -- or the Rflush itself, which
// frees both the flush tag and the abandoned oldtag. dispatch_rmsg routes both
// cases. A malformed header, out-of-range tag, or oversize frame is a protocol
// violation -> mark dead.
static void demux_frame_locked(struct p9_client *c, size_t len) {
    u32 size; u8 type; u16 tag;
    if (p9_peek_header(c->transport.recv_buf, len, &size, &type, &tag) < 0) {
        client_mark_dead_locked(c, false);
        return;
    }
    if (tag >= P9_SESSION_MAX_OUTSTANDING) {
        // Steady-state replies carry tags 0..MAX-1; NOTAG (Tversion) is only on
        // the serial handshake path, never demuxed here.
        client_mark_dead_locked(c, false);
        return;
    }
    struct p9_rpc *owner = c->inflight[tag];
    if (owner) {
        if (owner->on_complete) {
            // Async (POST_CQE, Loom): there is no submitter to dispatch +
            // extract the reply, so the engine does it HERE, under c->lock,
            // and hands the mapped result to on_complete. dispatch_rmsg clears
            // session.outstanding[tag] + applies fid state -- exactly what
            // client_run does for a sync op after CLIENT_WAIT_DONE. `dr` aliases
            // the recv buffer and is valid only for the callback's duration. The
            // callback runs under c->lock and MUST NOT sleep (seam contract).
            c->inflight[tag] = NULL;
            struct p9_dispatch_result dr;
            int drc = p9_session_dispatch_rmsg(&c->session, c->transport.recv_buf,
                                               len, &dr);
            int result = map_error(0, drc, &dr);
            owner->done = true;                       // set before the callback;
            owner->on_complete(owner, result, &dr);   // may free owner -> last use
            // Same fail-closed posture as the sync DONE path: a negative drc is
            // an unrecoverable protocol violation that left outstanding[tag]
            // uncleared. This owner already got its error CQE (result < 0); latch
            // the session dead so the leaked slot cannot wedge the pool and every
            // remaining in-flight op fails closed. owner may be freed by the
            // callback above; mark_dead touches only the OTHER inflight[] slots
            // (this tag was NULLed before dispatch).
            if (drc < 0) client_mark_dead_locked(c, false);
            return;
        }
        if (len > c->recv_cap) { client_mark_dead_locked(c, false); return; }  // defensive
        client_copy(owner->reply_buf, c->transport.recv_buf, len);
        owner->reply_len = (int)len;
        owner->done = true;
        wakeup(&owner->rendez);
    } else {
        struct p9_dispatch_result discard;
        (void)p9_session_dispatch_rmsg(&c->session, c->transport.recv_buf,
                                       len, &discard);
    }
}

// Block until rpc->done, the session dies, or my Proc dies (the elected-reader
// loop). c->lock HELD on entry + exit.
static int client_wait(struct p9_client *c, struct p9_rpc *rpc) {
    for (;;) {
        if (rpc->done)            return CLIENT_WAIT_DONE;
        if (rpc->dead)            return CLIENT_WAIT_DEAD;
        if (client_self_dying()) {
            // F6 (round-2): if I was the designated-next reader (a departing
            // reader handed me the role via be_reader + a wake) but my Proc is
            // dying before I reach the election below, the role would be LOST --
            // a surviving Proc's pending op would have no reader to demux/wake
            // it and no future handoff would fire (the dying-reader handoff at
            // line ~282 only runs for a thread that ACTUALLY assumed the role).
            // Re-hand-off before unwinding. Gated on be_reader so the
            // active-reader-dies case (already handed off; be_reader cleared
            // when it assumed the role) and the plain-sleeper case (an active
            // reader still exists) do NOT spuriously double-hand-off.
            if (rpc->be_reader) {
                rpc->be_reader = false;
                client_handoff_reader_locked(c, rpc);
            }
            return CLIENT_WAIT_DIED;
        }
        if (!c->reader_active) {
            // Become THE reader: read frames (c->lock dropped) until MY reply
            // lands, the session dies, or I'm dying.
            c->reader_active = true;
            rpc->be_reader   = false;
            for (;;) {
                if (rpc->done || rpc->dead) break;
                if (client_self_dying())    break;
                spin_unlock(&c->lock);
                int rr = reader_recv_frame(c, 0, NULL);
                spin_lock(&c->lock);
                if (rr > 0) {
                    demux_frame_locked(c, (size_t)rr);
                    client_send_progress_signal(c);     // #349: a c2s slot freed
                } else if (client_self_dying()) {
                    break;                              // death-interrupt: unwind
                } else {
                    // rr == 0 (clean EOF = peer/server endpoint gone) -> device-
                    // gone; rr < 0 (recv error / malformed) -> transport.
                    client_mark_dead_locked(c, rr == 0);
                }
            }
            c->reader_active = false;
            client_send_progress_signal(c);             // #349: I departed -- a
                                                        // parked sender may self-pump
            client_handoff_reader_locked(c, rpc);
            // re-loop: done / dead / dying now decides the return.
        } else {
            // Another thread is the reader. Sleep on MY rpc (c->lock dropped)
            // until my reply lands, the session dies, or I'm handed the reader
            // role. A death-interrupt returns SLEEP_INTR -> the loop-top
            // client_self_dying() returns CLIENT_WAIT_DIED.
            //
            // F7 (round-2): clear a stale be_reader FIRST. If I was handed the
            // role (be_reader set + woken) but lost the election race to a
            // thread that grabbed reader_active before I re-checked, reaching
            // here with be_reader still set makes rpc_wait_cond TRUE -> sleep
            // returns immediately -> I busy-spin (re-lock, re-check, re-sleep)
            // until that reader departs, burning a CPU + contending c->lock
            // (the exact pathology the elected reader removes). Dropping it lets
            // me sleep; the active reader's departure handoff re-wakes me. No
            // lost wakeup: a handoff re-setting be_reader + waking me after this
            // clear is observed by sleep's register-then-observe cond-check.
            rpc->be_reader = false;
            spin_unlock(&c->lock);
            (void)sleep(&rpc->rendez, rpc_wait_cond, rpc);
            spin_lock(&c->lock);
        }
    }
}

// #349 send-flow-control park condition. The parked sender retries its send on
// ANY reader progress (send_progress bumped past the snapshot) or session death.
struct send_wait_ctx { struct p9_client *c; u64 gen; };
static int send_wait_cond(void *arg) {
    struct send_wait_ctx *w = (struct send_wait_ctx *)arg;
    return w->c->dead || w->c->send_progress != w->gen;
}

// Send the framed Tmsg in c->out_buf with #349 flow control. c->lock HELD on
// entry + exit. A transiently-FULL c2s ring (P9_TRANSPORT_EAGAIN -- back-pressure
// under #841 pipelining + concurrent large frames) is NOT a session death: the
// sender makes progress on the reply path so the server drains c2s, then retries.
// Returns 0 = the whole frame is on the wire; -P9_E_IO = a genuine transport
// break or self-death (the caller cleans up + decides whether to latch dead).
//
// Deadlock-freedom: a back-pressured sender MUST drop c->lock (else it blocks the
// active reader's demux -> the server can't drain s2c -> can't drain c2s -> wedge).
// If no reader is active it self-pumps one s2c frame (the reader-election body);
// else it parks until a reader makes progress (register-then-observe on
// send_progress -- the rpc->done pattern). A live server always produces replies
// for the queued requests, so the reply path drains + c2s frees; a dead server
// surfaces as a reader EOF/error -> c->dead -> the park/loop exits -EIO. The op's
// own tag is NOT on the wire (this send is failing), so a self-pump only demuxes
// OTHER ops' replies -- never its own (no reentrancy on rpc).
static int client_send_flow(struct p9_client *c, size_t built_len,
                            struct p9_rpc *rpc) {
    for (;;) {
        if (client_self_dying()) return -P9_E_IO;
        if (c->dead)             return -P9_E_IO;

        int src = p9_transport_send(&c->transport, c->out_buf, built_len);
        if (src == 0)                   return 0;          // whole frame sent
        if (src != P9_TRANSPORT_EAGAIN) return -P9_E_IO;   // genuine break

        if (!c->reader_active) {
            // No reader: pump one s2c frame myself (drain -> free the server to
            // drain c2s), then retry. Mirrors client_wait's elected-reader body.
            // R2-F3: a recv error/EOF here latches the SHARED session dead via
            // client_mark_dead_locked, identically to the elected reader -- a real
            // peer-gone/transport break is a death for everyone; the fail-close
            // semantics are unchanged from #841 (just now reachable from the send
            // path too, not only the read path).
            c->reader_active = true;
            rpc->be_reader   = false;
            spin_unlock(&c->lock);
            int rr = reader_recv_frame(c, 0, NULL);
            spin_lock(&c->lock);
            if (rr > 0)                    demux_frame_locked(c, (size_t)rr);
            else if (!client_self_dying()) client_mark_dead_locked(c, rr == 0);
            c->reader_active = false;
            client_send_progress_signal(c);          // a c2s slot may have freed
            client_handoff_reader_locked(c, rpc);
        } else {
            // Another thread is the reader (draining s2c). Park until it makes
            // progress (bumps send_progress) or the session dies, then retry.
            // MULTI-WAITER (R2-F1): N senders can be back-pressured on the shared
            // client at once, so each parks on its OWN stack Rendez via a
            // poll_waiter on c->send_waiters_list -- a single Rendez extincts on the
            // 2nd sleeper (rendez.h). register-then-observe: register the hook +
            // snapshot send_progress under c->lock, so a concurrent reader's
            // bump-then-wake is either captured by the snapshot (cond true at
            // sleep's re-check) or delivered to the now-registered hook -- no lost
            // wake (I-9; the poll.tla register-then-observe). The stack Rendez/hook
            // outlive the sleep (unregistered under c->lock before this frame pops).
            struct Rendez      pr;
            struct poll_waiter pw;
            rendez_init(&pr);
            poll_waiter_init(&pw, &pr);
            struct send_wait_ctx w = { c, c->send_progress };
            poll_waiter_list_register(&c->send_waiters_list, &pw);
            c->send_waiters++;
            spin_unlock(&c->lock);
            (void)sleep(&pr, send_wait_cond, &w);
            spin_lock(&c->lock);
            c->send_waiters--;
            poll_waiter_list_unregister(&pw);
        }
    }
}

// Submit the Tmsg already built into c->out_buf (built_len bytes; the caller's
// session_send_* allocated the tag + set session.outstanding[tag]), run the
// elected-reader wait, dispatch the reply, surface the result in *out. c->lock
// HELD on entry + exit. Returns 0 / -P9_E_IO / -<server errno>. On a death-
// interrupt the Thread is unwinding to its EL0-return die-check; the return is
// immaterial (it never re-enters EL0).
static int client_run(struct p9_client *c, size_t built_len,
                      struct p9_dispatch_result *out) {
    u32 size; u8 type; u16 tag;
    if (p9_peek_header(c->out_buf, built_len, &size, &type, &tag) < 0)
        return -P9_E_IO;

    if (tag == P9_NOTAG) {
        // Tversion (the only NOTAG message; serial handshake path -- single-
        // threaded on a fresh client, so send + recv-one + dispatch is safe).
        int rc = p9_transport_exchange(&c->transport, &c->session,
                                       c->out_buf, built_len, out);
        return map_error(0, rc, out);
    }
    if (tag >= P9_SESSION_MAX_OUTSTANDING)
        return -P9_E_IO;                 // the session never allocates such a tag

    struct p9_rpc rpc;
    rpc.tag         = (u16)tag;
    rpc.done        = false;
    rpc.dead        = false;
    rpc.be_reader   = false;
    rpc.reply_len   = 0;
    rpc.on_complete = NULL;          // sync (WAKE_RENDEZ): the submitter waits
    rendez_init(&rpc.rendez);
    rpc.reply_buf = kmalloc(c->recv_cap, KP_ZERO);
    if (!rpc.reply_buf)
        return -P9_E_IO;

    c->inflight[tag] = &rpc;

    // Send the framed Tmsg with #349 flow control. c->lock HELD: holding it
    // serializes senders so two frames never interleave; a transiently-full c2s
    // ring (back-pressure under #841 pipelining) is drained + retried inside
    // client_send_flow, NOT a death (the pre-#349 path marked the whole session
    // dead here, killing every other op including in-flight text page-ins).
    int sfr = client_send_flow(c, built_len, &rpc);
    if (sfr < 0) {
        c->inflight[tag] = NULL;
        kfree(rpc.reply_buf);
        // A genuine transport break latches the session dead (every op then fails
        // closed). A self-death does NOT: the request never reached the wire (the
        // send kept back-pressuring), so there is nothing in flight to flush and
        // the session stays live for peers -- mark dead only if the ring broke.
        if (!c->dead && !client_self_dying())
            client_mark_dead_locked(c, false);
        return -P9_E_IO;
    }

    int wr = client_wait(c, &rpc);
    if (wr == CLIENT_WAIT_DIED) {
        // My Proc is dying. Abandon the op (#845): drop the inflight
        // registration (the reader must not touch reply_buf once freed) + free
        // reply_buf, then send a Tflush(oldtag=tag) so the server promptly
        // releases the request and the tag is reclaimed when the Rflush arrives
        // -- drained ownerless by a survivor's reader via demux_frame_locked.
        // send_flush leaves session.outstanding[tag] ACTIVE + awaiting_flush:
        // per 9P the tag is reusable only after the Rflush, so a stray late
        // original reply can never be mis-attributed to a reused tag (I-10).
        // The Tflush send is a non-blocking ring write under c->lock (reusing
        // out_buf, whose prior frame was already pushed to the ring). Reaching
        // DIED means rpc->dead was false (DEAD is checked first in client_wait),
        // so c->dead is false and the session is OPEN here. If the flush cannot
        // be built/sent (pool full / send fail), fall back to the pre-#845
        // reclaim path -- outstanding[tag] stays active, reclaimed by the
        // eventual late reply or session teardown (no regression); a failed
        // transport write means the byte stream is broken, so latch dead.
        c->inflight[tag] = NULL;
        kfree(rpc.reply_buf);
        int flen = p9_session_send_flush(&c->session, c->out_buf,
                                         sizeof(c->out_buf), (u16)tag);
        if (flen > 0) {
            if (p9_transport_send(&c->transport, c->out_buf, (size_t)flen) < 0)
                client_mark_dead_locked(c, false);
        }
        return -P9_E_IO;
    }
    c->inflight[tag] = NULL;
    if (wr == CLIENT_WAIT_DEAD) {
        kfree(rpc.reply_buf);
        return -P9_E_IO;
    }

    // CLIENT_WAIT_DONE: dispatch my own reply buffer (clears
    // session.outstanding[tag], applies fid state) under c->lock.
    int drc = p9_session_dispatch_rmsg(&c->session, rpc.reply_buf,
                                       (size_t)rpc.reply_len, out);
    // A negative drc is an UNRECOVERABLE protocol violation: a reply that is
    // well-framed (it reached demux) but cannot be parsed for its own
    // outstanding op -- a wrong R-type on the tag, a body that fails the
    // strict-length parse, or a tag-echo mismatch. dispatch_rmsg returns before
    // clear_outstanding on every such path, so the session.outstanding[tag] slot
    // is otherwise LEAKED (a benign Rlerror returns 0, not <0, so this never
    // fires on a normal server error). Fail closed -- identical in class to the
    // demux-level malformations client_mark_dead_locked already latches on
    // (a corrupted in-kernel ring / a buggy server). Without this the leaked
    // slot exhausts the 64-tag pool after <=64 such replies (a session wedge),
    // and a later well-formed reply on the still-active tag dispatches
    // ownerlessly + can spuriously mutate fid state.
    if (drc < 0) client_mark_dead_locked(c, false);
    // Do NOT free rpc.reply_buf here: dispatch set the read/readdir/readlink
    // zero-copy aliases (out->read_data etc.) pointing INTO it, and the public
    // op copies those out AFTER this returns. Keep it as the client's
    // most-recent reply buffer; free the PRIOR one (its caller has long since
    // copied out + dropped c->lock). Freeing here was a use-after-free on the
    // read-family ops (the old serial path aliased the long-lived transport
    // recv_buf, never the per-op buffer).
    if (c->done_reply_buf) kfree(c->done_reply_buf);
    c->done_reply_buf = rpc.reply_buf;
    return map_error(0, drc, out);
}

// =============================================================================
// Asynchronous (Loom) front-end -- the pluggable-completion seam (Loom-2b).
//
// submit_async sends an op + registers it WITHOUT blocking; the reply is
// demuxed later by reader_pump_once / the SYS_LOOM_ENTER reap / the SQPOLL
// kthread, which invokes rpc->on_complete (the POST_CQE front-end). The
// elected-reader / demux machinery is unchanged; only the completion ACTION is
// pluggable (one engine, two front-ends -- LOOM.md §8.4).
// =============================================================================

int p9_client_submit_async(struct p9_client *c, struct p9_rpc *rpc,
                           p9_session_build_fn build, void *build_ctx) {
    // Reject WITHOUT firing on_complete: nothing was taken over yet, and an
    // absent on_complete is precisely what we'd need to complete the op.
    if (!c || c->magic != P9_CLIENT_MAGIC)     return -P9_E_INVAL;
    if (!rpc || !rpc->on_complete || !build)   return -P9_E_INVAL;

    spin_lock(&c->lock);
    if (c->dead || !p9_session_is_open(&c->session)) {
        spin_unlock(&c->lock);
        rpc->on_complete(rpc, -P9_E_IO, NULL);   // own it: complete + bail
        return -P9_E_IO;
    }
    // Build the Tmsg into the shared out_buf under the lock (allocating the tag
    // + marking session.outstanding[tag]); a >0 return is a well-formed frame.
    int built = build(&c->session, c->out_buf, sizeof(c->out_buf), build_ctx);
    if (built <= 0) {
        spin_unlock(&c->lock);
        rpc->on_complete(rpc, -P9_E_IO, NULL);
        return -P9_E_IO;
    }
    u32 size; u8 type; u16 tag;
    if (p9_peek_header(c->out_buf, (size_t)built, &size, &type, &tag) < 0 ||
        tag == P9_NOTAG || tag >= P9_SESSION_MAX_OUTSTANDING) {
        // Unreachable for a conforming builder (a session_send_* writes a valid
        // tagged frame). If it ever fires, `build` already marked an outstanding
        // tag we cannot track (the header is unparseable / NOTAG / out of range),
        // so latch the session dead -- fail closed; the orphaned outstanding tag
        // is then moot (the session is unusable) -- and complete this op. `rpc`
        // is not yet registered, so mark_dead does not double-fire it.
        client_mark_dead_locked(c, false);
        spin_unlock(&c->lock);
        rpc->on_complete(rpc, -P9_E_IO, NULL);
        return -P9_E_IO;
    }
    rpc->tag       = (u16)tag;
    rpc->done      = false;
    rpc->dead      = false;
    rpc->be_reader = false;
    rpc->reply_len = 0;
    rpc->reply_buf = NULL;          // async: demux dispatches from recv_buf
    rendez_init(&rpc->rendez);      // unused for async, but kept inert
    c->inflight[tag] = rpc;

    int src = p9_transport_send(&c->transport, c->out_buf, (size_t)built);
    if (src < 0) {
        // The byte stream is broken: latch dead + complete every in-flight op.
        // `rpc` is registered, so mark_dead fires its on_complete (error CQE).
        client_mark_dead_locked(c, false);
        spin_unlock(&c->lock);
        return -P9_E_IO;
    }
    c->total_ops++;
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_reader_pump_once(struct p9_client *c) {
    if (!c || c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead)          { spin_unlock(&c->lock); return -P9_E_IO; }
    if (c->reader_active) { spin_unlock(&c->lock); return 0; }   // another reader
    c->reader_active = true;
    spin_unlock(&c->lock);

    int rr = reader_recv_frame(c, 0, NULL); // blocks; c->lock dropped (single reader)

    spin_lock(&c->lock);
    int ret;
    if (rr > 0) {
        demux_frame_locked(c, (size_t)rr);
        ret = 1;
    } else if (client_self_dying()) {
        // Death-interrupt: the caller's Proc is dying. Do NOT mark the shared
        // session dead (it serves survivors). Unwind after handing the role on.
        ret = -P9_E_IO;
    } else {
        // rr == 0 (clean EOF = peer/server endpoint gone) -> device-gone
        // (-P9_E_NODEV CQEs); rr < 0 (recv error) -> transport (-P9_E_IO).
        client_mark_dead_locked(c, rr == 0);
        ret = -P9_E_IO;
    }
    c->reader_active = false;
    client_handoff_reader_locked(c, NULL);
    spin_unlock(&c->lock);
    return ret;
}

bool p9_client_recv_is_deadline_capable(struct p9_client *c) {
    if (!c || c->magic != P9_CLIENT_MAGIC) return false;
    // The transport ops are immutable post-init, so this is a stable property of
    // the client (no lock needed). NULL => the recv blocks unbounded at a frame
    // boundary (the spoor pipe-pair transport, SYS_ATTACH_9P); non-NULL => an armed
    // deadline lets a boundary recv return (srvconn -> client_deadline_ns; the
    // loopback test backend also models the deadline).
    return c->transport.ops.set_recv_deadline != NULL;
}

int p9_client_reader_pump_once_deadline(struct p9_client *c, u64 deadline_ns) {
    if (!c || c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead)          { spin_unlock(&c->lock); return P9_PUMP_DEAD; }
    if (c->reader_active) { spin_unlock(&c->lock); return P9_PUMP_BUSY; }
    c->reader_active = true;
    spin_unlock(&c->lock);

    bool idle = false;
    int rr = reader_recv_frame(c, deadline_ns, &idle); // blocks; c->lock dropped

    spin_lock(&c->lock);
    int ret;
    if (rr > 0) {
        demux_frame_locked(c, (size_t)rr);
        ret = P9_PUMP_PROGRESS;
    } else if (idle) {
        // The idle deadline lapsed at the frame boundary: no bytes consumed, the
        // byte stream stays synced. NOT an error -- leave the session alive; the
        // SQPOLL kthread parks + retries (LOOM.md §8.6).
        ret = P9_PUMP_IDLE;
    } else if (client_self_dying()) {
        // Death-interrupt: the caller's Proc is dying. Do NOT mark the shared
        // session dead (it serves survivors). Unwind after handing the role on.
        ret = P9_PUMP_DEAD;
    } else {
        // rr == 0 (clean EOF = peer/server endpoint gone) -> device-gone
        // (-P9_E_NODEV CQEs); rr < 0 (recv error) -> transport (-P9_E_IO).
        client_mark_dead_locked(c, rr == 0);
        ret = P9_PUMP_DEAD;
    }
    c->reader_active = false;
    client_handoff_reader_locked(c, NULL);
    spin_unlock(&c->lock);
    return ret;
}

void p9_client_handoff_reader(struct p9_client *c) {
    if (!c || c->magic != P9_CLIENT_MAGIC) return;
    spin_lock(&c->lock);
    client_handoff_reader_locked(c, NULL);
    spin_unlock(&c->lock);
}

void p9_client_abandon_async(struct p9_client *c, struct p9_rpc *rpc) {
    // Quiesce ONE in-flight async op (Loom teardown / #898). This is the async
    // analog of client_run's CLIENT_WAIT_DIED path (#845): the Loom is being
    // destroyed with `rpc` possibly still in flight, so the engine must drop the
    // registration -- after which NO future demux / mark_dead can fire
    // rpc->on_complete -- and Tflush the op so the server releases the request
    // and the tag is reclaimed by the Rflush (a late original reply is then
    // discarded ownerless by demux_frame_locked, never mis-attributed to a
    // reused tag: 9P forbids reusing oldtag until the Rflush -- the I-10
    // reuse-race guard).
    //
    // Crucially this runs UNDER c->lock, so it is mutually exclusive with the
    // demux / mark_dead that also touch c->inflight[tag]. The check
    // `c->inflight[tag] == rpc` is the authoritative state:
    //   - STILL ours: the reply has not been demuxed; we clear + Tflush, and the
    //     caller (loom_free) then tears down the container with NO on_complete
    //     having fired (the op is "abandoned", no CQE -- the spec's Teardown).
    //   - NOT ours (NULL or a reused tag): the reply was already demuxed (or the
    //     session died), so on_complete ALREADY fired (terminal); this is a
    //     no-op and the caller just reclaims the container.
    // Either way, after this returns rpc is unreachable from inflight[] -> the
    // caller owns the container's teardown with no concurrent completer.
    if (!c || c->magic != P9_CLIENT_MAGIC || !rpc) return;
    spin_lock(&c->lock);
    u16 tag = rpc->tag;
    if (tag < P9_SESSION_MAX_OUTSTANDING && c->inflight[tag] == rpc) {
        c->inflight[tag] = NULL;
        // Reaching here, the reply never arrived (inflight still ours), so the
        // op never completed: c->dead would have fired mark_dead (clearing the
        // slot). Send the Tflush only on a live, open session. send_flush leaves
        // session.outstanding[tag] reserved (awaiting_flush). A failed
        // build/send means the byte stream is broken -> latch dead (no
        // regression vs the pre-#845 reclaim; the tag is then moot). The send is
        // a non-blocking ring write reusing out_buf under c->lock.
        if (!c->dead && p9_session_is_open(&c->session)) {
            int flen = p9_session_send_flush(&c->session, c->out_buf,
                                             sizeof(c->out_buf), tag);
            if (flen > 0) {
                if (p9_transport_send(&c->transport, c->out_buf, (size_t)flen) < 0)
                    client_mark_dead_locked(c, false);
            }
        }
    }
    spin_unlock(&c->lock);
}

// Mark the session DEVICE-GONE (MENAGERIE.md section 10): the explicit entry
// point for a holder of the client (a device-teardown / warden-removal hook)
// to proactively fail every in-flight ASYNC (Loom) op with a -P9_E_NODEV
// terminal CQE. Idempotent: on an already-dead session the in-flight slots are
// cleared, so the mark_dead loop is a no-op (the FIRST death's reason stands).
// The AUTOMATIC path -- a peer-gone EOF the reader classifies device-gone --
// needs no caller, so a driver group-terminated by a DeviceRemoved already
// yields device-gone CQEs without this.
void p9_client_mark_devgone(struct p9_client *c) {
    if (!c || c->magic != P9_CLIENT_MAGIC) return;
    spin_lock(&c->lock);
    client_mark_dead_locked(c, true);
    spin_unlock(&c->lock);
}

// =============================================================================
// Lifecycle.
// =============================================================================

int p9_client_init(struct p9_client *c,
                    u32 root_fid, u32 msize,
                    struct p9_transport_ops transport_ops,
                    u8 *recv_buf, size_t recv_cap) {
    if (!c) return -P9_E_INVAL;
    if (!recv_buf) return -P9_E_INVAL;
    if (recv_cap < P9_HDR_LEN) return -P9_E_INVAL;
    int rc = p9_session_init(&c->session, root_fid, msize);
    if (rc < 0) return -P9_E_INVAL;
    rc = p9_transport_init(&c->transport, transport_ops, recv_buf, recv_cap);
    if (rc < 0) {
        p9_session_destroy(&c->session);
        return -P9_E_INVAL;
    }
    c->magic        = P9_CLIENT_MAGIC;
    spin_lock_init(&c->lock);
    // #841 elected-reader pipeline state. recv_cap sizes each per-rpc reply
    // buffer (a frame is <= the transport's recv_cap). inflight[] starts empty;
    // the session is live (dead latches only on transport EOF/error).
    c->recv_cap       = recv_cap;
    c->reader_active  = false;
    c->dead           = false;
    c->done_reply_buf = NULL;
    poll_waiter_list_init(&c->send_waiters_list);   // #349 send-flow-control park (multi-waiter)
    c->send_progress  = 0;
    c->send_waiters   = 0;
    for (u32 i = 0; i < P9_SESSION_MAX_OUTSTANDING; i++) c->inflight[i] = NULL;
    // Fid allocator starts at root_fid + 1; dev9p (and other callers)
    // pull fresh fids monotonically via p9_client_alloc_fid.
    c->next_fid     = (root_fid < P9_NOFID - 1) ? (root_fid + 1) : 1;
    c->total_ops    = 0;
    c->total_errors = 0;
    return 0;
}

void p9_client_destroy(struct p9_client *c) {
    if (!c) return;
    if (c->magic != P9_CLIENT_MAGIC) return;
    c->magic = 0;
    // F8 (round-2): free the deferred reply buffer under c->lock so the impl
    // matches the documented "freed at the next completion or at destroy, both
    // under c->lock" invariant. Destroy runs when the last attached ref drops
    // (no op is in flight), so the lock is uncontended -- this is defense in
    // depth against a future caller that destroys a still-shared client.
    spin_lock(&c->lock);
    if (c->done_reply_buf) { kfree(c->done_reply_buf); c->done_reply_buf = NULL; }
    spin_unlock(&c->lock);
    p9_transport_destroy(&c->transport);
    p9_session_destroy(&c->session);
}

int p9_client_close(struct p9_client *c) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    int rc = p9_transport_close(&c->transport);
    int rc2 = p9_session_close(&c->session);
    spin_unlock(&c->lock);
    if (rc < 0) return -P9_E_IO;
    if (rc2 < 0) return -P9_E_IO;
    return 0;
}

// =============================================================================
// Handshake.
// =============================================================================

int p9_client_handshake(struct p9_client *c,
                         const u8 *uname, size_t uname_len,
                         const u8 *aname, size_t aname_len,
                         u32 n_uname) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);

    // Phase 1: Tversion → Rversion (drives INIT → VERSIONED). Tversion is the
    // only NOTAG message; client_run's NOTAG branch keeps it serial.
    int len = p9_session_send_version(&c->session, c->out_buf,
                                       sizeof(c->out_buf), NULL, 0);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }

    // Phase 2: Tattach → Rattach (drives VERSIONED → OPEN). A real tag -> the
    // elected-reader path; the fresh client is unshared, so the single thread
    // simply becomes the reader for its own Rattach (no contention).
    len = p9_session_send_attach(&c->session, c->out_buf,
                                  sizeof(c->out_buf),
                                  uname, uname_len, aname, aname_len,
                                  n_uname);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    spin_unlock(&c->lock);
    return 0;
}

// =============================================================================
// Path operations.
// =============================================================================

int p9_client_walk(struct p9_client *c,
                    u32 src_fid, u32 new_fid,
                    u16 nwname,
                    const u8 *const *names, const size_t *name_lens,
                    u16 *out_nwqid, struct p9_qid *out_qids) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_walk(&c->session, c->out_buf,
                                    sizeof(c->out_buf),
                                    src_fid, new_fid,
                                    nwname, names, name_lens);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (out_nwqid) *out_nwqid = r.nwqid;
    if (out_qids) {
        for (u16 i = 0; i < r.nwqid && i < P9_MAX_WALK; i++) {
            copy_qid(&out_qids[i], &r.qids[i]);
        }
    }
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_walk_one(struct p9_client *c,
                        u32 src_fid, u32 new_fid,
                        const u8 *name, size_t name_len,
                        struct p9_qid *out_qid) {
    const u8 *names[1] = { name };
    const size_t name_lens[1] = { name_len };
    u16 nwqid;
    struct p9_qid qids[P9_MAX_WALK];
    int e = p9_client_walk(c, src_fid, new_fid, 1, names, name_lens, &nwqid, qids);
    if (e != 0) return e;
    if (nwqid != 1) return -P9_E_IO;       // partial walk; we asked for 1
    if (out_qid) copy_qid(out_qid, &qids[0]);
    return 0;
}

int p9_client_clunk(struct p9_client *c, u32 fid) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_clunk(&c->session, c->out_buf,
                                     sizeof(c->out_buf), fid);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    spin_unlock(&c->lock);
    return 0;
}

// =============================================================================
// IO operations.
// =============================================================================

int p9_client_lopen(struct p9_client *c, u32 fid, u32 flags,
                     struct p9_qid *out_qid, u32 *out_iounit) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_lopen(&c->session, c->out_buf,
                                     sizeof(c->out_buf), fid, flags);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (out_qid) copy_qid(out_qid, &r.open_qid);
    if (out_iounit) *out_iounit = r.open_iounit;
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_lcreate(struct p9_client *c, u32 fid,
                       const u8 *name, size_t name_len,
                       u32 flags, u32 mode, u32 gid,
                       struct p9_qid *out_qid, u32 *out_iounit) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_lcreate(&c->session, c->out_buf,
                                       sizeof(c->out_buf), fid,
                                       name, name_len, flags, mode, gid);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (out_qid) copy_qid(out_qid, &r.open_qid);
    if (out_iounit) *out_iounit = r.open_iounit;
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_read(struct p9_client *c, u32 fid, u64 offset,
                    u32 count, u8 *out_data, u32 *out_count) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    if (count > 0 && !out_data) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_read(&c->session, c->out_buf,
                                    sizeof(c->out_buf),
                                    fid, offset, count);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    // Copy out the data (Rread's data_ptr aliases the transport's
    // recv_buf; we copy so the caller doesn't have to track lifetime).
    if (r.read_count > count) CLIENT_UNLOCK_RET(c, -P9_E_IO);     // defense
    for (u32 i = 0; i < r.read_count; i++) {
        out_data[i] = r.read_data[i];
    }
    if (out_count) *out_count = r.read_count;
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_write(struct p9_client *c, u32 fid, u64 offset,
                     u32 count, const u8 *data, u32 *out_accepted) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    if (count > 0 && !data) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_write(&c->session, c->out_buf,
                                     sizeof(c->out_buf),
                                     fid, offset, count, data);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (out_accepted) *out_accepted = r.write_count;
    spin_unlock(&c->lock);
    return 0;
}

// =============================================================================
// Metadata operations.
// =============================================================================

int p9_client_getattr(struct p9_client *c, u32 fid,
                       u64 request_mask, struct p9_attr *out_attr) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_getattr(&c->session, c->out_buf,
                                       sizeof(c->out_buf),
                                       fid, request_mask);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (out_attr) copy_attr(out_attr, &r.attr);
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_setattr(struct p9_client *c, u32 fid,
                       const struct p9_setattr *attr) {
    if (!c || !attr) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_setattr(&c->session, c->out_buf,
                                       sizeof(c->out_buf), fid, attr);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_readdir(struct p9_client *c, u32 fid, u64 offset,
                       u32 count, u8 *out_data, u32 *out_count) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    if (count > 0 && !out_data) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_readdir(&c->session, c->out_buf,
                                       sizeof(c->out_buf),
                                       fid, offset, count);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (r.readdir_count > count) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    for (u32 i = 0; i < r.readdir_count; i++) {
        out_data[i] = r.readdir_data[i];
    }
    if (out_count) *out_count = r.readdir_count;
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_statfs(struct p9_client *c, u32 fid,
                      struct p9_statfs *out_statfs) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_statfs(&c->session, c->out_buf,
                                      sizeof(c->out_buf), fid);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (out_statfs) copy_statfs(out_statfs, &r.statfs);
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_fsync(struct p9_client *c, u32 fid, u32 datasync) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_fsync(&c->session, c->out_buf,
                                     sizeof(c->out_buf), fid, datasync);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    spin_unlock(&c->lock);
    return 0;
}

// =============================================================================
// Weft operations (Weft-6).
// =============================================================================

// Tweft: request the per-flow zero-copy ring for the flow bound to `fid`.
// On success `*out` carries the netd-minted share_id + ring geometry (plain
// scalars -- no alias into the recv buffer, so they survive the call). The
// dev9p layer joins the share_id to the pinned ring Burrow (SYS_WEFT_MAP).
int p9_client_weft(struct p9_client *c, u32 fid,
                   struct p9_weft_geom *out) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_weft(&c->session, c->out_buf,
                                    sizeof(c->out_buf), fid);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (out) *out = r.weft_geom;
    spin_unlock(&c->lock);
    return 0;
}

// Tweftio: drive a validated payload descriptor through the flow's shared ring.
// On success `*out_count` carries the count of bytes netd moved (TX: queued to
// smoltcp; RX: copied into the ring). Mirrors p9_client_weft -- the lock is
// never held across the blocking recv (the #841 elected reader; client_run).
int p9_client_weftio(struct p9_client *c, u32 fid,
                     u32 off, u32 len, u32 dir, u32 *out_count) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int slen = p9_session_send_weftio(&c->session, c->out_buf,
                                       sizeof(c->out_buf), fid, off, len, dir);
    if (slen < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)slen, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (out_count) *out_count = r.weftio_count;
    spin_unlock(&c->lock);
    return 0;
}

// =============================================================================
// Mutation operations.
// =============================================================================

int p9_client_symlink(struct p9_client *c, u32 fid,
                       const u8 *name, size_t name_len,
                       const u8 *symtgt, size_t symtgt_len,
                       u32 gid, struct p9_qid *out_qid) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_symlink(&c->session, c->out_buf,
                                       sizeof(c->out_buf),
                                       fid, name, name_len,
                                       symtgt, symtgt_len, gid);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (out_qid) copy_qid(out_qid, &r.created_qid);
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_mknod(struct p9_client *c, u32 dfid,
                     const u8 *name, size_t name_len,
                     u32 mode, u32 major, u32 minor, u32 gid,
                     struct p9_qid *out_qid) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_mknod(&c->session, c->out_buf,
                                     sizeof(c->out_buf),
                                     dfid, name, name_len,
                                     mode, major, minor, gid);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (out_qid) copy_qid(out_qid, &r.created_qid);
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_rename(struct p9_client *c, u32 fid, u32 dfid,
                      const u8 *name, size_t name_len) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_rename(&c->session, c->out_buf,
                                      sizeof(c->out_buf),
                                      fid, dfid, name, name_len);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_readlink(struct p9_client *c, u32 fid,
                        u8 *out_target, u16 *out_target_len) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    if (!out_target || !out_target_len) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_readlink(&c->session, c->out_buf,
                                        sizeof(c->out_buf), fid);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    // Caller's `out_target` is assumed to be sized for P9_NAME_MAX
    // (255 bytes) by convention; we cap the copy at *out_target_len
    // on entry to admit smaller caller buffers.
    u16 cap = *out_target_len;
    u16 to_copy = (r.readlink_target_len <= cap) ? r.readlink_target_len : cap;
    for (u16 i = 0; i < to_copy; i++) out_target[i] = r.readlink_target[i];
    *out_target_len = r.readlink_target_len;
    if (r.readlink_target_len > cap) CLIENT_UNLOCK_RET(c, -P9_E_INVAL);
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_link(struct p9_client *c, u32 dfid, u32 fid,
                    const u8 *name, size_t name_len) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_link(&c->session, c->out_buf,
                                    sizeof(c->out_buf),
                                    dfid, fid, name, name_len);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_mkdir(struct p9_client *c, u32 dfid,
                     const u8 *name, size_t name_len,
                     u32 mode, u32 gid, struct p9_qid *out_qid) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_mkdir(&c->session, c->out_buf,
                                     sizeof(c->out_buf),
                                     dfid, name, name_len, mode, gid);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (out_qid) copy_qid(out_qid, &r.created_qid);
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_renameat(struct p9_client *c, u32 olddirfid,
                        const u8 *oldname, size_t oldname_len,
                        u32 newdirfid,
                        const u8 *newname, size_t newname_len) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_renameat(&c->session, c->out_buf,
                                        sizeof(c->out_buf),
                                        olddirfid, oldname, oldname_len,
                                        newdirfid, newname, newname_len);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_unlinkat(struct p9_client *c, u32 dfid,
                        const u8 *name, size_t name_len, u32 flags) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_unlinkat(&c->session, c->out_buf,
                                        sizeof(c->out_buf),
                                        dfid, name, name_len, flags);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    spin_unlock(&c->lock);
    return 0;
}

// =============================================================================
// Query helpers.
// =============================================================================

u32 p9_client_alloc_fid(struct p9_client *c) {
    if (!c) return P9_NOFID;
    if (c->magic != P9_CLIENT_MAGIC) return P9_NOFID;
    spin_lock(&c->lock);
    if (c->next_fid == P9_NOFID) {
        spin_unlock(&c->lock);
        return P9_NOFID;       // exhausted
    }
    u32 fid = c->next_fid;
    c->next_fid++;
    spin_unlock(&c->lock);
    return fid;
}

bool p9_client_is_open(const struct p9_client *c) {
    if (!c) return false;
    if (c->magic != P9_CLIENT_MAGIC) return false;
    // const-cast for the lock: spin_lock requires non-const; the read
    // through the lock is logically const at the caller's level.
    spin_lock(&((struct p9_client *)c)->lock);
    bool open = p9_session_is_open(&c->session);
    spin_unlock(&((struct p9_client *)c)->lock);
    return open;
}

size_t p9_client_inflight(const struct p9_client *c) {
    if (!c) return 0;
    if (c->magic != P9_CLIENT_MAGIC) return 0;
    spin_lock(&((struct p9_client *)c)->lock);
    size_t n = p9_session_inflight(&c->session);
    spin_unlock(&((struct p9_client *)c)->lock);
    return n;
}
