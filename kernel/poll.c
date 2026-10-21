// poll — kernel-side mechanism + the SYS_POLL testable core.
//
// Per ARCHITECTURE.md §23.3 + specs/poll.tla. See <thylacine/poll.h>
// for the design preamble (single-waiter Rendez constraint; the
// register-then-observe discipline; per-list lock with object → list
// → rendez ordering; stack-allocated hook lifetime).
//
// SPEC-TO-CODE mapping (specs/poll.tla; the remote half is specs/net_poll.tla):
//
//   Register / Resample  ↔ a pass's scan: `dev->poll(c, events, pw)` installs
//                          and samples each local fd in one step; each remote
//                          fd's `dev->poll_snapshot` sends its snapshot. A
//                          pass that begins past the deadline passes pw = NULL
//                          and hooks nothing (ScanHooks).
//   SnapshotAnswer       ↔ the Dev's completion writing the slot (ANSWERED)
//                          and waking the poller; `poll_settle` waits for
//                          every snapshot of the pass, resending an UNSENT one
//                          and taking back one unanswered for the fixed bound.
//   SettleDeath          ↔ poll_settle's TSLEEP_INTR -> the sweep, which
//                          releases (flushes) every snapshot before anything.
//   EvaluateFirst /      ↔ the verdict after `poll_collect`: ready returns;
//   EvaluateWake /         nothing ready returns 0 once the CLOCK says the
//   EvaluateFinal          deadline has passed; otherwise the call arms.
//   Arm                  ↔ `poll_arm_remote`: `dev->poll_arm` per remote fd,
//                          hook first; an uncovered fd sets the retry timer.
//   TSleepCommit /       ↔ the park's `tsleep` (cond `poll_cond_any_flagged`,
//   Timeout / RetryWake    under the poller's rendez lock) against the
//                          earlier of the call's deadline and the retry timer.
//   Rearm / LoopCheck    ↔ after the park, whatever tsleep returned:
//                          `poll_unhook_all`, the loop's own die-check + stop
//                          park, then the next pass.
//   MakeReady(f)         ↔ a producer's `poll_waiter_list_wake` (devpipe,
//                          devsrv, ...; for a remote fd, the poll-pump
//                          kthread's walk after an arm's answer). Sets
//                          `pw->ready = true` AND signals `pw->rendez`.
//   Unregister sweep     ↔ the goto target before return: release every
//                          snapshot, THEN unhook every waiter.

#include <thylacine/poll.h>

#include <thylacine/cons.h>       // cons_diag_line -- the fail-safe's line
#include <thylacine/dev.h>
#include <thylacine/devsrv.h>      // srv_handle_poll — KObj_Srv dispatch
#include <thylacine/errno.h>       // T_E_INTR
#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/loom.h>       // loom_poll -- KObj_Loom .poll (KT-1.5)
#include <thylacine/notes.h>      // thread_die_pending
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>      // sched_yield_hint
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../arch/arm64/timer.h"   // timer_now_ns — deadline conversion

// =============================================================================
// Diagnostics.
// =============================================================================

static u64 g_poll_calls;
static u64 g_poll_slept;
static u64 g_poll_resleeps;

u64 poll_total_calls(void) {
    return __atomic_load_n(&g_poll_calls, __ATOMIC_RELAXED);
}

// g_poll_slept counts callers that ENTERED the slow path — incremented
// just before `tsleep`. The counter is "slow path entered" rather than
// "actually parked": if a producer races the first scan + register and
// flips a `pw->ready=true` between register-and-tsleep, tsleep's pre-sleep
// cond-check returns TSLEEP_AWOKEN without ever transitioning the thread
// to SLEEPING. The counter still increments — its semantic is "we
// committed to the slow path," useful for test assertions like
// "timeout=10ms with no immediate ready took the tsleep branch."
u64 poll_total_slept(void) {
    return __atomic_load_n(&g_poll_slept, __ATOMIC_RELAXED);
}

// Wakes whose re-sample found nothing asked-about ready, so the poller slept
// again. The witness that a wake for someone else's event does not end a poll.
u64 poll_total_resleeps(void) {
    return __atomic_load_n(&g_poll_resleeps, __ATOMIC_RELAXED);
}

static u64 g_poll_snap_failsafes;
static u64 g_poll_arm_retries;
static u64 g_poll_snap_bound_ns;    // 0 = POLL_SNAP_BOUND_NS; a test's bound is silent

u64 poll_total_snap_failsafes(void) {
    return __atomic_load_n(&g_poll_snap_failsafes, __ATOMIC_RELAXED);
}

