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
            !r->on_complete &&
            // 8c-3 (#89): skip an op whose owner is being stopped -- by the
            // debugger OR (PTY-1f) a job-control stop; the gate is
            // proc_stop_requested's disjunction (round-2 R2-F2: a Ctrl-Z'd
            // owner parks exactly as a debug-stopped one, so handing it the
            // role is the same strand). Its thread parks (8c-2) and cannot run
            // the reader loop, so handing it the role would strand a survivor
            // whose reply needs reading (the wakeup would land on a
            // rpc->rendez whose waiter moved to debug_rendez -- a no-op).
            // Handing to a runnable survivor (or, if none is pending, dropping
            // the role -- reader_active is already false, so a future survivor
            // op self-elects) keeps the shared client LIVE across a stop.
            // `owner` is the submitter's Proc, alive while this rpc is
            // inflight (this deref is as safe as `r->done` above). Async ops
            // (on_complete) are skipped first, so `owner` is never read there.
            // A death (owner group-terminating, not stopped) sets neither stop
            // flag -> unaffected: the existing F6 bounce handles the
            // dying-owner case.
            !(r->owner && proc_stop_requested(r->owner))) {
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
// 8c-3 (#89, F1): the inner recv. The wrapper (reader_recv_frame) holds
// stop_no_park for the whole tenure so a mid-frame stop BLOCKS THROUGH; this
// body sets self->stop_unwinds = (got == 0) before each recv, so a stop
// (either owner -- the debugger's, or PTY-1f's job stop; the detour gate is
// proc_stop_requested) UNWINDS the reader ONLY at a frame boundary (no bytes
// of the frame consumed) and NEVER mid-frame (unwinding mid-frame discards
// the consumed partial bytes -> the survivor reader reads the frame TAIL as
// a header -> stream desync). `self` may be a kproc thread (SQPOLL, site 4):
// kproc is neither debuggable nor job-stoppable (both delivers reject it),
// so both stop flags are always 0 and the detour never fires -- the sets are
// harmless.
static int do_reader_recv_frame(struct p9_client *c, u64 deadline_ns, bool *idle) {
    struct p9_transport *t = &c->transport;
    struct Thread *self = current_thread();
    u8 *buf = t->recv_buf;
    size_t cap = t->recv_cap;
    size_t got = 0;
    if (idle) *idle = false;
    while (got < P9_HDR_LEN) {
        if (got == 0 && deadline_ns)
            p9_transport_set_recv_deadline(t, deadline_ns);
        // Unwindable-by-stop ONLY at got==0 (a clean frame boundary).
        if (self) self->stop_unwinds = (got == 0);
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
        // Mid-frame (got>0): a stop must NOT unwind here -- block through.
        if (self) self->stop_unwinds = false;
        int n = t->ops.recv(t->ops.ctx, buf + got, (size_t)size - got);
        if (n == 0) return 0;              // mid-frame EOF: peer vanished mid-reply (device-gone)
        if (n < 0)  return -1;             // transport error
        if ((size_t)n > (size_t)size - got) return -1;
        got += (size_t)n;
    }
    return (int)got;
}

// 8c-3 (#89, F1): the frame-atomic reader recv. stop_no_park is held for the
// WHOLE recv so the sched detour blocks a mid-frame stop through (the reader
// finishes the frame, bounded by the trusted server's delivery -- CF-3 B) and
// unwinds ONLY at a frame boundary (do_reader_recv_frame sets stop_unwinds).
// Both flags are cleared on exit so a following client_debug_stop_park PARKS.
// Centralizing here gives all four reader_active-holding callers (the
// client_wait election, the client_pump_or_park_locked self-pump, and both
// p9_client_reader_pump_once variants) the block-through, closing F2.
static int reader_recv_frame(struct p9_client *c, u64 deadline_ns, bool *idle) {
    struct Thread *self = current_thread();
    // Reset stop_unwound at ENTRY (per-recv). The detour SETS it if this recv
    // stop-unwinds at a boundary; the client classifier READS+clears it after we
    // return (F1 re-audit -- a STABLE signal vs a racy debug_stop_req re-read).
    // It is deliberately NOT cleared at exit (it must survive to the classifier).
    if (self) { self->stop_no_park = true; self->stop_unwound = false; }
    int r = do_reader_recv_frame(c, deadline_ns, idle);
    if (self) { self->stop_no_park = false; self->stop_unwinds = false; }
    return r;
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

// 8c-3 (#89): is a stop pending on the calling thread's Proc -- a debugger
// stop OR (PTY-1f) a job-control stop? Mirror of client_self_dying(). A
// stopped thread must not hold or assume the elected-reader role -- it parks
// (8c-2), and a parked reader freezes every survivor sharing this client; the
// job axis is round-2 R2-F2's exact re-opening of #89 (an everyday Ctrl-Z of
// a fg child blocked as the elected reader on the shared SYSTEM-Stratum
// client would freeze /bin,/lib for every survivor until fg), so the gate is
// proc_stop_requested's disjunction. ACQUIRE pairs with each deliver's
// RELEASE set; a racy miss at the client layer is caught by the sleep/tsleep
// detour's register-then-observe under wait_lock (the I-9-critical path). A
// kproc thread has t->proc == kproc() (non-NULL), but kproc is neither
// debuggable nor job-stoppable (both delivers reject it), so both flags are
// always 0 -> this returns false for a kthread.
static bool client_stop_pending(struct Thread *t) {
    return t && t->proc && proc_stop_requested(t->proc);
}

// 8c-3 (#89): park the calling thread on its debug_rendez for a pending stop
// (either owner), with the reader role ALREADY released. Drops c->lock across
// the blocking park (proc_stop_sleeper_park sleeps on debug_rendez until BOTH
// stop owners clear) then re-acquires it. stop_unwinds MUST be false here so
// this park PARKS (it must not itself unwind). Returns SLEEP_OK on resume, or
// SLEEP_INTR if the Proc started dying while stopped -> the caller re-loops and
// client_self_dying() unwinds (DEATH WINS). c->lock HELD on entry + exit.
static int client_debug_stop_park(struct p9_client *c) {
    struct Thread *t = current_thread();
    spin_unlock(&c->lock);
    int drc = proc_stop_sleeper_park(t);
    spin_lock(&c->lock);
    return drc;
}

// Block until rpc->done, the session dies, or my Proc dies (the elected-reader
// loop). c->lock HELD on entry + exit.
static int client_wait(struct p9_client *c, struct p9_rpc *rpc) {
    struct Thread *t = current_thread();
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
        if (client_stop_pending(t)) {
            // 8c-3 (#89): a stop is pending on my Proc (a debugger stop or, per
            // PTY-1f, a job-control stop -- client_stop_pending reads the
            // disjunction, so an everyday Ctrl-Z gets the SAME role release).
            // I must NOT hold or assume the elected-reader role -- a stopped
            // thread parks (8c-2), and a parked reader freezes every survivor
            // sharing this client until the resume. Release the role
            // (re-hand-off if I was designated the next reader, mirroring the
            // F6 death case) + park role-free + re-loop on resume to
            // re-elect. The handoff skips stopped owners, so the role always
            // lands on a survivor (or is dropped -> a future survivor op
            // self-elects). This is the top-of-loop guard so a stopped thread
            // that reaches client_wait fresh (a mid-syscall stop) parks
            // PROMPTLY instead of electing + reading. The blocked-reader case
            // (the common one) is handled below via stop_unwinds.
            // DEBUG-FS-DESIGN 5c.6.
            if (rpc->be_reader) {
                rpc->be_reader = false;
                client_handoff_reader_locked(c, rpc);
            }
            (void)client_debug_stop_park(c);   // drop c->lock, park, re-acquire
            continue;                          // resumed (or dying): re-check
        }
        if (!c->reader_active) {
            // Become THE reader: read frames (c->lock dropped) until MY reply
            // lands, the session dies, or I'm dying.
            c->reader_active = true;
            rpc->be_reader   = false;
            // 8c-3 (#89, F1): reader_recv_frame manages stop_no_park/stop_unwinds
            // (frame-atomic -- a mid-frame stop blocks through, a boundary stop
            // unwinds the recv). On a boundary unwind the recv returns <= 0 with
            // client_stop_pending true -> `stopped` -> release + hand off the
            // role + park role-free below (the wrapper cleared both flags, so the
            // park PARKS), then re-elect on resume.
            bool stopped     = false;
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
                    // death-interrupt: unwind. Clear a possibly-set stop_unwound
                    // (a stop+death at the same boundary sets it) so no branch
                    // leaves the latch set -- symmetric with the arms below (the
                    // reader_recv_frame entry reset already guards a later read;
                    // this is defense-in-depth on the dying path).
                    if (t) t->stop_unwound = false;
                    break;
                } else if (t && t->stop_unwound) {
                    // 8c-3 (#89): my recv was stop-unwound at a frame boundary (the
                    // detour's stop_unwinds branch). F1 re-audit: read the STABLE
                    // stop_unwound latch, NOT client_stop_pending (which re-reads
                    // the stop flags -- each cleared asynchronously by ITS resume:
                    // debug_stop_req by proc_debug_resume on a debugger
                    // detach/death, job_stop_req by the PTY-1f tty:cont fan --
                    // so a resume in this window would misclassify a benign
                    // stop-unwind as a real break -> mark the SHARED session
                    // dead). Not a death, not a real error: release + hand off +
                    // park role-free below. Read+clear (owner-only).
                    t->stop_unwound = false;
                    stopped = true;
                    break;
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
            if (stopped) {
                // Role released + handed to a survivor; park role-free, then
                // re-loop to re-elect on resume. My reply may have been demuxed by
                // the survivor reader while I was stopped -> the re-loop's
                // rpc->done check returns CLIENT_WAIT_DONE.
                (void)client_debug_stop_park(c);
            }
            // re-loop: done / dead / dying / stop now decides the return.
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
// Make ONE unit of s2c progress while back-pressured, then return so the caller
// re-tests its retry condition. Either self-pump one s2c frame (draining a reply
// -> the server frees a c2s slot AND its tag) when no reader is active, or park
// on the multi-waiter list until an active reader signals progress. c->lock HELD
// on entry + exit (dropped only across the blocking recv/sleep). Extracted from
// client_send_flow so the FID-LIFECYCLE async-clunk's tag-pool drain reuses the
// IDENTICAL, audited wait/wake body (F1/F2 share this root).
static void client_pump_or_park_locked(struct p9_client *c, struct p9_rpc *rpc) {
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
        struct Thread *self = current_thread();
        if (rr > 0) {
            demux_frame_locked(c, (size_t)rr);
        } else {
            bool unwound = self && self->stop_unwound;   // F1 re-audit: stable latch
            if (unwound) self->stop_unwound = false;     // read+clear (owner-only)
            if (!client_self_dying() && !unwound) {
                // rr <= 0, not self-dying, not a boundary stop-unwind -> a real
                // transport break; latch the shared session dead (#841 fail-close).
                // A stop-unwind (8c-3 F2) skips this -- the role is released below,
                // and client_send_flow / client_drain_until_free_tag parks me at
                // its loop top (else I would spin re-electing without draining).
                // Reading the STABLE stop_unwound latch (not client_stop_pending)
                // closes the F1 async-resume race that would spuriously mark the
                // SHARED session dead on a debugger detach/death mid-stop.
                client_mark_dead_locked(c, rr == 0);
            }
        }
        c->reader_active = false;
        client_send_progress_signal(c);          // a c2s slot / tag may have freed
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

// client_send_flow return contract (#52): 0 = the whole frame is on the wire;
// CLIENT_SEND_NEVER = the frame NEVER reached the wire and the byte stream is
// INTACT (a self-dying sender refusing to park, the session observed already
// dead, or a spill-OOM under back-pressure -- in every case the transport's
// all-or-nothing contract means zero bytes were pushed), so the caller must
// reclaim the never-sent tag (p9_session_abort_unsent) and must NOT latch the
// shared session dead; any other negative = a genuine transport break (the
// stream may hold a partial frame), the caller latches dead as before.
#define CLIENT_SEND_NEVER  (-1000)

static int client_send_flow(struct p9_client *c, size_t built_len,
                            struct p9_rpc *rpc) {
    // #375: the frame is built in the SHARED c->out_buf, but the pump/park
    // above DROPS c->lock -- and a peer may then legally build ITS frame into
    // out_buf. Pre-spill, the retry pushed built_len bytes of whatever out_buf
    // then held: an equal-length peer frame (two msize-clamped F1 flush
    // Twrites -- the dominant concurrent cold-build shape) went out as a clean
    // DUPLICATE with this frame LOST; the duplicate's second reply landed on
    // the freed-then-reused tag as a WRONG reply (an Rlerror parses cleanly
    // for ANY op -- 9P has no per-tag wire generation), so a stray
    // Rlerror(ENOENT) poisoned a live Twalkgetattr into a persistent negative
    // dentry (the task-#50 S3 ENOENT cluster) and injected stray errors (the
    // write EIO). An unequal-length peer frame fails p9_transport_send's
    // size==len validation -> session death. So: at the FIRST back-pressure,
    // BEFORE the window can open, spill the frame to a private buffer and
    // retry from the spill -- out_buf is never re-read after the lock has
    // dropped, closing the whole class (including the DIED-path Tflush +
    // async-clunk writers, which reuse out_buf assuming the prior frame is
    // fully on the wire). kmalloc under c->lock is non-blocking (the
    // rpc->reply_buf precedent). The transport's all-or-nothing EAGAIN
    // contract (zero bytes pushed) is what makes the retry-from-spill exact:
    // there is never a partial prefix on the ring -- and it is also what
    // makes the CLIENT_SEND_NEVER exits (self-dying / dead-observed /
    // spill-OOM) safely reclaimable: the server never saw the frame.
    u8 *spill = NULL;
    const u8 *frame = c->out_buf;
    int rc;
    struct Thread *self = current_thread();
    for (;;) {
        if (client_self_dying()) { rc = CLIENT_SEND_NEVER; break; }
        if (c->dead)             { rc = CLIENT_SEND_NEVER; break; }

        // 8c-3 (#89, F2): a debugger stop is pending -- park role-free instead
        // of retrying/self-pumping. A stop-unwound self-pump drains nothing (it
        // unwinds at the frame boundary), so retrying would SPIN: c2s never
        // frees -> every send EAGAINs -> the Proc never fully-stops -> the
        // debugger's stop HANGS. Spill out_buf FIRST -- client_debug_stop_park
        // drops c->lock, opening the #375 out_buf-clobber window; on resume the
        // retry pushes the spill, never a peer's overwritten frame.
        if (client_stop_pending(self)) {
            if (!spill) {
                spill = kmalloc(built_len, 0);
                if (!spill) { rc = CLIENT_SEND_NEVER; break; }   // never sent
                client_copy(spill, c->out_buf, built_len);
                frame = spill;
            }
            (void)client_debug_stop_park(c);
            continue;
        }

        int src = p9_transport_send(&c->transport, frame, built_len);
        if (src == 0)                   { rc = 0; break; }          // whole frame sent
        if (src != P9_TRANSPORT_EAGAIN) { rc = -P9_E_IO; break; }   // genuine break

        if (!spill) {
            spill = kmalloc(built_len, 0);
            if (!spill) { rc = CLIENT_SEND_NEVER; break; }   // never sent (zero pushed)
            client_copy(spill, c->out_buf, built_len);
            frame = spill;
        }
        client_pump_or_park_locked(c, rpc);         // drain c2s, then retry
    }
    if (spill) kfree(spill);
    return rc;
}

// FID-LIFECYCLE async-clunk F1: drain ownerless replies until a tag slot frees.
// A >64-fd async-close burst (the #926 proc-exit close of a handle table, or a
// userspace batch-close) with no interleaved sync op fills the 64-slot tag pool
// with undrained ownerless Rclunks; the next p9_session_send_clunk's alloc_tag
// would then fail and the fid would leak BOUND (send_clunk returns before its
// fid_unbind) -> eventual bound_fids[] exhaustion -> a system-wide FS partial
// DoS on the shared mount. Draining ANY reply frees its tag (in a full pool the
// outstanding set is dominated by ownerless clunks, so a drain frees a clunk
// tag; even a sync peer's reply drain frees a tag). Uses the SAME pump/park body
// as the send flow. c->lock HELD; returns 0 (a tag is free) or -P9_E_IO (death).
static int client_drain_until_free_tag(struct p9_client *c, struct p9_rpc *rpc) {
    struct Thread *self = current_thread();
    for (;;) {
        // has_free_tag FIRST (round-2 F1): with a free tag, proceed to
        // send_clunk even when the caller's Proc is dying, so the fid unbinds
        // cleanly -- matching the old sync clunk (which unbound BEFORE it
        // detected death). Only the rare burst-DURING-a-kill (pool full AND
        // dying) bails, and a bail there leaks the fid bound exactly as the old
        // sync path did on the same race. The common (not-in-a-burst) close is a
        // free tag -> a clean immediate unbind.
        if (p9_session_has_free_tag(&c->session)) return 0;
        if (c->dead)             return -P9_E_IO;
        if (client_self_dying()) return -P9_E_IO;
        // 8c-3 (#89, F2): a debugger stop -- park role-free (no frame built, so
        // no spill). Else a stop-unwound self-pump would spin (drains nothing).
        // Resume re-checks has_free_tag.
        if (client_stop_pending(self)) { (void)client_debug_stop_park(c); continue; }
        client_pump_or_park_locked(c, rpc);
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
    if (p9_peek_header(c->out_buf, built_len, &size, &type, &tag) < 0) {
        // Unreachable by construction (session_send_* built the frame), but a
        // future builder bug would otherwise leak the just-marked tag on a
        // LIVE session with no way to identify it -- mirror submit_async's
        // fail-closed posture (the orphaned tag is moot on a dead session).
        client_mark_dead_locked(c, false);
        return -P9_E_IO;
    }

    if (tag == P9_NOTAG) {
        // Tversion (the only NOTAG message; serial handshake path -- single-
        // threaded on a fresh client, so send + recv-one + dispatch is safe).
        //
        // #360: drop c->lock across the exchange -- its recv BLOCKS (the
        // srvconn/spoor transports tsleep), and a plain spinlock may never be
        // held across sched() (the lock-across-sleep class; sched() asserts).
        // The drop is data-race-free by the same argument that made the old
        // held-across-recv shape deadlock-free: Tversion exists only inside
        // p9_client_handshake on a client that is PRIVATE by construction
        // (p9_attached_create has not yet published it -- no spoor, no handle,
        // no peer thread can reach c->lock / out_buf / session). The elected-
        // reader steady state already follows this exact drop discipline
        // (client_wait line ~384).
        spin_unlock(&c->lock);
        int rc = p9_transport_exchange(&c->transport, &c->session,
                                       c->out_buf, built_len, out);
        spin_lock(&c->lock);
        return map_error(0, rc, out);
    }
    if (tag >= P9_SESSION_MAX_OUTSTANDING) {
        client_mark_dead_locked(c, false);   // as above: fail closed, not leak
        return -P9_E_IO;                 // the session never allocates such a tag
    }

    struct p9_rpc rpc;
    rpc.tag         = (u16)tag;
    rpc.done        = false;
    rpc.dead        = false;
    rpc.be_reader   = false;
    rpc.reply_len   = 0;
    rpc.on_complete = NULL;          // sync (WAKE_RENDEZ): the submitter waits
    struct Thread *submitter = current_thread();
    rpc.owner       = submitter ? submitter->proc : NULL;   // 8c-3 (#89): the
                                     // handoff skips a stopped owner's op
                                     // (debug OR job -- PTY-1f);
                                     // NULL for a kproc client (never stopped)
    rendez_init(&rpc.rendez);
    rpc.reply_buf = kmalloc(c->recv_cap, KP_ZERO);
    if (!rpc.reply_buf) {
        // #52: the caller's session_send_* already marked outstanding[tag],
        // but the frame will never be sent -- reclaim the slot (never-sent =>
        // I-10-safe) or 64 such OOMs wedge the shared session's tag pool.
        p9_session_abort_unsent(&c->session, tag);
        return -P9_E_IO;
    }

    c->inflight[tag] = &rpc;

    // Send the framed Tmsg with #349 flow control. c->lock HELD: holding it
    // serializes senders so two frames never interleave; a transiently-full c2s
    // ring (back-pressure under #841 pipelining) is drained + retried inside
    // client_send_flow, NOT a death (the pre-#349 path marked the whole session
    // dead here, killing every other op including in-flight text page-ins).
    int sfr = client_send_flow(c, built_len, &rpc);
    if (sfr == CLIENT_SEND_NEVER) {
        // #52: the frame never reached the wire and the stream is intact (a
        // self-dying sender, a dead-observed session, or a spill-OOM -- all
        // zero-bytes-pushed by the all-or-nothing contract). Reclaim the tag
        // (never-sent => I-10-safe) and leave the session LIVE for peers --
        // pre-#52 the self-dying path leaked the slot and the spill-OOM
        // latched the whole shared session dead.
        c->inflight[tag] = NULL;
        kfree(rpc.reply_buf);
        p9_session_abort_unsent(&c->session, tag);
        return -P9_E_IO;
    }
    if (sfr < 0) {
        c->inflight[tag] = NULL;
        kfree(rpc.reply_buf);
        // A genuine transport break: the stream may hold a partial frame, so
        // latch the session dead (every op then fails closed). The tag slot is
        // moot on a dead session (teardown reclaims).
        if (!c->dead)
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
                                         c->out_buf_cap, (u16)tag);
        if (flen <= 0) {
            // R2-F1: no flush could be staged (pool full at the abandon
            // instant) -- the owner is still gone; mark abandoned so the
            // #294 cancel-then-close clunk is not refused (see rollback).
            p9_session_mark_abandoned(&c->session, (u16)tag);
        }
        if (flen > 0) {
            int fsr = p9_transport_send(&c->transport, c->out_buf, (size_t)flen);
            if (fsr == P9_TRANSPORT_EAGAIN) {
                // #53: a transiently-full c2s ring is BACK-PRESSURE, not a
                // break. A dying thread must not park (it is unwinding), and
                // latching the SHARED session dead here is the #349 collapse
                // on a different send path. Roll the flush back (the flush
                // frame never left -> its tag is I-10-safe to free) and fall
                // to the pre-#845 ownerless reclaim: outstanding[tag] stays
                // ACTIVE until the late original reply is drained by a
                // survivor's reader, or session teardown reclaims it.
                p9_session_flush_rollback(&c->session, (u16)tag);
            } else if (fsr < 0) {
                client_mark_dead_locked(c, false);
            }
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
    int built = build(&c->session, c->out_buf, c->out_buf_cap, build_ctx);
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
    rpc->owner     = NULL;          // 8c-3 (#89): async ops are skipped in the
                                    // handoff (on_complete != NULL first), so
                                    // owner is never read -- keep it non-garbage
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
    } else if (current_thread() && current_thread()->stop_unwound) {
        // 8c-3 (#89, F2 + F1 re-audit): a debugger stop unwound the recv at a
        // FRAME BOUNDARY (frame-atomic -- no bytes lost). NOT a break: do NOT
        // latch the shared session dead. Hand the role off below; this thread
        // parks at its EL0-return tail (pump_once is one-shot). Read the STABLE
        // stop_unwound latch, NOT client_stop_pending (which races an async
        // proc_debug_resume -> would misclassify a stop-unwind as a real break ->
        // spuriously mark the SHARED session dead). Read+clear (owner-only). A
        // kproc caller never sets it (the detour never fires for a kthread).
        current_thread()->stop_unwound = false;
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
        // 8c-3 (#89, F2 + F1 re-audit): NO stop_unwound arm here (unlike
        // pump_once) -- the deadline variant is called ONLY by the SQPOLL /
        // dev9p-poll-pump KPROC kthreads (dev9p_poll.c, loom.c). Their t->proc is
        // kproc() (NON-NULL), but kproc is neither debuggable nor job-stoppable
        // (both delivers reject it -- PTY-1f), so both stop flags stay 0, the
        // detour never fires, and stop_unwound is never set -> a stop can never unwind this
        // recv. If an EL0 thread is ever added as a caller, add the stop_unwound
        // arm like pump_once.
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
                                             c->out_buf_cap, tag);
            if (flen <= 0) {
                // R2-F1: flush-less abandon (see the DIED-path twin).
                p9_session_mark_abandoned(&c->session, tag);
            }
            if (flen > 0) {
                int fsr = p9_transport_send(&c->transport, c->out_buf,
                                            (size_t)flen);
                if (fsr == P9_TRANSPORT_EAGAIN) {
                    // #53: back-pressure, not a break (see the DIED-path
                    // twin). Roll the flush back; the abandoned op's tag
                    // stays reserved until its late reply drains ownerlessly
                    // or teardown reclaims -- never latch the shared session.
                    p9_session_flush_rollback(&c->session, tag);
                } else if (fsr < 0) {
                    client_mark_dead_locked(c, false);
                }
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
    // CF-3 B two-tier outbound buffer: a default-msize session builds frames
    // in the inline array (no allocation -- every static test client + every
    // small-frame service); a bulk session (msize > the inline cap) takes a
    // heap buffer sized to its msize so the frame-build bound matches the
    // proposal. OOM DEGRADES to the inline tier: writes then clamp at the
    // smaller cap (a short op, never a failed init -- reads are unaffected,
    // their payload rides the recv buffer). No KP_ZERO: every send writes a
    // built frame of exact length, so uninitialized bytes never leave.
    // B1-audit F3: the cache-mode flags are contract-bearing ("set ONLY from
    // the validated attach flag") -- initialize them explicitly rather than
    // relying on allocation zeroing, so an in-place re-init of a recycled
    // client (destroy -> init, no re-alloc -- the test scaffolding's shape)
    // can never carry a stale loose/cacheable/wga latch across lives.
    c->loose           = false;
    c->cacheable       = false;
    c->wga_unsupported = false;
    c->out_buf     = c->out_buf_inline;
    c->out_buf_cap = P9_CLIENT_OUT_BUF_MAX;
    if (msize > P9_CLIENT_OUT_BUF_MAX) {
        u8 *big = kmalloc(msize, 0);
        if (big) {
            c->out_buf     = big;
            c->out_buf_cap = msize;
        }
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
    c->wga_unsupported = false;   // POUNCE: Twalkgetattr capability unknown
    c->cacheable       = false;   // L1e: not a proven content-versioned FS yet
    larder_init(&c->larder);      // L1c: the guest FS cache (its own leaf lock)
    // G2: the dir-fid table starts empty; arm its leaf lock and zero it (init
    // may run on non-zeroed storage -- the loopback tests stack/reuse clients).
    // Parked fids die with the session, so destroy has no wire teardown for it.
    for (size_t di = 0; di < sizeof(c->dirfid); di++) ((u8 *)&c->dirfid)[di] = 0;
    spin_lock_init(&c->dirfid.lock);
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
    // CF-3 B: release a heap out_buf (bulk-msize tier). The inline tier is
    // storage inside *c -- nothing to free. No op is in flight at destroy
    // (the last attached ref dropped), so no builder can be mid-frame here.
    if (c->out_buf && c->out_buf != c->out_buf_inline) kfree(c->out_buf);
    c->out_buf     = NULL;
    c->out_buf_cap = 0;
    // L1e: free the Larder's lazily-allocated page buffers (the attr/dentry
    // sub-caches are inline in *c -- nothing to free there). No op is in flight
    // (last attached ref dropped), so the Larder lock is uncontended.
    larder_destroy(&c->larder);
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
                                       c->out_buf_cap, NULL, 0);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }

    // Phase 2: Tattach → Rattach (drives VERSIONED → OPEN). A real tag -> the
    // elected-reader path; the fresh client is unshared, so the single thread
    // simply becomes the reader for its own Rattach (no contention).
    len = p9_session_send_attach(&c->session, c->out_buf,
                                  c->out_buf_cap,
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
                                    c->out_buf_cap,
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

int p9_client_walkgetattr(struct p9_client *c,
                          u32 src_fid, u32 new_fid,
                          u64 request_mask,
                          u16 nwname,
                          const u8 *const *names, const size_t *name_lens,
                          u16 *out_nwqid, struct p9_qid *out_qids,
                          struct p9_attr *out_attrs) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_walkgetattr(&c->session, c->out_buf,
                                          c->out_buf_cap,
                                          src_fid, new_fid, request_mask,
                                          nwname, names, name_lens);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    if (out_nwqid) *out_nwqid = r.nwqid;
    if (out_qids) {
        for (u16 i = 0; i < r.nwqid && i < P9_MAX_WALK; i++)
            copy_qid(&out_qids[i], &r.qids[i]);
    }
    if (out_attrs && r.wga_data) {
        // The reply frame is retained (done_reply_buf) until the NEXT op
        // on this client and we still hold c->lock, so the fixed-stride
        // elements (frame-validated by p9_parse_rwalkgetattr) are safe
        // to extract here.
        for (u16 i = 0; i < r.nwqid && i < P9_MAX_WALK; i++) {
            if (p9_parse_getattr_body(
                    r.wga_data + (size_t)i * P9_WGA_BODY_LEN,
                    P9_WGA_BODY_LEN, &out_attrs[i]) < 0) {
                c->total_errors++;
                CLIENT_UNLOCK_RET(c, -P9_E_IO);
            }
        }
    }
    spin_unlock(&c->lock);
    return 0;
}

int p9_client_clunk(struct p9_client *c, u32 fid) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    int len = p9_session_send_clunk(&c->session, c->out_buf,
                                     c->out_buf_cap, fid);
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    struct p9_dispatch_result r;
    int e = client_run(c, (size_t)len, &r);
    c->total_ops++;
    if (e != 0) { c->total_errors++; CLIENT_UNLOCK_RET(c, e); }
    spin_unlock(&c->lock);
    return 0;
}

// Fire-and-forget clunk (FID-LIFECYCLE async-clunk; docs/FID-LIFECYCLE-DESIGN.md
// section 3.1): send the Tclunk and return WITHOUT blocking on Rclunk, so the
// submitter's thread is not parked for the clunk RTT (the go-build close path was
// 21% of the warm-floor RTs). Correctness rests on the #845 Tflush ownerless-drain
// discipline, and the verified fid-lifecycle facts make it hazard-light:
//   - I-11 (fid): p9_session_send_clunk unbinds the fid at SEND (already
//     reply-independent); the monotonic fid NUMBER (p9_client_alloc_fid) is never
//     reused, so the clunk-pending fid can never be re-referenced.
//   - I-10 (tag): the Tclunk marks session.outstanding[tag] but we register NO
//     c->inflight[tag] owner, so the Rclunk is OWNERLESS. A later op's elected
//     reader drains it via demux_frame_locked's else-branch -> dispatch_rmsg
//     (the P9_TCLUNK arm, "send-time already unbound; no further action") ->
//     clear_outstanding frees the tag.
// Two contention cases the fire-and-forget must compose with (the arc-audit
// F1/F2), both via the SAME audited pump/park machinery the sync send uses:
//   - F1 tag-pool full: a >64-fd async-close BURST with no interleaved sync op
//     fills all 64 outstanding[] slots with undrained ownerless Rclunks; without
//     a drain, alloc_tag fails and the fid leaks BOUND (send_clunk returns before
//     fid_unbind) -> bound_fids[] exhaustion -> a shared-mount FS DoS. FIX:
//     client_drain_until_free_tag pumps an ownerless reply (freeing a tag) before
//     send_clunk. So the burst self-limits (each clunk drains ~1, sends 1).
//   - F2 c2s back-pressure: a transiently-full c2s ring returns EAGAIN; the send
//     goes through client_send_flow (self-pump s2c + retry), NEVER a session
//     death (the pre-fix raw send treated EAGAIN as fatal -- the #349 collapse).
// A GENUINE transport break marks the session dead (mirror client_run). A
// send_clunk failure now means only fid unbound / root / a live op on the fid --
// all cases where the fid should not be clunked here; the sole caller
// (dev9p_close) ignores it exactly as it ignored a sync-clunk error.
int p9_client_clunk_async(struct p9_client *c, u32 fid) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);

    // The async clunk composes with the #841 tag pool + the #349 c2s back-
    // pressure via a SYNTHETIC (never-inflight) rpc token: the pump/park body
    // uses it only for `be_reader` + the handoff "not-me" pointer compare (it is
    // never stored in c->inflight[], so a stack rpc is safe). on_complete = NULL,
    // be_reader = false.
    struct p9_rpc rpc;
    for (size_t i = 0; i < sizeof(rpc); i++) ((u8 *)&rpc)[i] = 0;

    // F1: ensure a free tag BEFORE send_clunk (else alloc_tag fails and the fid
    // leaks bound). Drains ownerless Rclunks on a full pool (the close-burst).
    int de = client_drain_until_free_tag(c, &rpc);
    if (de != 0) CLIENT_UNLOCK_RET(c, de);

    int len = p9_session_send_clunk(&c->session, c->out_buf,
                                     c->out_buf_cap, fid);
    // send_clunk fails only for a non-tag reason now (fid unbound / root / a live
    // op targets the fid) -- all cases where the fid should NOT be clunked here;
    // the caller ignores the error exactly as it ignored a sync-clunk error.
    if (len < 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    // #52: capture the clunk's tag NOW -- out_buf is reusable by a peer once
    // send_flow's pump/park drops c->lock (the #375 lesson: a post-send peek
    // could read a PEER's frame and abort the peer's LIVE tag).
    u32 csz; u8 cty; u16 ctag = P9_NOTAG;
    (void)p9_peek_header(c->out_buf, (size_t)len, &csz, &cty, &ctag);

    // F2: send via the #349 flow-control discipline -- a transiently-full c2s
    // ring is back-pressure (self-pump s2c + retry), NEVER a session death (the
    // pre-fix raw send treated EAGAIN as fatal, re-opening the #349 shared-mount
    // collapse). No inflight[tag] is registered, so the Rclunk is OWNERLESS
    // (drained by a later op's reader or this function's own F1 drain in a
    // burst). On a GENUINE break, mirror client_run: mark dead only if the
    // ring actually broke (a self-death leaves the session live for peers).
    int sfr = client_send_flow(c, (size_t)len, &rpc);
    if (sfr == CLIENT_SEND_NEVER) {
        // #52: never-sent (see client_run). Reclaim the clunk's tag (captured
        // BEFORE the send -- out_buf is not re-readable here) -- the frame
        // never left, so I-10 holds on immediate reuse. The fid was already
        // unbound at send (I-11, reply-independent); the server-side fid
        // persists until session end -- the documented dying-path cost,
        // bounded, vs the pre-#52 leak of a tag slot on a LIVE session.
        if (ctag != P9_NOTAG)
            p9_session_abort_unsent(&c->session, ctag);
        CLIENT_UNLOCK_RET(c, -P9_E_IO);
    }
    if (sfr < 0) {
        if (!c->dead)
            client_mark_dead_locked(c, false);
        CLIENT_UNLOCK_RET(c, -P9_E_IO);
    }
    c->total_ops++;
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
                                     c->out_buf_cap, fid, flags);
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
                                       c->out_buf_cap, fid,
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

// CF-3 A: a single op's payload is bounded by the negotiated msize (and, for
// Twrite, additionally by this client's out_buf -- the frame-build bound).
// Pre-lift no syscall caller could exceed either (SYS_RW_MAX was 4096); the
// lift made bulk counts reachable, and an unclamped Twrite FAILED the frame
// build (-P9_E_IO on every over-payload write -- the CF-3 bench cascade:
// EIO'd object writes -> no cache puts -> the warm build ran cold). The
// client clamps and returns the SHORT count -- the protocol's own contract
// (a 9P client never emits an op exceeding the negotiated msize); callers
// loop per the POSIX short-read/short-write discipline.
static u32 client_max_read_count(const struct p9_client *c) {
    u32 ms = c->session.negotiated_msize ? c->session.negotiated_msize
                                         : c->session.msize;
    // Rread reply framing: hdr(7) + count(4).
    return (ms > P9_HDR_LEN + 4u) ? ms - (P9_HDR_LEN + 4u) : 0;
}

static u32 client_max_write_payload(const struct p9_client *c) {
    u32 ms = c->session.negotiated_msize ? c->session.negotiated_msize
                                         : c->session.msize;
    if (ms > (u32)c->out_buf_cap) ms = (u32)c->out_buf_cap;
    // Twrite framing: hdr(7) + fid(4) + offset(8) + count(4).
    return (ms > P9_HDR_LEN + 16u) ? ms - (P9_HDR_LEN + 16u) : 0;
}

int p9_client_read(struct p9_client *c, u32 fid, u64 offset,
                    u32 count, u8 *out_data, u32 *out_count) {
    if (!c) return -P9_E_INVAL;
    if (c->magic != P9_CLIENT_MAGIC) return -P9_E_INVAL;
    if (count > 0 && !out_data) return -P9_E_INVAL;
    spin_lock(&c->lock);
    if (c->dead) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (!p9_session_is_open(&c->session)) CLIENT_UNLOCK_RET(c, -P9_E_BUSY);
    u32 rmax = client_max_read_count(c);
    // Audit F2: a degenerate negotiated msize (<= the framing overhead)
    // would clamp every op to 0 -- a spurious EOF a looping reader spins
    // on. Unreachable from any v1.0 mount (all >= 4096); fail closed.
    if (rmax == 0 && count > 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (count > rmax) count = rmax;      // short read; the caller loops
    int len = p9_session_send_read(&c->session, c->out_buf,
                                    c->out_buf_cap,
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
    u32 wmax = client_max_write_payload(c);
    // Audit F2: the write-side twin of the degenerate-msize guard above --
    // a 0-clamp would return accepted=0 forever to a looping writer.
    if (wmax == 0 && count > 0) CLIENT_UNLOCK_RET(c, -P9_E_IO);
    if (count > wmax) count = wmax;      // short write; the caller loops
    int len = p9_session_send_write(&c->session, c->out_buf,
                                     c->out_buf_cap,
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
                                       c->out_buf_cap,
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
                                       c->out_buf_cap, fid, attr);
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
                                       c->out_buf_cap,
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
                                      c->out_buf_cap, fid);
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
                                     c->out_buf_cap, fid, datasync);
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
                                    c->out_buf_cap, fid);
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
                                       c->out_buf_cap, fid, off, len, dir);
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
                                       c->out_buf_cap,
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
                                     c->out_buf_cap,
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
                                      c->out_buf_cap,
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
                                        c->out_buf_cap, fid);
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
                                    c->out_buf_cap,
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
                                     c->out_buf_cap,
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
                                        c->out_buf_cap,
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
                                        c->out_buf_cap,
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
