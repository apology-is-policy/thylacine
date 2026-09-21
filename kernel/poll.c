// poll — kernel-side mechanism + the SYS_POLL testable core.
//
// Per ARCHITECTURE.md §23.3 + specs/poll.tla. See <thylacine/poll.h>
// for the design preamble (single-waiter Rendez constraint; the
// register-then-observe discipline; per-list lock with object → list
// → rendez ordering; stack-allocated hook lifetime).
//
// SPEC-TO-CODE mapping (specs/poll.tla):
//
//   Register             ↔ the first scan in `sys_poll_for_proc`;
//                          `dev->poll(c, events, pw)` is the per-fd
//                          atomic "install + sample" step.
//   TSleepCommit         ↔ the post-scan flag check + `tsleep` call;
//                          `tsleep`'s cond `poll_cond_any_flagged` runs
//                          under the poller's rendez lock.
//   Rearm / LoopCheck /  ↔ each loop pass: `poll_unhook_all`, the loop's
//   Resample               own die-check + stop park, then the first
//                          scan's register+sample again.
//   MakeReady(f)         ↔ a producer's `poll_waiter_list_wake`
//                          (called from devpipe wakeup sites; devsrv
//                          at P5-poll-b). Sets `pw->ready = true` AND
//                          signals `pw->rendez` — both required.
//   AdvanceTime/Timeout  ↔ `tsleep`'s deadline path (specs/tsleep.tla
//                          composition).
//   BackoffCommit /      ↔ the noise backstop: a pass that finds the spin
//   BackoffTimeout /       budget spent with `nsleeps` unmoved unhooks every
//   SpinLapse              fd and sleeps on `poll_never` for POLL_BACKOFF_NS.
//   Unregister sweep     ↔ the goto target before return; sweeps every
//                          waiter via `poll_waiter_list_unregister`.

#include <thylacine/poll.h>

#include <thylacine/dev.h>
#include <thylacine/devsrv.h>      // srv_handle_poll — KObj_Srv dispatch
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
static u64 g_poll_backoffs;
static u64 g_poll_spin_budget_ns = POLL_SPIN_BUDGET_NS;

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

u64 poll_total_backoffs(void) {
    return __atomic_load_n(&g_poll_backoffs, __ATOMIC_RELAXED);
}