u64 poll_total_arm_retries(void) {
    return __atomic_load_n(&g_poll_arm_retries, __ATOMIC_RELAXED);
}

void poll_test_set_snap_bound_ns(u64 bound_ns) {
    __atomic_store_n(&g_poll_snap_bound_ns, bound_ns, __ATOMIC_RELAXED);
}

bool poll_test_snap_bound_release(void) {
    return __atomic_exchange_n(&g_poll_snap_bound_ns, 0ull, __ATOMIC_RELAXED) != 0;
}

// Only the first few are printed: the first one already fails every gate, and a
// hung server would otherwise print one line per polled fd per second.
#define POLL_FAILSAFE_PRINTS 16u

static void poll_note_failsafe(u64 waited_ns, bool quiet) {
    u64 n = __atomic_add_fetch(&g_poll_snap_failsafes, 1u, __ATOMIC_RELAXED);
    if (quiet || n > POLL_FAILSAFE_PRINTS) return;
    struct cons_diag_line l;
    cons_diag_line_init(&l);
    cons_diag_line_puts(&l, "poll: FAILSAFE a readiness snapshot went unanswered for ");
    cons_diag_line_putdec(&l, waited_ns / 1000000u);
    cons_diag_line_puts(&l, " ms and was reported not ready (count=");
    cons_diag_line_putdec(&l, n);
    cons_diag_line_puts(&l, n == POLL_FAILSAFE_PRINTS ? "; further ones are counted only)\n"
                                                      : ")\n");
    cons_diag_line_emit(&l);
}


// =============================================================================
// poll_waiter + poll_waiter_list — the kernel-side hook mechanism.
// =============================================================================

void poll_waiter_init(struct poll_waiter *pw, struct Rendez *r) {
    if (!pw || !r) extinction("poll_waiter_init: NULL");
    pw->magic  = POLL_WAITER_MAGIC;
    pw->ready  = false;
    pw->rendez = r;
    pw->list   = NULL;
    pw->next   = NULL;
}

void poll_waiter_list_init(struct poll_waiter_list *l) {
    if (!l) extinction("poll_waiter_list_init: NULL");
    spin_lock_init(&l->lock);
    l->head = NULL;
}

// Every operation on a hook list takes its lock IRQSAVE. The list lock nests
// under Dev object locks that IRQ handlers take (g_cons.lock and
// g_cons_drain.lock, taken by the UART RX IRQ): a holder with IRQs on --
// console_mgr is a kthread -- could be interrupted by an IRQ spinning on the
// object lock that another CPU holds while it spins on this list lock, a
// deadlock through the IRQ edge (B-0 audit round 4 F3). A lock acquired while
// holding an IRQ-taken lock must itself be taken with IRQs masked, everywhere.
void poll_waiter_list_register(struct poll_waiter_list *l,
                               struct poll_waiter *pw) {
    if (!l || !pw)                            extinction("pw_register: NULL");
    if (pw->magic != POLL_WAITER_MAGIC)       extinction("pw_register: bad magic");
    if (pw->list != NULL)                     extinction("pw_register: double register");

    irq_state_t s = spin_lock_irqsave(&l->lock);
    pw->next = l->head;
    pw->list = l;
    l->head  = pw;
    spin_unlock_irqrestore(&l->lock, s);
}

void poll_waiter_list_unregister(struct poll_waiter *pw) {
    if (!pw) return;
    struct poll_waiter_list *l = pw->list;
    if (!l) return;   // already unregistered — idempotent no-op.

    irq_state_t s = spin_lock_irqsave(&l->lock);
    // Re-check under lock: a concurrent unregister on this pw isn't
    // possible (pw belongs to the calling poller), but the list could
    // have been modified by other pollers' register/unregister against
    // their own hooks. Walk for pw; remove if found.
    struct poll_waiter **slot = &l->head;
    while (*slot) {
        if (*slot == pw) {
            *slot    = pw->next;
            pw->next = NULL;
            pw->list = NULL;
            spin_unlock_irqrestore(&l->lock, s);
            return;
        }
        slot = &(*slot)->next;
    }
    // Not on the list — pw->list was non-NULL on entry but pw is gone.
    // That's a corruption: the unregister-on-NULL idempotency check
    // above already covered the legitimate "already gone" case.
    spin_unlock_irqrestore(&l->lock, s);
    extinction("pw_unregister: pw->list set but pw not on list (corruption)");
}

