// poll — the multi-fd wait/wake primitive (P5-poll-a).
//
// Per ARCHITECTURE.md §23.3 + specs/poll.tla. `poll(fds, nfds,
// timeout_ms)` parks the caller until at least one of N fds is ready,
// or a timeout elapses. Thylacine has no separate fd layer — an `fd` is
// a handle index (hidx_t) — and a thread can wait on only ONE `Rendez`
// (single-waiter; rendez.h extincts on a second). poll therefore does
// NOT make `Rendez` multi-waiter: the poller sleeps on its OWN private
// `Rendez` via `tsleep`, and registers a lightweight `struct
// poll_waiter` hook on each polled object's poll-hook list.
//
// LOAD-BEARING DISCIPLINE
//
//   REGISTER-THEN-OBSERVE. `dev->poll(spoor, events, pw)` installs the
//   hook AND samples readiness in ONE locked step under the object's
//   own lock. No readiness event between the sample and the sleep can
//   reach a still-empty hook list. specs/poll.tla `Register` ↔
//   `dev->poll(c, events, pw)` is the binding spec action.
//
//   Producer side: every existing wakeup site (the existing
//   `wakeup(&r->X)` in devpipe; future devsrv) ALSO walks the object's
//   poll-hook list, sets each registered `poll_waiter`'s `ready` flag,
//   and signals that poller's private `Rendez`. specs/poll.tla
//   `MakeReady(f)` ↔ `poll_waiter_list_wake`.
//
// HOOK LIFETIME — STACK-ALLOCATED FOR ONE poll CALL
//
//   The hook is stack-allocated in `sys_poll_for_proc` — one slot per
//   pollfd, `nfds ≤ PROC_HANDLE_MAX = 64`. The hook MUST be
//   unregistered before the poll returns; a leftover hook is a
//   dangling stack pointer the next readiness event will walk.
//   specs/poll.tla `NoStaleHook` is the invariant.
//
// PER-OBJECT LIST OWNS ITS OWN LOCK
//
//   `struct poll_waiter_list` carries its own spinlock — separate from
//   the polled object's lock. The Dev's `.poll` implementation takes
//   the object lock for the readiness sample, then calls
//   `poll_waiter_list_register` (which takes the list lock internally)
//   to install the hook ATOMICALLY with the sample under the object
//   lock. The producer's wakeup site mutates readiness under the
//   object lock; after releasing it the site calls
//   `poll_waiter_list_wake` (which takes the list lock internally) —
//   any concurrent register's "sample" either happens-before the
//   producer's mutation (and sees the un-mutated state, OK because the
//   producer's wake will then find the hook and set ready) or after
//   (and the sample sees the mutation directly).
//
//   `poll_waiter_list_unregister(pw)` takes ONLY the list lock — no
//   object lock. Lock order globally:
//     object → list → g_timerwait → rendez → cpu_sched
//   The list lock is non-irqsave (held with IRQs ON during the wake
//   walk); the wake's inner `wakeup(pw->rendez)` enters wakeup which
//   takes `g_timerwait.lock` (irqsave) then the `rendez.lock` then the
//   per-CPU `cs->lock` (in ready()). No IRQ handler enters
//   `poll_list.lock`. Unregister takes only list, so no inversion. The
//   hook stores a back-pointer `pw->list` set at register time so the
//   kernel-side unregister sweep knows which list each hook is on
//   without consulting the Dev.
//
// REGISTERED-OBJECT LIFETIME (multi-thread-Proc safe -- RW-2 2C-F1)
//
//   The dangerous window is not merely "between the two scans" -- it is
//   the entire time a stack waiter stays listed on an object's embedded
//   `poll_waiter_list` (`r->poll_list` in a pipe ring, `cn->poll_list`
//   in a SrvConn): the first scan REGISTERS, the poller SLEEPS, and the
//   waiter remains listed across the whole sleep. Multi-thread Procs
//   landed at P6-pouch-threads, so a SIBLING thread sharing the handle
//   table can `handle_close` the last handle to that object DURING the
//   sleep -> `spoor_clunk` / `srvconn_unref` frees the object and its
//   embedded list, leaving the still-listed stack waiter's `pw->list`
//   dangling -> `poll_waiter_list_unregister` would spin_lock freed
//   memory (and `poll_waiter_list_wake` would walk a freed list).
//
//   FIX (applied): `poll_scan_one`'s register scan RETAINS the
//   `handle_get` obj ref (the `struct Handle` snapshot) whenever it
//   actually registers a waiter (`pw->list != NULL`), instead of
//   dropping it before the sleep. `sys_poll_for_proc` drops every
//   retained ref AFTER the unregister sweep.
//
//   The two REAL registering paths are both KOBJ_SPOOR, and the retained
//   Spoor handle ref keeps the object alive TRANSITIVELY:
//     - pipe ring: `devpipe_close` drops the ring ref only at the Spoor's
//       last `spoor_clunk`, so a live Spoor handle ref defers the free of
//       `r->poll_list`.
//     - devsrv connection Spoor (the live corvus `/srv` path, a KOBJ_SPOOR
//       with a `SRV_CONN_MAGIC` aux): `devsrv_close` -> `srvconn_unref`
//       likewise runs only at the Spoor's last clunk, deferring the free of
//       `cn->poll_list`.
//
//   CAVEAT -- the KObj_Srv LISTENER retain is INERT (RW-2 R2-poll F1). The
//   only KObj_Srv path that registers a poll waiter is a SrvService listener
//   (`svc_listener_poll` -> `svc->poll_list`); but `handle_acquire_obj` /
//   `handle_release_obj` are NO-OPS for KObj_Srv, so the retained `held[]`
//   entry holds no ref to the SrvService or its registry. Listener-poll
//   lifetime is safe ONLY because the sole registry today is the immortal
//   boot registry (`g_boot_srv_registry`; SrvService entries are tombstoned,
//   never freed). A mortal per-session registry (A-5b / #827, see
//   `kernel/devsrv.c` `srv_registry_unref`'s `kfree(reg)`) reintroduces the
//   round-1 UAF on the listener-poll path: it MUST then take a real
//   `srv_registry_ref` at register and drop it post-sweep (or thread a
//   registry ref through `held[]`). Tracked.
//
// THE Dev.poll vtable op (declared in <thylacine/dev.h>)
//
//   short (*poll)(struct Spoor *c, short events, struct poll_waiter *pw);
//
//   Returns the currently-ready `revents` (POLLIN/POLLOUT subset of
//   `events`, plus output-only POLLERR/POLLHUP if the device sees an
//   error / hangup). If `pw` is non-NULL, atomically registers `pw` on
//   the object's hook list under the object's lock. If `pw` is NULL,
//   the call is sample-only (used by the post-wake re-scan and the
//   timeout=0 non-blocking probe).
//
//   A NULL `Dev.poll` slot means the fd is always ready for the
//   requested events — the POSIX-correct answer for a regular file, so
//   only objects with genuine readiness state (devpipe at v1.0; devsrv
//   at P5-poll-b) implement a real `.poll`.