u64 poll_spin_budget_set_for_test(u64 ns) {
    return __atomic_exchange_n(&g_poll_spin_budget_ns, ns, __ATOMIC_RELAXED);
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
// stop, or a death-interrupt: the noise backstop's sleep, and the zero-fd
// sleep at the bottom of this file. There is nothing to be ready.
static int poll_never(void *arg) {
    (void)arg;
    return 0;
}

// Per-fd scan step: REGISTER + SAMPLE -- `dev->poll(c, events, pw)` installs
// the hook and samples readiness in one step under the object's lock. Every
// scan registers, the loop's re-arm passes included (specs/poll.tla Register /
// Resample). Sets `kfds[i].revents` and returns 1 iff the fd is "ready"
// (revents != 0).
//
// We deliberately do NOT require RIGHT_READ/RIGHT_WRITE: poll's
// semantics are "is this fd ready for the requested event", and a
// reader without RIGHT_READ can still observe POLLHUP/POLLERR (POSIX
// permits polling a write-only fd for POLLIN — revents=0).
static int poll_scan_one(struct Proc *p, struct pollfd *pfd,
                         struct poll_waiter *pw_or_null,
                         struct Handle *keep_out) {
    // RW-2 2C-F1: `keep_out` receives the obj ref this scan must HOLD past the
    // sleep when it registers a waiter on an object's poll_list -- see the
    // retain decision below. Default: hold nothing (zeroed snapshot; handle_put
    // no-ops on it). The caller has already put whatever the slot held.
    if (keep_out) *keep_out = (struct Handle){0};
    s16 revents = 0;
    if (pfd->fd < 0) {
        pfd->revents = POLLNVAL;
        return 1;
    }
    // #844: snapshot + hold the obj ref across the brief scan. dev->poll /
    // srv_handle_poll register a waiter but return promptly; the actual poll
    // sleep happens later in sys_poll_for_proc. handle_put before every return
    // EXCEPT when this scan registered a waiter on the object's poll_list --
    // then the ref is RETAINED (transferred to keep_out) so the object cannot
    // be freed out from under the still-listed stack waiter (RW-2 2C-F1 below).
    struct Handle hh;
    if (handle_get(p, (hidx_t)pfd->fd, &hh) < 0) {
        pfd->revents = POLLNVAL;
        return 1;
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
    // Retain the obj ref (transfer hh to keep_out); sys_poll_for_proc drops it
    // AFTER the unregister sweep, when no waiter references the list anymore.
    // A scan that did NOT register (POLLNVAL, a no-.poll dev, a NULL-obj Spoor)
    // drops the transient ref now.
    if (keep_out && pw_or_null && pw_or_null->list != NULL) {
        *keep_out = hh;            // RETAIN -- released post-sweep by the caller
    } else {
        handle_put(&hh);
    }
    return (revents != 0) ? 1 : 0;
}

// Take every hook off its list, THEN drop every retained object ref, then
// clear each hook (specs/poll.tla Rearm). The order is load-bearing (RW-2
// 2C-F1): a final handle_put may run an object's close hook against its
// embedded list, which must no longer hold one of ours. Off its list no
// producer can reach a hook, so the clear takes no lock, and a hook goes back
// on clear. Idempotent, so the return sweep runs it after a pass that already
// did.
static void poll_unhook_all(struct poll_waiter *waiters, struct Handle *held,
                            u64 nfds) {
    for (u64 i = 0; i < nfds; i++) poll_waiter_list_unregister(&waiters[i]);
    for (u64 i = 0; i < nfds; i++) handle_put(&held[i]);   // zeroes the slot
    for (u64 i = 0; i < nfds; i++) waiters[i].ready = false;
}

s64 sys_poll_for_proc(struct Proc *p, struct pollfd *kfds, u64 nfds,
                      s32 timeout_ms) {
    if (!p)                                   return -1;
    if (nfds == 0 || nfds > POLL_MAX_NFDS)    return -1;
    if (!kfds)                                return -1;

    __atomic_fetch_add(&g_poll_calls, 1u, __ATOMIC_RELAXED);

    // The poller's private rendez and the per-fd waiter array — stack-
    // allocated for the lifetime of this call. With nfds ≤ POLL_MAX_NFDS (64),
    // sizeof(poll_waiter) ≈ 32 B → ~2 KiB; plus the kfds[] array the handler
    // stack-allocates. Well within the kernel-thread stack. Sized to
    // POLL_MAX_NFDS, NOT PROC_HANDLE_MAX, so a larger fd table cannot grow
    // this frame past the kstack guard.
    struct Rendez r;
    rendez_init(&r);
    struct poll_waiter waiters[POLL_MAX_NFDS];
    // RW-2 2C-F1: per-fd retained obj refs. A registering scan holds the obj
    // ref past the sleep (zeroed = "nothing held"); released after the
    // unregister sweep. ~1.5 KiB on the kstack alongside waiters[] (~2 KiB).
    struct Handle held[POLL_MAX_NFDS];
    for (u64 i = 0; i < nfds; i++) {
        poll_waiter_init(&waiters[i], &r);
        held[i] = (struct Handle){0};
    }

    s64 ready_count = 0;

    // First scan: REGISTER + SAMPLE per fd. This is specs/poll.tla's
    // `Register` — `dev->poll(c, events, &waiters[i])` installs the
    // hook AND samples readiness under the object's lock in one step.
    // No readiness event between sample and sleep can slip past a
    // not-yet-installed hook.
    for (u64 i = 0; i < nfds; i++) {
        ready_count += poll_scan_one(p, &kfds[i], &waiters[i], &held[i]);
    }

    // Fast path: any fd ready at the first scan → unregister all,
    // return. specs/poll.tla EvaluateFirst: seen → done_ready.
    if (ready_count > 0) {
        goto unregister_and_return;
    }
    // Non-blocking poll (timeout_ms == 0) → unregister all, return 0.
    if (timeout_ms == 0) {
        goto unregister_and_return;
    }

    // Slow path: tsleep on the poller's private rendez. cond walks the
    // waiters; any pw->ready set returns true. Deadline is the timeout
    // converted to the absolute timer_now_ns timebase (or 0 — the
    // "no deadline" sentinel — when timeout_ms < 0).
    u64 deadline_ns = 0;
    if (timeout_ms > 0) {
        u64 now = timer_now_ns();
        u64 add = (u64)timeout_ms * 1000000ull;
        u64 dl  = now + add;
        if (dl < now) dl = (u64)-1ll;   // u64 wrap → clamp to "very far future"
        if (dl == 0)  dl = 1;            // 0 reads as "no deadline"; nudge off
        deadline_ns = dl;
    }

    __atomic_fetch_add(&g_poll_slept, 1u, __ATOMIC_RELAXED);
    struct Thread *t = current_thread();
    struct poll_cond_arg cond_arg = { .waiters = waiters, .nfds = nfds };

    // The noise backstop's clock (specs/poll.tla SpinLapse): when the current
    // run of passes began, and the thread's park count then. nsleeps moves only
    // when this thread was switched out SLEEPING -- its CPU went to other work
    // or idle, where interrupts are taken -- so a pass that finds it moved
    // starts a new run. A yield does not count: two noise pollers yielding to
    // each other keep their CPU IRQ-masked between them.
    u64  spin_from   = timer_now_ns();
    u64  spin_sleeps = __atomic_load_n(&t->nsleeps, __ATOMIC_RELAXED);
    bool backoff     = false;
    for (;;) {
        // The backoff (BackoffCommit) sleeps with every hook already off: on a
        // cond nothing makes true, for POLL_BACKOFF_NS, capped at the poll's
        // own deadline -- at which point its TIMEDOUT is the poll's timeout.
        u64 sleep_dl = deadline_ns;
        int ts;
        if (backoff) {
            u64 bdl = timer_now_ns() + POLL_BACKOFF_NS;
            if (deadline_ns == 0 || bdl < deadline_ns) sleep_dl = bdl;
            ts = tsleep(&r, poll_never, NULL, sleep_dl);
        } else {
            ts = tsleep(&r, poll_cond_any_flagged, &cond_arg, sleep_dl);
        }

        // #811 (ARCH §8.8.1): death-interrupted -> the Proc is group-
        // terminating. Skip the re-sample (the Thread dies at its EL0-return
        // die-check; the result is immaterial) and fall to the sweep, which is
        // REQUIRED -- waiters[] are stack-allocated and, unless this was the
        // backoff's sleep, still listed.
        if (ts == TSLEEP_INTR) {
            ready_count = 0;
            goto unregister_and_return;
        }

        // A flag is a HINT: the list it sits on is walked for every event on
        // the object, asked-about or not, and a competing reader can drain
        // what readied it before we look. So every pass RE-REGISTERS: every
        // hook comes off its list (clear), then each fd's .poll runs WITH its
        // hook again, the first scan's install-and-sample (specs/poll.tla
        // Rearm -> Resample). An event before an fd's install is seen by its
        // sample; one after reaches the fresh hook. Re-registering, not merely
        // re-sampling, is what lets a Dev that chooses its list by state
        // re-choose it: the console files a frozen poller on its episode list,
        // and a hook left there after the episode ended never saw another
        // keystroke (cons_poll.tla BUGGY_NO_REREGISTER). It also re-resolves
        // the fd with its hook, so a closed fd reports POLLNVAL.
        poll_unhook_all(waiters, held, nfds);

        // tsleep's own die-check and stop detour sit BEHIND its cond test: a
        // producer that keeps a flag set in every re-sample window keeps every
        // tsleep returning AWOKEN without reaching either. So the loop makes
        // both itself, with no hook listed -- a parked poller is walked by no
        // producer for as long as the stop lasts (DeathTerminates /
        // StopHonoured; DEATH WINS: the park returns SLEEP_INTR on death). The
        // backoff's tsleep would reach both too, a budget later; these keep
        // death and a stop prompt.
        if (thread_die_pending(t)) {
            ready_count = 0;
            goto unregister_and_return;
        }
        if (t->proc && proc_stop_requested(t->proc) &&
            proc_stop_sleeper_park(t) == SLEEP_INTR) {
            ready_count = 0;
            goto unregister_and_return;
        }

        ready_count = 0;
        for (u64 i = 0; i < nfds; i++) {
            ready_count += poll_scan_one(p, &kfds[i], &waiters[i], &held[i]);
        }
        if (ready_count > 0) break;
        if (ts == TSLEEP_TIMEDOUT && sleep_dl == deadline_ns) break;

        // Woken for nothing we asked about: sleep AGAIN, against the SAME
        // absolute deadline. poll returns 0 only at its deadline (ARCH 23.3;
        // NoSpuriousZero) -- until 2026-09-21 this fell through and returned 0,
        // so a timed poll reported a timeout the moment a second reader won
        // the bytes and poll(-1) returned 0 at all. The explicit test bounds
        // the loop: tsleep prefers a set flag to a passed deadline, so a
        // producer that never stops walking a list would otherwise hold us
        // here past the timeout (PollTerminates).
        u64 now = timer_now_ns();
        if (deadline_ns != 0 && now >= deadline_ns) break;
        __atomic_fetch_add(&g_poll_resleeps, 1u, __ATOMIC_RELAXED);

        // The backstop (specs/poll.tla SpinBounded). A noise pass is IRQ-masked
        // from end to end -- syscalls run so -- and a producer can supply one
        // after another forever, so no deadline bounds how long this CPU goes
        // without taking an interrupt: poll(-1) has none, and a producer can
        // hold a poll with ten seconds left for all ten. Once a budget has
        // passed with no real sleep in it, take every hook off and back off.
        u64 sleeps = __atomic_load_n(&t->nsleeps, __ATOMIC_RELAXED);
        if (sleeps != spin_sleeps) {
            spin_sleeps = sleeps;
            spin_from   = now;
        }
        backoff = now - spin_from >=
                  __atomic_load_n(&g_poll_spin_budget_ns, __ATOMIC_RELAXED);
        if (backoff) {
            poll_unhook_all(waiters, held, nfds);
            __atomic_fetch_add(&g_poll_backoffs, 1u, __ATOMIC_RELAXED);
        } else {
            // A noise pass cost this CPU a full pass and bought nothing; let
            // queued work run before the next one, or a producer that keeps
            // walking a list keeps every other thread on this CPU waiting out
            // our budget.
            (void)sched_yield_hint();
        }
    }

unregister_and_return:
    // Sweep: every exit path lands here (specs/poll.tla NoStaleHook). Hooks
    // off, THEN refs dropped (poll_unhook_all); a pass that already unhooked
    // makes the first two steps no-ops. The magic is scribbled only AFTER the
    // unregister: a magic-0 hook still on a list would extinct a concurrent
    // producer's walk.
    poll_unhook_all(waiters, held, nfds);
    for (u64 i = 0; i < nfds; i++) {
        waiters[i].magic = 0;   // defense-in-depth: scribble before stack pops
    }
    return ready_count;
}

s64 sys_poll_sleep_for(s32 timeout_ms) {
    // A zero-length sleep is a no-op, not a trip through the scheduler.
    if (timeout_ms == 0) return 0;

    struct Rendez r;
    rendez_init(&r);

    // The same deadline arithmetic as the slow path above, and it must stay the
    // same: 0 is the "no deadline" sentinel, so a computed 0 (or a u64 wrap) has
    // to be nudged off it or an intended wait becomes an infinite one.
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
    // deadline, or TSLEEP_INTR if the Proc is being terminated (#811). Both are
    // "the wait is over"; the death case unwinds at the EL0 return tail, so
    // there is nothing to report differently here.
    (void)tsleep(&r, poll_never, NULL, deadline_ns);
    return 0;
}