// Whether the list currently has no registered hooks. Under the list lock (a
// momentary snapshot). The dev9p_poll GC (net-6b-2b) calls this while holding
// g_dev9p_poll_lock to decide, atomically with the unlink, whether an outstanding
// readiness op is stranded (no poller cares) -- so the check + the unlink + the
// ps->op clear are one critical section vs a concurrent reuse that registers its
// hook BEFORE taking g_dev9p_poll_lock.
bool poll_waiter_list_empty(struct poll_waiter_list *l) {
    if (!l) return true;
    irq_state_t s = spin_lock_irqsave(&l->lock);
    bool empty = (l->head == NULL);
    spin_unlock_irqrestore(&l->lock, s);
    return empty;
}

void poll_waiter_list_wake(struct poll_waiter_list *l) {
    if (!l) return;

    irq_state_t s = spin_lock_irqsave(&l->lock);
    for (struct poll_waiter *pw = l->head; pw; pw = pw->next) {
        if (pw->magic != POLL_WAITER_MAGIC) {
            // The walker mid-iteration found a corrupted link. A
            // stack-lifetime'd hook leaked past its poll call's
            // return would dereference clobbered stack memory here.
            extinction("pw_wake: corrupted waiter magic (stale hook / UAF)");
        }
        // Order matters: write `ready` FIRST, then call `wakeup`.
        // wakeup acquires the rendez's lock — the release/acquire pair
        // makes pw->ready=true visible to the woken poller's cond
        // re-check (which runs under the same rendez lock).
        pw->ready = true;
        (void)wakeup(pw->rendez);
    }
    spin_unlock_irqrestore(&l->lock, s);
}

// =============================================================================
// SYS_POLL — the testable core.
// =============================================================================

// poll_cond — `tsleep`'s wait predicate. Returns 1 iff any of the
// poller's waiters has its `ready` flag set. Called under the poller's
// PRIVATE rendez lock (sleep's discipline); reads pw->ready without
// taking each fd's object lock — see <thylacine/poll.h>'s preamble for
// the release/acquire chain that makes the read sound.
struct poll_cond_arg {
    const struct poll_waiter *waiters;
    u64                       nfds;
};

static int poll_cond_any_flagged(void *arg) {
    const struct poll_cond_arg *a = (const struct poll_cond_arg *)arg;
    for (u64 i = 0; i < a->nfds; i++) {
        if (a->waiters[i].ready) return 1;
    }
    return 0;
}

// A cond that is never true, so a wait on it can only end on its deadline, a
// stop, or a death-interrupt: the zero-fd sleep at the bottom of this file,
// which is its only caller. There is nothing to be ready.
static int poll_never(void *arg) {
    (void)arg;
    return 0;
}

