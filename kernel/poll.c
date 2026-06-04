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
//   CommitOrSleep        ↔ the post-scan flag check + `tsleep` call;
//                          `tsleep`'s cond `poll_cond_any_flagged` runs
//                          under the poller's rendez lock.
//   MakeReady(f)         ↔ a producer's `poll_waiter_list_wake`
//                          (called from devpipe wakeup sites; devsrv
//                          at P5-poll-b). Sets `pw->ready = true` AND
//                          signals `pw->rendez` — both required.
//   AdvanceTime/Timeout  ↔ `tsleep`'s deadline path (specs/tsleep.tla
//                          composition).
//   Unregister sweep     ↔ the goto target before return; sweeps every
//                          waiter via `poll_waiter_list_unregister`.

#include <thylacine/poll.h>

#include <thylacine/dev.h>
#include <thylacine/devsrv.h>      // srv_handle_poll — KObj_Srv dispatch
#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

#include "../arch/arm64/timer.h"   // timer_now_ns — deadline conversion

// =============================================================================
// Diagnostics.
// =============================================================================

static u64 g_poll_calls;
static u64 g_poll_slept;

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

void poll_waiter_list_register(struct poll_waiter_list *l,
                               struct poll_waiter *pw) {
    if (!l || !pw)                            extinction("pw_register: NULL");
    if (pw->magic != POLL_WAITER_MAGIC)       extinction("pw_register: bad magic");
    if (pw->list != NULL)                     extinction("pw_register: double register");

    spin_lock(&l->lock);
    pw->next = l->head;
    pw->list = l;
    l->head  = pw;
    spin_unlock(&l->lock);
}

void poll_waiter_list_unregister(struct poll_waiter *pw) {
    if (!pw) return;
    struct poll_waiter_list *l = pw->list;
    if (!l) return;   // already unregistered — idempotent no-op.

    spin_lock(&l->lock);
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
            spin_unlock(&l->lock);
            return;
        }
        slot = &(*slot)->next;
    }
    // Not on the list — pw->list was non-NULL on entry but pw is gone.
    // That's a corruption: the unregister-on-NULL idempotency check
    // above already covered the legitimate "already gone" case.
    spin_unlock(&l->lock);
    extinction("pw_unregister: pw->list set but pw not on list (corruption)");
}

void poll_waiter_list_wake(struct poll_waiter_list *l) {
    if (!l) return;

    spin_lock(&l->lock);
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
    spin_unlock(&l->lock);
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

// Per-fd scan step: either REGISTER + SAMPLE (pw != NULL on first
// scan) or SAMPLE-ONLY (pw == NULL on post-wake re-scan and on
// timeout=0 fast path). Sets `kfds[i].revents` and returns 1 iff the
// fd is "ready" (revents != 0).
//
// We deliberately do NOT require RIGHT_READ/RIGHT_WRITE: poll's
// semantics are "is this fd ready for the requested event", and a
// reader without RIGHT_READ can still observe POLLHUP/POLLERR (POSIX
// permits polling a write-only fd for POLLIN — revents=0).
static int poll_scan_one(struct Proc *p, struct pollfd *pfd,
                         struct poll_waiter *pw_or_null) {
    s16 revents = 0;
    if (pfd->fd < 0) {
        pfd->revents = POLLNVAL;
        return 1;
    }
    // #844: snapshot + hold the obj ref across the brief scan. dev->poll /
    // srv_handle_poll register a waiter but return promptly; the actual poll
    // sleep happens later in sys_poll_for_proc (the poll-hook list lifetime is
    // poll's own concern, unchanged here). handle_put before every return.
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
    default:
        // Every other kobj kind (Burrow / Mmio / Irq / Dma / Interrupt /
        // Process / Thread) lacks readiness semantics in v1.0.
        revents = POLLNVAL;
        break;
    }
    pfd->revents = revents;
    handle_put(&hh);
    return (revents != 0) ? 1 : 0;
}

s64 sys_poll_for_proc(struct Proc *p, struct pollfd *kfds, u64 nfds,
                      s32 timeout_ms) {
    if (!p)                                   return -1;
    if (nfds == 0 || nfds > PROC_HANDLE_MAX)  return -1;
    if (!kfds)                                return -1;

    __atomic_fetch_add(&g_poll_calls, 1u, __ATOMIC_RELAXED);

    // The poller's private rendez and the per-fd waiter array — stack-
    // allocated for the lifetime of this call. With nfds ≤ 64,
    // sizeof(poll_waiter) ≈ 32 B → ~2 KiB; plus the kfds[] array the
    // handler stack-allocates. Well within the kernel-thread stack.
    struct Rendez r;
    rendez_init(&r);
    struct poll_waiter waiters[PROC_HANDLE_MAX];
    for (u64 i = 0; i < nfds; i++) {
        poll_waiter_init(&waiters[i], &r);
    }

    s64 ready_count = 0;

    // First scan: REGISTER + SAMPLE per fd. This is specs/poll.tla's
    // `Register` — `dev->poll(c, events, &waiters[i])` installs the
    // hook AND samples readiness under the object's lock in one step.
    // No readiness event between sample and sleep can slip past a
    // not-yet-installed hook.
    for (u64 i = 0; i < nfds; i++) {
        ready_count += poll_scan_one(p, &kfds[i], &waiters[i]);
    }

    // Fast path: any fd ready at the first scan → unregister all,
    // return. specs/poll.tla CommitOrSleep: anyFlagged → done_ready.
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
    struct poll_cond_arg cond_arg = { .waiters = waiters, .nfds = nfds };
    int ts = tsleep(&r, poll_cond_any_flagged, &cond_arg, deadline_ns);

    // #811 (ARCH §8.8.1): death-interrupted -> the Proc is group-terminating.
    // Skip the re-sample (the Thread dies at its EL0-return die-check; the
    // result is immaterial) and fall to the unregister sweep, which is
    // REQUIRED -- waiters[] are stack-allocated and still listed on each fd's
    // poll_list; returning without unregistering would dangle them.
    if (ts == TSLEEP_INTR) {
        ready_count = 0;
        goto unregister_and_return;
    }

    // Post-wake re-sample: tsleep returned either TSLEEP_AWOKEN (some
    // pw->ready was set) or TSLEEP_TIMEDOUT (deadline lapsed with no
    // ready flag). In either case, sample each fd's CURRENT revents
    // via `dev->poll(c, events, NULL)` — sample-only, no list op. The
    // sample under each fd's object lock observes any producer state
    // change happens-before via the object lock's release/acquire chain.
    ready_count = 0;
    for (u64 i = 0; i < nfds; i++) {
        ready_count += poll_scan_one(p, &kfds[i], NULL);
    }

unregister_and_return:
    // Sweep: unregister every still-listed hook. Idempotent for any
    // hook that wasn't registered (e.g., POLLNVAL'd fds where
    // poll_scan_one returned early). specs/poll.tla NoStaleHook.
    //
    // Order is load-bearing: unregister FIRST (removes pw from the list
    // under list->lock; after release no producer walk can find pw),
    // THEN scribble magic = 0. Reversing would expose a magic=0 hook
    // to a concurrent producer walker that holds list->lock — the
    // wake's magic check would extinct.
    for (u64 i = 0; i < nfds; i++) {
        poll_waiter_list_unregister(&waiters[i]);
        waiters[i].magic = 0;   // defense-in-depth: scribble before stack pops
    }
    return ready_count;
}