#ifndef THYLACINE_POLL_H
#define THYLACINE_POLL_H

#include <thylacine/spinlock.h>
#include <thylacine/types.h>

struct Rendez;
struct Proc;

// =============================================================================
// User-facing ABI: struct pollfd + event constants.
// =============================================================================
//
// The userspace ABI of SYS_POLL (syscall 29). Linux-shaped so the
// future musl shim is a no-op — pollfd has the same layout and event
// values as Linux's <poll.h>.

struct pollfd {
    s32 fd;            // a handle index (hidx_t); negative ⇒ POLLNVAL
    s16 events;        // requested events bitmask
    s16 revents;       // returned events bitmask (kernel-filled)
};

_Static_assert(sizeof(struct pollfd) == 8,
               "struct pollfd is a SYS_POLL ABI type — pinned at 8 bytes");
_Static_assert(__builtin_offsetof(struct pollfd, fd) == 0,
               "pollfd.fd at ABI offset 0");
_Static_assert(__builtin_offsetof(struct pollfd, events) == 4,
               "pollfd.events at ABI offset 4");
_Static_assert(__builtin_offsetof(struct pollfd, revents) == 6,
               "pollfd.revents at ABI offset 6");

// Event bits — Linux values, the future musl shim is a no-op.
#define POLLIN     0x001    // data may be read without blocking
#define POLLOUT    0x004    // writing will not block
#define POLLERR    0x008    // error condition (output-only)
#define POLLHUP    0x010    // hang up — peer closed (output-only)
#define POLLNVAL   0x020    // fd not open / invalid handle (output-only)

// The subset of bits a caller may request in `events`. POLLERR /
// POLLHUP / POLLNVAL are output-only — the kernel sets them regardless
// of `events`. POLLIN / POLLOUT are gated by request.
#define POLL_REQUESTABLE     (POLLIN | POLLOUT)
#define POLL_OUTPUT_ONLY     (POLLERR | POLLHUP | POLLNVAL)

// =============================================================================
// kernel-internal poll_waiter hook + per-object hook list.
// =============================================================================

struct poll_waiter_list;

// A poll_waiter is the kernel-side hook a polling thread installs on a
// pollable object's hook list. The poller stack-allocates one per fd.
// The producer (a wakeup site) walks the object's list, sets `ready`,
// and signals `rendez` to wake the sleeping poller.
//
// Lifetime: stack-allocated in `sys_poll_for_proc`; valid only for the
// duration of one poll call. MUST be unregistered before the poll
// returns (specs/poll.tla NoStaleHook).
//
// `magic` is a defense-in-depth check: a walk that hits a non-magic
// link is a use-after-free of a leftover hook (or a memory
// corruption). `extinction`-asserted by `poll_waiter_list_wake`.
//
// `list` is set by `poll_waiter_list_register` and cleared by
// `poll_waiter_list_unregister`. A non-NULL `list` means "this hook is
// currently on `*list`"; the kernel-side unregister sweep uses this
// back-pointer to find each hook's home without a Dev vtable op.
struct poll_waiter {
    u32                       magic;   // POLL_WAITER_MAGIC
    bool                      ready;   // set under list->lock by a producer
    struct Rendez            *rendez;  // back-ref to the poller's private Rendez
    struct poll_waiter_list  *list;    // non-NULL while listed; NULL otherwise
    struct poll_waiter       *next;    // singly-linked; valid only while listed
};