// Per-fd scan step. A LOCAL fd REGISTERS + SAMPLES: `dev->poll(c, events, pw)`
// installs the hook and samples readiness in one step under the object's lock
// (specs/poll.tla Register / Resample). A REMOTE fd -- a Dev with
// `.poll_snapshot` -- is sent a snapshot and hooks nothing (net_poll.tla Scan):
// its answer is collected after the settle, and its hook waits for the arm.
// `pw_or_null` is NULL on a pass that begins past the deadline, which hooks
// nothing (ScanHooks). Sets `pfd->revents`; a remote fd reads 0 until
// `poll_collect` fills it in.
//
// We deliberately do NOT require RIGHT_READ/RIGHT_WRITE: poll's
// semantics are "is this fd ready for the requested event", and a
// reader without RIGHT_READ can still observe POLLHUP/POLLERR (POSIX
// permits polling a write-only fd for POLLIN — revents=0).
static void poll_scan_one(struct Proc *p, struct pollfd *pfd, struct Spoor *pre,
                          struct poll_waiter *pw_or_null,
                          struct Handle *keep_out, struct poll_snap *snap) {
    // RW-2 2C-F1: `keep_out` receives the obj ref this scan must HOLD past the
    // sleep when it registers a waiter on an object's poll_list -- see the
    // retain decision below. Default: hold nothing (zeroed snapshot; handle_put
    // no-ops on it). The caller has already put whatever the slot held.
    if (keep_out) *keep_out = (struct Handle){0};
    // Every snapshot of the previous pass was released before its refs were
    // dropped; one still here would be answered into a slot we are reusing.
    if (snap->op) extinction("poll: a readiness snapshot outlived its pass");
    snap->revents = 0;
    __atomic_store_n(&snap->state, (u8)POLL_SNAP_NONE, __ATOMIC_RELAXED);
    snap->remote  = false;

    s16 revents = 0;
    if (pfd->fd < 0) {
        pfd->revents = POLLNVAL;
        return;
    }
    // #844: snapshot + hold the obj ref across the brief scan. dev->poll /
    // srv_handle_poll register a waiter but return promptly; the actual poll
    // sleep happens later in sys_poll_for_proc. handle_put before every return
    // EXCEPT when this scan registered a waiter on the object's poll_list or
    // sent a snapshot -- then the ref is RETAINED (transferred to keep_out).
    // A pre-resolved entry is polled through the same snapshot a table lookup
    // would give, so everything below -- retention included -- is unchanged.
    struct Handle hh;
    if (pre) {
        handle_snapshot_spoor(&hh, pre);
    } else if (handle_get(p, (hidx_t)pfd->fd, &hh) < 0) {
        pfd->revents = POLLNVAL;
        return;
    }

    switch (hh.kind) {
    case KOBJ_SPOOR: {
        struct Spoor *sp = (struct Spoor *)hh.obj;
        if (!sp || !sp->dev) {
            // Malformed Spoor handle (test path or wild slot). Symmetric
            // with the KOBJ_SRV NULL-obj path in `srv_handle_poll` —
            // POLLNVAL, not "always-ready," so a buggy caller polling a
            // NULL-obj fd doesn't spin observing fake readiness.
            revents = POLLNVAL;
        } else if (sp->dev->poll_snapshot) {
            // Readiness that lives in a server. The Dev either answers here
            // (a file with none to ask about) or sends the snapshot.
            sp->dev->poll_snapshot(sp, pfd->events, snap);
            revents = snap->remote ? 0 : (s16)snap->revents;
        } else if (sp->dev->poll) {
            revents = (s16)sp->dev->poll(sp, pfd->events, pw_or_null);
        } else {
            // Dev with NO .poll slot is POSIX-regular-file (read/write
            // never blocks). Return only the requested POLLIN/POLLOUT;
            // output-only POLLHUP/POLLERR/POLLNVAL are meaningless here.
            revents = (s16)(pfd->events & POLL_REQUESTABLE);
        }
        break;
    }
    case KOBJ_SRV:
        // The KObj_Srv flavor (listener SrvService vs client SrvConn) is
        // discriminated inside srv_handle_poll via the obj's magic.
        revents = (s16)srv_handle_poll(hh.obj, pfd->events, pw_or_null);
        break;
    case KOBJ_LOOM:
        // KT-1.5: a Loom ring folds into a poll(2) set (KObj_Loom.poll). The
        // keep_out retention below holds the loom_ref across the sleep once
        // loom_poll lists pw on l->cq_waiters (handle_put's loom_unref pairs it).
        // Meaningful on an SQPOLL ring, whose kthread posts CQEs without ENTER.
        revents = (s16)loom_poll((struct Loom *)hh.obj, pfd->events, pw_or_null);
        break;
    default:
        // Every other kobj kind (Burrow / Mmio / Irq / Dma / Interrupt /
        // Process / Thread) lacks readiness semantics in v1.0.
        revents = POLLNVAL;
        break;
    }
    pfd->revents = revents;

    // RW-2 2C-F1 (the multi-thread-Proc poll-lifetime UAF): if this scan
    // REGISTERED a waiter on the object's embedded poll_list (pw_or_null got
    // listed -> pw->list != NULL), the object must stay alive until we
    // unregister. The poll-hook precondition in <thylacine/poll.h> assumed a
    // single-thread-per-Proc poller; that lift landed at P6-pouch-threads, so a
    // SIBLING thread closing the last handle to this object would
    // spoor_clunk/srvconn_unref it -> free the embedded poll_list out from
    // under our still-listed stack waiter -> UAF at poll_waiter_list_unregister.
    // A remote fd is retained too: its snapshot is resent and released through
    // the Spoor, and its arm hooks onto the Spoor's list later in the pass --
    // and the release must come before the Spoor's close can clunk the fid.
    // Retain the obj ref (transfer hh to keep_out); sys_poll_for_proc drops it
    // AFTER the release + unregister sweep, when nothing references the object.
    // A scan that did neither (POLLNVAL, a no-.poll dev, a NULL-obj Spoor)
    // drops the transient ref now.
    if (keep_out && ((pw_or_null && pw_or_null->list != NULL) || snap->remote)) {
        *keep_out = hh;            // RETAIN -- released post-sweep by the caller
    } else {
        handle_put(&hh);
    }
}

