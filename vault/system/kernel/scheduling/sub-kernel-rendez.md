---
id: sub-kernel-rendez
type: sub
parent: moc-kernel-scheduling
title: "The wait/wake primitive — Rendez, sleep, tsleep, wakeup"
code: ["kernel/sched.c", "kernel/include/thylacine/rendez.h"]
audit: hard
guarded-by: [inv-i9, inv-i8]
validated-by: [spec-scheduler, spec-tsleep, spec-death-wake, gate-smp]
locks: [lock-wait, lock-timerwait, lock-rendez]
created: 2026-08-01
updated: 2026-08-16
---
## Purpose

The one place a kernel thread blocks. Plan 9's Rendez: a waiter, a waker,
and a caller-supplied condition. Every higher wait in the tree — poll,
pipe, the 9P client, srvconn, the console, `wait_pid`, torpor — is built
on `sleep`/`tsleep` and inherits their properties, including the ones
that are not obvious.

## Contract

- `sleep(r, cond, arg)` blocks until `cond(arg)` is true. Returns
  `SLEEP_OK`, or `SLEEP_INTR` when the thread must **unwind**: its Proc
  is group-terminating, or a terminate-disposition `interrupt` is
  pending. A caller that receives `SLEEP_INTR` releases its locks, frees
  transient state, and returns; the Thread then dies at its EL0-return
  die-check. The value returned to userspace is immaterial — a flagged
  Thread never re-enters EL0.
- `tsleep(r, cond, arg, deadline_ns)` adds a deadline on the
  `timer_now_ns` timebase. Returns `TSLEEP_AWOKEN` / `TSLEEP_TIMEDOUT` /
  `TSLEEP_INTR`. **`cond` has precedence**: a wait satisfied exactly as
  the deadline lapses reports AWOKEN. `deadline_ns == 0` means "no
  deadline" and degrades to `sleep`.
- `wakeup(r)` wakes the at-most-one sleeper; a no-op if none. Returns
  whether it woke anyone. Safe from IRQ context.
- **The producer's obligation**: make `cond` true *before* calling
  `wakeup`. `cond` is evaluated under `r->lock`, so the producer's write
  must either hold `r->lock` or be followed by `wakeup(r)`, which takes
  it.
- `cond` must be side-effect-free and cheap: it is called repeatedly, and
  in `tsleep` it is called while three locks are held.
- **Single waiter.** At most one Thread per Rendez. A second sleeper is
  an extinction, not a queue — multi-waiter waits are built one layer up
  (`poll_waiter_list`).
- `tsleep`'s deadline is delivered off the 1 kHz tick, so it may
  overshoot by up to a tick. It is a coarse backstop, not a timer.

## Mechanism

**The core protocol** is check-under-lock, register-under-lock, then
drop and yield:

    lock(t->wait_lock);  lock(r->lock);
    while (!cond(arg)) {
        r->waiter = t;  t->rendez_blocked_on = r;  t->state = SLEEPING;
        ...death re-check...
        unlock(r->lock);  unlock(t->wait_lock);
        sched();                       /* prev is SLEEPING -> stays out of the tree */
        lock(t->wait_lock);  t->rendez_blocked_on = NULL;  lock(r->lock);
    }
    unlock(r->lock);  unlock_irqrestore(t->wait_lock);

The window between the unlock and `sched()` is the canonical wait/wake
race, and it is closed on the *waker's* side: `wake_rendez_waiter` spins
on `t->on_cpu` until the sleeper's previous CPU has finished switching it
out, so a waiter is never readied off a half-saved context.

**`wait_lock` is the outermost lock and carries the IRQ mask** for the
entire call, including across the `sched()` yields. It is taken before
`r->lock` (and before `g_timerwait.lock`) so that the group-terminate
cascade — which holds a peer's `wait_lock` while it reads
`rendez_blocked_on` and wakes the rendez — is serialized against this
thread's register-then-observe. It is emphatically **not** held across
`sched()`: a descheduled sleeper holding it would deadlock the cascade.

**Register-then-observe.** After registering and before yielding, the
sleeper re-checks `thread_die_pending(t)` *under `wait_lock`* — the same
lock both wakers take per peer. Either this thread registered before the
waker's walk (so the walk finds and wakes it), or the flag-set
happens-before this thread's `wait_lock` acquire (so the re-check sees
it). There is no third interleaving. On a hit it undoes the **full**
registration — rendez waiter, backref, and in `tsleep` the timer-wait
link — and returns INTR.

The same check repeats on the resume path as the *prompt* path: the
registered check would catch it on the next iteration anyway, but
returning immediately avoids a pointless loop.