#define POLL_WAITER_MAGIC  0x504F4C57u   // "POLW" little-endian

// A pollable object embeds one of these — `struct pipe_ring` at v1.0,
// `struct SrvService` + `struct SrvConn` at P5-poll-b. The list lock
// is internal: register / unregister / wake all take it. The polled
// object's existing lock serializes readiness mutations; the list lock
// serializes hook-list mutations. They compose by lock order object →
// list — see this header's preamble.
struct poll_waiter_list {
    spin_lock_t         lock;
    struct poll_waiter *head;     // singly-linked; NULL when empty
};

// Initialize a poll_waiter for one poll call. Sets `magic`, `rendez`,
// clears `ready`, `list`, `next`. Does NOT touch any list.
void poll_waiter_init(struct poll_waiter *pw, struct Rendez *r);

// Initialize a per-object hook list. Idempotent; safe at registration
// boot order. Static init via `POLL_WAITER_LIST_INIT` works for
// file-scope or struct-embedded lists when an explicit zero-init isn't
// otherwise guaranteed.
void poll_waiter_list_init(struct poll_waiter_list *l);
#define POLL_WAITER_LIST_INIT  { SPIN_LOCK_INIT, NULL }

// Register `pw` on `l`. Takes `l->lock` internally. The caller
// (typically a Dev's `.poll`) holds the object's lock when calling so
// the install is atomic with the same call's readiness sample under
// the object lock — this is the register-then-observe step.
//
// Extincts on: `pw->magic` corrupted, `pw->list != NULL` (a double-
// register — the caller missed an unregister).
void poll_waiter_list_register(struct poll_waiter_list *l,
                               struct poll_waiter *pw);

// Unregister `pw` from its list. Takes `pw->list->lock` internally;
// requires NO outer lock. Idempotent: a hook that is already
// unregistered (`pw->list == NULL`) is a no-op. Used by
// `sys_poll_for_proc` on return.
void poll_waiter_list_unregister(struct poll_waiter *pw);

// Wake every registered poller: walk `l` (under `l->lock`), set each
// hook's `ready` flag, then signal each hook's `rendez`. Called from
// the producer's existing wakeup site UNDER the object's lock — so the
// readiness change the producer just made is visible to any concurrent
// register's sample (which also runs under the object's lock).
//
// `wakeup()` takes the rendez's own lock; the lock chain (object →
// list → rendez) is acyclic. Idempotent: walking an empty list is a
// no-op.
//
// Extincts on a corrupted `pw->magic` mid-walk (UAF on a stale hook).
void poll_waiter_list_wake(struct poll_waiter_list *l);

// Whether `l` has no registered hooks (a momentary snapshot under l->lock). Used
// by the dev9p_poll GC (net-6b-2b) to detect a stranded readiness op (no poller
// cares) while holding g_dev9p_poll_lock -- so the check is atomic with the
// op unlink vs a concurrent poller that registers its hook before reusing the op.
bool poll_waiter_list_empty(struct poll_waiter_list *l);

// =============================================================================
// The SYS_POLL testable core. The user-VA wrapper sys_poll_handler
// lives in kernel/syscall.c.
// =============================================================================

// sys_poll_for_proc — the kernel-side `poll(fds, nfds, timeout_ms)`.
// Operates on a kernel-side `kfds[]` array (the handler copies user-VA
// in, then back). Returns:
//   ≥ 0 — number of pollfds with revents != 0
//   -1  — error (nfds out of range; p NULL).
//
// `timeout_ms`:
//   < 0 — block indefinitely (no deadline).
//   = 0 — return immediately after the first scan (a non-blocking
//         readiness probe). No tsleep.
//   > 0 — block for at most `timeout_ms` milliseconds.
//
// `nfds` MUST be 1..PROC_HANDLE_MAX = 64. The `struct poll_waiter
// waiters[64]` array lives on this routine's kernel stack. specs/
// poll.tla `Register` ↔ the first scan; `CommitOrSleep` ↔ the flag-
// check + tsleep; `MakeReady` ↔ a producer's `poll_waiter_list_wake`.
s64 sys_poll_for_proc(struct Proc *p, struct pollfd *kfds, u64 nfds,
                      s32 timeout_ms);

// =============================================================================
// Diagnostics.
// =============================================================================

// Monotonic counter of poll calls that returned (including timeout
// returns at 0). Tests assert this increments to confirm the syscall
// path executed.
u64 poll_total_calls(void);

// Monotonic counter of poll calls that slept on tsleep. Distinguishes
// the fast path (any fd ready at first scan) from the slow path.
u64 poll_total_slept(void);

#endif // THYLACINE_POLL_H