// Take every hook off its list, THEN drop every retained object ref, then
// clear each hook (specs/poll.tla Rearm). The order is load-bearing (RW-2
// 2C-F1): a final handle_put may run an object's close hook against its
// embedded list, which must no longer hold one of ours. Off its list no
// producer can reach a hook, so the clear takes no lock, and a hook goes back
// on clear. Idempotent, so the return sweep runs it after a pass that already
// did. Every snapshot of the pass must be released before this runs.
static void poll_unhook_all(struct poll_waiter *waiters, struct Handle *held,
                            u64 nfds) {
    for (u64 i = 0; i < nfds; i++) poll_waiter_list_unregister(&waiters[i]);
    for (u64 i = 0; i < nfds; i++) handle_put(&held[i]);   // zeroes the slot
    for (u64 i = 0; i < nfds; i++) waiters[i].ready = false;
}

// Release every snapshot that still holds a request. A slot with no `op` holds
// nothing (never sent for want of memory, or already released), and its Spoor
// ref may already be gone, so it is skipped without touching the Dev.
static void poll_release_snaps(struct Handle *held, struct poll_snap *snaps,
                               u64 nfds) {
    for (u64 i = 0; i < nfds; i++) {
        if (!snaps[i].op) continue;
        struct Spoor *sp = (struct Spoor *)held[i].obj;
        sp->dev->poll_snapshot_release(sp, &snaps[i]);
    }
}

struct poll_settle_arg {
    const struct poll_snap *snaps;
    u64                     nfds;
};

// The settle's wait predicate: no snapshot of the pass is still out. Runs
// under the poller's rendez lock; an answer stores its state (RELEASE) before
// it wakes the rendez, so a wake is never missed between this check and the
// sleep. A local fd's flag does not end the settle -- it waits for the park.
static int poll_cond_settled(void *arg) {
    const struct poll_settle_arg *a = (const struct poll_settle_arg *)arg;
    for (u64 i = 0; i < a->nfds; i++) {
        u8 st = __atomic_load_n(&a->snaps[i].state, __ATOMIC_ACQUIRE);
        if (st == POLL_SNAP_SENT || st == POLL_SNAP_UNSENT) return 0;
    }
    return 1;
}

// SETTLE (specs/poll.tla MayDecide; net_poll.tla SnapshotReply /
// SnapshotFailSafe): wait until every snapshot the scan sent at `sent_ns` is
// answered. One a shortage kept off the wire is resent every
// POLL_SNAP_RESEND_NS. One still out a fixed bound after the scan is released
// -- flushed at its server -- and, unless its answer beat the release, reported
// not ready and counted. The call's own deadline plays no part: cutting the
// settle at it would report a healthy server's socket not-ready on a guess
// (net_poll buggy_settle_cut_by_deadline). Returns -1 on a death-interrupt
// (the caller sweeps: SettleDeath), else 0.
static int poll_settle(struct Rendez *r, struct pollfd *kfds, struct Handle *held,
                       struct poll_snap *snaps, u64 nfds, u64 sent_ns) {
    u64 bound = __atomic_load_n(&g_poll_snap_bound_ns, __ATOMIC_RELAXED);
    bool quiet = bound != 0;
    if (!quiet) bound = POLL_SNAP_BOUND_NS;
    u64 limit     = sent_ns + bound;
    u64 resend_at = sent_ns + POLL_SNAP_RESEND_NS;
    struct poll_settle_arg arg = { .snaps = snaps, .nfds = nfds };

    for (;;) {
        u64 now = timer_now_ns();
        bool resend = now >= resend_at;
        if (resend) resend_at = now + POLL_SNAP_RESEND_NS;
        bool out = false, unsent = false;
        for (u64 i = 0; i < nfds; i++) {
            struct poll_snap *s = &snaps[i];
            u8 st = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
            if (st != POLL_SNAP_SENT && st != POLL_SNAP_UNSENT) continue;
            struct Spoor *sp = (struct Spoor *)held[i].obj;
            if (now >= limit) {
                if (s->op) sp->dev->poll_snapshot_release(sp, s);
                if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != POLL_SNAP_ANSWERED) {
                    __atomic_store_n(&s->state, (u8)POLL_SNAP_EXPIRED, __ATOMIC_RELAXED);
                    poll_note_failsafe(now - sent_ns, quiet);
                }
                continue;
            }
            if (st == POLL_SNAP_UNSENT && resend) {
                sp->dev->poll_snapshot(sp, kfds[i].events, s);
                st = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
            }
            if (st == POLL_SNAP_UNSENT)    unsent = true;
            else if (st == POLL_SNAP_SENT) out = true;
        }
        if (!out && !unsent) return 0;
        u64 wake = (unsent && resend_at < limit) ? resend_at : limit;
        if (tsleep(r, poll_cond_settled, &arg, wake) == TSLEEP_INTR) return -1;
    }
}