**Who clears `rendez_blocked_on`.** Only the owning Thread, on its own
resume, under its own `wait_lock`. `wake_rendez_waiter` deliberately does
*not* clear it (#811): clearing under `r->lock` would race the cascade's
read under `wait_lock`. The owner is still SLEEPING when it is woken, so
the backref stays valid until it resumes.

**Two detours**, both inside the loop, both before registration:

- **The stop detour** (8c-2). If a stop is pending from either owner —
  the debugger or job control, via `proc_stop_requested`'s
  `debug | job` disjunction — the sleeper parks on its own
  `debug_rendez` until both clear, then re-loops and re-checks the
  *original* condition. The syscall re-blocks in place: no unwind, no
  restart. Gated `r != &t->debug_rendez` so the nested park cannot
  recurse, and gated on `t->proc` so a kernel thread is skipped.
- **The frame-atomic exception** (8c-3 / #90). The elected 9P reader is
  the one sleeper whose unwind is deferred. Mid-frame — `stop_no_park`
  set, `stop_unwinds` clear, meaning some bytes of the current frame are
  already consumed — it **blocks through** both a stop and a death:
  unwinding would discard the partial frame, and the survivor that takes
  over the reader role would then read the frame *tail* as a header and
  desync the shared byte stream. At a frame boundary (`got == 0`) it
  unwinds normally. Between frames the reader also *releases the role*
  rather than parking in place, because a parked reader freezes every
  survivor sharing the client.

  Death still wins over a stop at every branch; both now simply unwind at
  a boundary rather than immediately.

**`tsleep`'s third wake source.** A deadlined sleeper is also linked into
one global list, `g_timerwait`, and registered atomically with the rendez
under all three locks. `sched_tick` calls `timerwait_tick` on every fire,
which wakes expired sleepers **one at a time**: each iteration takes
`g_timerwait.lock`, finds one expired sleeper, unlinks and wakes it under
both locks, then *releases the global lock* before the next. Each wake is
still atomic, but a burst of simultaneous timeouts can no longer stall
every other CPU's tick behind one long hold (the P5-tsleep F6 fix). `now`
is sampled once so the set this pass wakes is fixed and the loop
terminates.

`timerwait_tick` **pre-filters on `on_cpu`**: a mid-switch sleeper is
skipped and caught by a later tick, so the wake never spins inside the
timer IRQ handler.

**`wakeup`'s lock order.** It takes `g_timerwait.lock` as the *outer*
lock even for a plain `sleep` waiter that is never on the list, because
it cannot know whether the waiter is deadlined until it holds `r->lock` —
by which point taking the global lock would invert the order. It releases
the global lock the moment the unlink is done, so the `on_cpu` spin and
the `ready()` run under `r->lock` alone.

## Data structures

`struct Rendez { spin_lock_t lock; struct Thread *waiter; }` — 16 bytes,
statically initializable (`RENDEZ_INIT`) or `rendez_init`'d, embedded
freely in other objects.

`g_timerwait { lock; head }` — one global doubly-linked list of deadlined
sleepers, threaded through `Thread.timerwait_next/prev`. One lock, not
per-CPU: deadlined waits are the cold path, the scan is O(timed
sleepers), and the global lock is what [[spec-tsleep]] verifies.
`timerwait_is_linked` uses the same three-way test as the run tree,
because a sole element has both links NULL.

Thread fields owned here: `rendez_blocked_on`, `sleep_deadline`,
`sleep_timedout`, `timerwait_next/prev`, `wait_lock`, `debug_rendez`,
`stop_no_park`, `stop_unwinds`, `stop_unwound`.

## Concurrency

The full chain, outermost first:

    lock-proc-table -> wait_lock -> g_timerwait.lock -> r->lock -> cs->lock

Every one of those edges is taken in that order at every site, and the
reverse of the middle edge (`r->lock` then `g_timerwait.lock`) is exactly
what `wakeup`'s outer acquire exists to avoid.

`timerwait_earliest_deadline` — read by the tickless idle path — is a
**leaf** acquisition of `g_timerwait.lock` alone, irqsave because the
timer IRQ's `timerwait_tick` takes the same lock and an IRQ landing
mid-hold would self-deadlock. Unlike `timerwait_tick` it does *not*
filter `on_cpu`: it reads deadlines and wakes nothing, and a mid-switch
sleeper's near deadline still needs covering.

## Invariants enforced

- **[[inv-i9]]** — no wakeup lost between the condition check and the
  sleep. This dossier is the primitive the invariant is stated about;
  [[sub-kernel-death]] holds the death-wake generalization, and
  [[spec-death-wake]] pins it.
- **[[inv-i8]]** — a woken thread is `ready()`'d, so it re-enters
  dispatch.

## Error paths

`sleep`/`tsleep` return `*_INTR`; everything else is an extinction:

| Condition | Where |
|---|---|
| NULL rendez / NULL cond | entry |
| no current thread; corrupted current | entry |
| a second sleeper on one Rendez | the registration guard |
| current is not RUNNING | the registration guard |
| current already blocked on a rendez | the registration guard |
| already on the timer-wait list | `tsleep` only |
| waker sees a corrupted waiter, a non-SLEEPING waiter, or a backref mismatch | `wakeup` |

The three `wakeup` checks are worth reading as a set: they assert the
waiter is intact, is actually asleep, and agrees with the Rendez about
which Rendez it is asleep on. A violation of any of them means the wait
state has been corrupted by someone else.

## Performance

- `sleep`'s fast path (condition already true) takes two locks and
  returns without any state transition.
- `timerwait_tick`'s rescan-from-head is O(n²) in the per-tick herd size.
  Bounded and cheap for a cold path; per-CPU sharding would make it O(n)
  and is an optimization, not a correctness need.
- `wakeup` holds the global timer lock for exactly one unlink.

## Prosecution

- **The unconditional `r->lock` acquire in `wakeup` is LOAD-BEARING**
  (PTY-4e R2). Even on the no-waiter path. It is the only ordering chain
  delivering a torpor poster's `awoken = 1` — written before the call —
  to a stop-parked waiter's resumed `tsleep` re-loop, whose `cond` read
  pairs with *this* release. A lockless `r->waiter == NULL` fast path
  here looks like free performance and reintroduces a lost wake on the
  preserved-wait path. This is the single most attractive-looking wrong
  change in the file.
- **`wake_rendez_waiter` must not clear `rendez_blocked_on`.** Clearing
  it there races the cascade's read (#811).
- **The register-then-observe must undo the FULL registration.** In
  `tsleep` that means the timer-wait link as well as the rendez waiter;
  leaving the link behind strands an entry the tick will later wake into
  a thread that is no longer sleeping.
- **The detour ordering is fixed**: death check precedes the stop check,
  the stop detour precedes `timerwait_link`, and the detour is gated
  against its own rendez. A stop-parked `tsleep` re-registers with its
  *original* deadline on resume, and a deadline that lapsed while stopped
  correctly reports TIMEDOUT — wall-clock advances while a thread is
  stopped, and that is the accepted freeze semantics.
- **The frame-atomic guard applies on BOTH paths** — the
  register-then-observe check and the prompt post-resume check. Guarding
  only the first silently defeats it on the very next wake.
- **`timerwait_tick`'s `on_cpu` pre-filter stays.** Removing it puts an
  unbounded spin inside a timer IRQ handler.
- **Single-waiter is enforced, not assumed.** Any new caller that could
  see two threads on one Rendez needs a `poll_waiter_list`, not a second
  sleeper — the extinction is an unprivileged panic if it is reachable
  from EL0.

## Seams

- [[seam-timerwait-sharding]] — the one global timer-wait lock.

## Caveats

- `SLEEP_INTR` **aliases**: it means "unwind", not "died". Since LS-5c it
  also covers a terminate-disposition `interrupt`, and since 8c-3 it also
  covers a stop-unwind — which is why the 9P client reads the separate,
  stable `stop_unwound` latch rather than re-reading `debug_stop_req`
  (which races an async resume).
- A caller that ignores `SLEEP_INTR` leaks whatever it was holding. The
  return is documented as ignorable *only* for callers with nothing to
  unwind.
- `tsleep`'s `deadline_ns` is absolute and the caller owns the overflow:
  a wrapped, now-past deadline times out at once, and a wrap to exactly 0
  reads as "no deadline".
- The single-waiter restriction is a special case of the multi-waiter
  spec, not a different protocol — the invariants carry over unchanged
  for a singleton-or-empty waiter set.

## Provenance

`sleep`/`wakeup` landed at P2-Bb ([[chg-2026-05-05-p2b-sched-dispatch]]);
`tsleep` and the timer-wait list at P5-tsleep
([[chg-2026-05-17-p5-tsleep]]). Universal death-interruptibility is
[[chg-2026-06-01-811-death-interruptible]]; the terminate-`interrupt`
widening rides [[arc-life-support]]; the stop detour and the frame-atomic
block-through are [[arc-go-ide]] and [[arc-pty]].

Absorbed `docs/reference/16-rendez.md` at [[chg-2026-08-01-sched-sweep]].

**2026-08-16: re-verified, no content owed.** `kernel/sched.c` moved ~48
lines since the last sweep and this dossier was flagged for it, but every
hunk landed in `ready`, `sched_arm_clear_on_cpu`, `sched_install_asid_ttbr0`
and `sched()` — the dispatch half of a file two dossiers share. **Churn is
per FILE; ownership is per SURFACE**, and the two do not line up whenever a
file carries more than one layer. The check was hunk-context against the
function set this dossier owns (`sleep`, `tsleep`, `wakeup`,
`wake_rendez_waiter`, `timerwait_*`); none was touched. The dispatch-side
changes are on [[sub-kernel-sched]] ([[chg-2026-08-16-sched-addrspace-install]]).