// Take in the pass's answers: release each snapshot -- the release is the
// barrier after which no answer can touch the slot -- then read what it
// answered (an expired one reports not ready), and count the ready fds. Runs
// before any of the pass's object refs is dropped, so a close that follows
// never overtakes a read still out on the same fid.
static s64 poll_collect(struct pollfd *kfds, struct Handle *held,
                        struct poll_snap *snaps, u64 nfds) {
    poll_release_snaps(held, snaps, nfds);
    s64 ready_count = 0;
    for (u64 i = 0; i < nfds; i++) {
        struct poll_snap *s = &snaps[i];
        if (s->remote) {
            u8 st = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
            kfds[i].revents = (st == POLL_SNAP_ANSWERED) ? (s16)s->revents : 0;
        }
        if (kfds[i].revents != 0) ready_count++;
    }
    return ready_count;
}

// ARM (specs/poll.tla Arm; net_poll.tla PollerArm / PollerArmFails): the call
// will park, so each remote fd's hook goes on its list and its arm on the
// wire, hook first. Returns false when a shortage left any fd uncovered.
static bool poll_arm_remote(struct pollfd *kfds, struct poll_waiter *waiters,
                            struct Handle *held, const struct poll_snap *snaps,
                            u64 nfds) {
    bool covered = true;
    for (u64 i = 0; i < nfds; i++) {
        if (!snaps[i].remote) continue;
        struct Spoor *sp = (struct Spoor *)held[i].obj;
        if (!sp->dev->poll_arm(sp, kfds[i].events, &waiters[i])) covered = false;
    }
    return covered;
}

// The deadline has passed -- the CLOCK's answer, the only one the loop takes.
// Timeout 0 has always passed; a negative timeout never does.
static bool poll_expired(s32 timeout_ms, u64 deadline_ns) {
    if (timeout_ms == 0) return true;
    return deadline_ns != 0 && timer_now_ns() >= deadline_ns;
}

s64 sys_poll_for_proc(struct Proc *p, struct pollfd *kfds, u64 nfds,
                      s32 timeout_ms) {
    return sys_poll_for_proc_spoors(p, kfds, nfds, timeout_ms, NULL);
}

s64 sys_poll_for_proc_spoors(struct Proc *p, struct pollfd *kfds, u64 nfds,
                             s32 timeout_ms, struct Spoor *const *pre) {
    if (!p)                                   return -1;
    if (nfds == 0 || nfds > POLL_MAX_NFDS)    return -1;
    if (!kfds)                                return -1;

    __atomic_fetch_add(&g_poll_calls, 1u, __ATOMIC_RELAXED);

    // The poller's private rendez and the per-fd arrays -- stack-allocated for
    // the lifetime of this call. With nfds <= POLL_MAX_NFDS (64): waiters[]
    // ~2.5 KiB, held[] ~1.5 KiB, snaps[] ~1.5 KiB, plus the kfds[] array the
    // handler stack-allocates -- well within the kernel-thread stack. Sized to
    // POLL_MAX_NFDS, NOT PROC_HANDLE_MAX, so a larger fd table cannot grow this
    // frame past the kstack guard.
    struct Rendez r;
    rendez_init(&r);
    struct poll_waiter waiters[POLL_MAX_NFDS];
    // RW-2 2C-F1: per-fd retained obj refs. A registering or snapshotting scan
    // holds the obj ref past the settle and the sleep (zeroed = "nothing
    // held"); released after the release + unregister sweep.
    struct Handle held[POLL_MAX_NFDS];
    struct poll_snap snaps[POLL_MAX_NFDS];
    for (u64 i = 0; i < nfds; i++) {
        poll_waiter_init(&waiters[i], &r);
        held[i]  = (struct Handle){0};
        snaps[i] = (struct poll_snap){ .rendez = &r };
    }

    // The deadline, on the absolute timer_now_ns timebase (0 -- the "no
    // deadline" sentinel -- when timeout_ms < 0).
    u64 deadline_ns = 0;
    if (timeout_ms > 0) {
        u64 now = timer_now_ns();
        u64 add = (u64)timeout_ms * 1000000ull;
        u64 dl  = now + add;
        if (dl < now) dl = (u64)-1ll;   // u64 wrap → clamp to "very far future"
        if (dl == 0)  dl = 1;            // 0 reads as "no deadline"; nudge off
        deadline_ns = dl;
    }

    struct Thread *t = current_thread();
    struct poll_cond_arg cond_arg = { .waiters = waiters, .nfds = nfds };
    s64  ready_count = 0;
    bool parked      = false;

    for (;;) {
        // SCAN (Register / Resample). Local fds register + sample; remote fds
        // are sent their snapshots. A pass that begins past the deadline --
        // timeout 0, or the pass after the park timed out -- returns whatever
        // it finds, so it hooks nothing.
        bool expired    = poll_expired(timeout_ms, deadline_ns);
        bool any_remote = false;
        for (u64 i = 0; i < nfds; i++) {
            poll_scan_one(p, &kfds[i], pre ? pre[i] : NULL,
                          expired ? NULL : &waiters[i], &held[i], &snaps[i]);
            if (snaps[i].remote) any_remote = true;
        }

        // SETTLE, then take in the answers. The verdict waits for every
        // snapshot of the pass (poll buggy_verdict_before_settle): an fd ready
        // at once must not be reported not-ready because its server had not
        // answered yet, nor the one beside it left out of the count.
        if (any_remote &&
            poll_settle(&r, kfds, held, snaps, nfds, timer_now_ns()) < 0) {
            ready_count = 0;               // SettleDeath: the sweep flushes
            goto unregister_and_return;
        }
        ready_count = poll_collect(kfds, held, snaps, nfds);

        // VERDICT (EvaluateFirst / EvaluateWake). Ready returns. Nothing ready
        // returns 0 only at the deadline (ARCH 23.3; NoSpuriousZero) -- asked of
        // the clock NOW, so a deadline that lapsed during the settle ends the
        // call on this pass's answers, and a retry timer that ended the park
        // early (below) never does.
        if (ready_count > 0) break;

        // A caught note ends a pass that found nothing (ARCH 8.8.3), ahead of
        // the deadline: Linux's do_poll asks signal_pending before timed_out,
        // so a note pending at a lapsed or zero timeout is EINTR, not 0 (poll
        // NoZeroOverCaught). tsleep's own caught arm sits behind its cond test,
        // so a producer that keeps a flag set would keep a pause()-like poller
        // from ever reaching it (poll CaughtTerminates).
        if (thread_caught_note_unwinds(t)) {
            ready_count = -(s64)T_E_INTR;
            goto unregister_and_return;
        }
        if (poll_expired(timeout_ms, deadline_ns)) break;

        if (parked) {
            // Woken for nothing we asked about: this pass cost the CPU and
            // bought nothing, so let queued work run before parking again. The
            // INTERRUPT bound is not this file's to make (ARCH 8.12 runs the
            // syscall body unmasked); what remains is FAIRNESS to peers on this
            // CPU, load-bearing because interrupts-on is NOT preemption.
            __atomic_fetch_add(&g_poll_resleeps, 1u, __ATOMIC_RELAXED);
            (void)sched_yield_hint();
        }

        // ARM, then PARK against the SAME absolute deadline. A remote fd a
        // shortage left without an arm has nothing to wake us for its own
        // readiness, so the park is bounded by the retry timer (poll
        // buggy_no_retry); its expiry is a wake like any other.
        u64 park_dl = deadline_ns;
        if (any_remote && !poll_arm_remote(kfds, waiters, held, snaps, nfds)) {
            u64 retry = timer_now_ns() + POLL_ARM_RETRY_NS;
            if (park_dl == 0 || retry < park_dl) park_dl = retry;
            __atomic_fetch_add(&g_poll_arm_retries, 1u, __ATOMIC_RELAXED);
        }
        if (!parked) {
            __atomic_fetch_add(&g_poll_slept, 1u, __ATOMIC_RELAXED);
            parked = true;
        }
        int ts = tsleep_noteintr(&r, poll_cond_any_flagged, &cond_arg, park_dl);

        // #811 (ARCH §8.8.1): death-interrupted -> the Proc is group-
        // terminating. Skip the re-sample (the Thread dies at its EL0-return
        // die-check; the result is immaterial) and fall to the sweep, which is
        // REQUIRED -- waiters[] are stack-allocated and still listed.
        if (ts == TSLEEP_INTR) {
            ready_count = 0;
            goto unregister_and_return;
        }
        // A caught note unwound the park with no flag set (ARCH 8.8.3): EINTR,
        // through the same sweep. Never round again -- this thread holds the
        // note's claim, so the next park would unwind at once.
        if (ts == TSLEEP_NOTEINTR) {
            ready_count = -(s64)T_E_INTR;
            goto unregister_and_return;
        }

        // Whatever tsleep returned, go round: a flag is a HINT (the list it
        // sits on is walked for every event on the object, asked-about or not,
        // and a competing reader can drain what readied it before we look),
        // and TIMEDOUT may be the retry timer's rather than the call's (poll
        // buggy_retry_is_timeout) -- whether the call has timed out is the next
        // pass's `expired`, read off the clock. Every pass RE-REGISTERS: every
        // hook comes off its list (clear), then each local fd's .poll runs WITH
        // its hook again (Rearm -> Resample). An event before an fd's install is
        // seen by its sample; one after reaches the fresh hook. Re-registering,
        // not merely re-sampling, is what lets a Dev that chooses its list by
        // state re-choose it: the console files a frozen poller on its episode
        // list, and a hook left there after the episode ended never saw another
        // keystroke (cons_poll.tla BUGGY_NO_REREGISTER). It also re-resolves
        // the fd with its hook, so a closed fd reports POLLNVAL.
        poll_unhook_all(waiters, held, nfds);

        // tsleep's own die-check and stop detour sit BEHIND its cond test: a
        // producer that keeps a flag set in every re-sample window keeps every
        // tsleep returning AWOKEN without reaching either. So the loop makes
        // both itself, with no hook listed -- a parked poller is walked by no
        // producer for as long as the stop lasts (DeathTerminates /
        // StopHonoured; DEATH WINS: the park returns SLEEP_INTR on death).
        if (thread_die_pending(t)) {
            ready_count = 0;
            goto unregister_and_return;
        }
        if (t->proc && proc_stop_requested(t->proc) &&
            proc_stop_sleeper_park(t) == SLEEP_INTR) {
            ready_count = 0;
            goto unregister_and_return;
        }
    }

unregister_and_return:
    // Sweep: every exit path lands here (specs/poll.tla NoStaleHook,
    // NoSnapshotOutlivesCall). Snapshots released FIRST -- a death mid-settle
    // leaves some out, and their answers must not land in a frame that is gone
    // -- then hooks off, THEN refs dropped (poll_unhook_all); a pass that
    // already did these makes them no-ops. The magic is scribbled only AFTER
    // the unregister: a magic-0 hook still on a list would extinct a concurrent
    // producer's walk.
    poll_release_snaps(held, snaps, nfds);
    poll_unhook_all(waiters, held, nfds);
    for (u64 i = 0; i < nfds; i++) {
        waiters[i].magic = 0;   // defense-in-depth: scribble before stack pops
    }
    return ready_count;
}

s64 sys_poll_sleep_for(s32 timeout_ms) {
    // A zero-length sleep is not a trip through the scheduler; it only asks for
    // a pending note, below.
    if (timeout_ms != 0) {
        struct Rendez r;
        rendez_init(&r);

        // The same deadline arithmetic as the slow path above, and it must stay
        // the same: 0 is the "no deadline" sentinel, so a computed 0 (or a u64
        // wrap) has to be nudged off it or an intended wait becomes an infinite
        // one.
        u64 deadline_ns = 0;
        if (timeout_ms > 0) {
            u64 now = timer_now_ns();
            u64 add = (u64)timeout_ms * 1000000ull;
            u64 dl  = now + add;
            if (dl < now) dl = (u64)-1ll;
            if (dl == 0)  dl = 1;
            deadline_ns = dl;
        }

        // Nothing will ever signal `r`, so this returns TSLEEP_TIMEDOUT on the
        // deadline, TSLEEP_INTR if the Proc is being terminated (#811), or
        // TSLEEP_NOTEINTR for a caught note (ARCH 8.8.3), which is EINTR: pause()
        // -- ppoll with no fds -- returns once its handler ran. Death ends the
        // call at the EL0 return tail; its result is immaterial.
        int ts = tsleep_noteintr(&r, poll_never, NULL, deadline_ns);
        if (ts == TSLEEP_NOTEINTR) return -(s64)T_E_INTR;
        if (ts == TSLEEP_INTR)     return 0;
    }
    // The deadline passed, or there was none to wait for. tsleep asks the clock
    // before the note, Linux asks the signal first: a note pending now is EINTR.
    return thread_caught_note_unwinds(current_thread()) ? -(s64)T_E_INTR : 0;
}
