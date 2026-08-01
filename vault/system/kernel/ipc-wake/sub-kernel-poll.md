---
id: sub-kernel-poll
type: sub
parent: moc-kernel-ipc-wake
title: "poll — one thread waiting on N readiness sources"
code: ["kernel/poll.c", "kernel/include/thylacine/poll.h"]
audit: hard
guarded-by: [inv-i9]
validated-by: [spec-poll, spec-tsleep, gate-smp]
locks: [lock-poll-list, lock-rendez, lock-wait, lock-timerwait]
created: 2026-08-01
updated: 2026-08-01
---
## Purpose

`poll(fds, nfds, timeout_ms)` parks the caller until one of N fds is
ready or the timeout lapses. A `Rendez` is single-waiter and a thread
can wait on only ONE, so poll does not make Rendez multi-waiter: the
poller sleeps on its OWN private stack Rendez via `tsleep`, and
installs a lightweight `poll_waiter` hook on each polled object's
embedded hook list. Producers walk the list at every readiness edge.
This is the primitive under every select/poll-shaped consumer — the
`/srv` servers, netd's event loop via the pouch 0018 translation, the
dev9p.poll bridge's userside.

## Contract

- `sys_poll_for_proc(p, kfds, nfds, timeout_ms)` → count of pollfds
  with `revents != 0`, or -1 (bad args). `timeout_ms < 0` blocks
  indefinitely, `== 0` is a non-blocking probe, `> 0` bounds the park.
- `nfds` ∈ [1, `POLL_MAX_NFDS` = 64]. **Deliberately decoupled from
  `PROC_HANDLE_MAX`** (now 256; [[chg-2026-06-24-355-poll-decouple]]):
  the frame stack-allocates `waiters[]` + `held[]` of this size, and
  sizing them to the fd-table bound would blow the kstack (~14 KiB at
  256). Lifting past 64 needs heap-backed arrays —
  [[seam-poll-heap-waiters]].
- Event bits are Linux-valued (`POLLIN` 0x001, `POLLOUT` 0x004, and
  output-only `POLLERR`/`POLLHUP`/`POLLNVAL`); `struct pollfd` is
  8 bytes, offset-pinned ABI.
- Per-fd semantics: negative fd or dead handle ⇒ `POLLNVAL` (which
  COUNTS as ready — POSIX); a Dev with no `.poll` slot ⇒ always-ready
  for the requested bits (the regular-file answer); a NULL-obj Spoor ⇒
  `POLLNVAL`, never always-ready (a buggy caller must not spin on fake
  readiness); `KOBJ_SRV` dispatches through `srv_handle_poll` (magic
  discriminates listener vs connection); every other kobj kind ⇒
  `POLLNVAL`. No `RIGHT_READ`/`RIGHT_WRITE` gate — polling asks about
  readiness, not access, and POSIX permits polling a write-only fd.
- The `Dev.poll` vtable op: `dev->poll(spoor, events, pw)` returns
  current revents and, iff `pw != NULL`, registers it — **atomically
  with the sample, under the object's own lock**. `pw == NULL` is
  sample-only (the post-wake re-scan and the probe path).

## Mechanism

1. **Register scan**: `dev->poll(c, events, &waiters[i])` per fd —
   [[spec-poll]]'s `Register`. Install-then-sample under the object
   lock means no readiness edge between the sample and the park can
   miss the hook.
2. Fast path: any ready, or `timeout_ms == 0` ⇒ jump to the sweep.
3. `tsleep` on the private Rendez with cond `any waiter.ready` — the
   cond reads `pw->ready` without object locks; sound because the
   producer writes it under the list lock and then `wakeup` takes the
   same rendez lock the cond runs under (release/acquire).
4. `TSLEEP_INTR` (#811 death/terminate) ⇒ skip the re-sample — the
   thread dies at its EL0-return check — but the sweep still runs:
   the hooks are stack memory and MUST be unlisted.
5. Re-sample every fd (`pw = NULL`), rebuild the count.
6. **The sweep, in load-bearing order**: unregister every hook
   (idempotent), THEN scribble `magic = 0` (reversed, a concurrent
   producer walk holding the list lock would extinct on the zeroed
   magic), THEN `handle_put` every retained ref (below) — only after
   no waiter references any object's list.

**The retain discipline** (RW-2 2C-F1, [[fnd-rw2-2cf1]]): a
registered hook lives on the OBJECT's embedded list across the whole
sleep, and a **sibling thread** sharing the handle table can close the
last handle mid-sleep — `spoor_clunk` frees the object and its
embedded list, leaving `pw->list` dangling and the unregister
spin-locking freed memory. So the register scan RETAINS the #844
`handle_get` obj ref whenever it actually registered
(`pw->list != NULL`), transferring it to `held[i]`; the sweep releases
all of them after unregistering. The retain is transitively
sufficient for both real registering paths (pipe ring and devsrv
connection — each frees its embedded list only at the Spoor's last
clunk). The **listener** retain is INERT ([[fnd-rw2-r2poll-f1]],
[[seam-poll-srv-registry-retain]]): `handle_acquire_obj` is a no-op
for `KObj_Srv`, so listener-poll lifetime rests solely on the boot
registry being immortal.

## Data structures

`struct poll_waiter` — magic ("POLW") + `ready` + private-rendez
backref + `list` backpointer (set at register, cleared at unregister;
the sweep's route home without a Dev vtable op) + `next`.
`struct poll_waiter_list` — a spinlock + singly-linked head, embedded
in the pollable object. `struct pollfd` — the 8-byte pinned ABI.
Diagnostics: `poll_total_calls` / `poll_total_slept` — the latter
counts "committed to the slow path", not "actually parked" (a
producer racing register-to-tsleep still increments it).

## Concurrency

Producer order inside `poll_waiter_list_wake`, load-bearing: write
`pw->ready = true` FIRST, then `wakeup(pw->rendez)` — the rendez-lock
release/acquire pair carries the flag to the woken cond. The full
chain: object lock → list lock → (the wake enters the wait chain:
`g_timerwait` → rendez → runq). Unregister takes ONLY the list lock —
that asymmetry is what lets the sweep run without deadlocking against
a producer holding the object lock. The list lock is non-irqsave; no
IRQ handler enters it (the console's IRQ-side readiness is relayed to
process context precisely to honor this — the cons_poll design).

Double-register, a stale magic mid-walk, or `pw->list` set but absent
from the list are all extinctions — corruption, not recoverable
states.

## Invariants enforced

[[inv-i9]] across N fds — `NoMissedPoll` in [[spec-poll]]: a poller
is never left asleep while a registered fd is ready. The single-fd
core is [[spec-scheduler]]'s NoMissedWakeup; the deadline leg is
[[spec-tsleep]]; poll adds the N-sources-behind-N-locks composition,
with the hook flag as the cross-lock handoff. `NoStaleHook` pins the
sweep.

## Error paths

-1 for `p`/`kfds` NULL, `nfds` 0 or > 64. Per-fd failures are
`POLLNVAL` in revents, never a call failure. The user-VA wrapper
(`sys_poll_handler`) validates the whole array range before copy-in
and scrubs partially-written revents on a writeback fault.

## Performance

O(nfds) lock pairs per scan; no allocation anywhere — `waiters[]`
(~2 KiB) + `held[]` (~1.5 KiB) are frame-resident, which is exactly
why `POLL_MAX_NFDS` is a frame bound, not an fd-table bound.

## Prosecution

- The sweep's three phases must keep their order: unregister →
  scribble → put. Each inversion is a distinct UAF/extinction.
- A new registering path must either be reachable from a retained
  handle kind or add its own ref — the inert-listener caveat is the
  worked counterexample.
- `poll_scan_one`'s retain condition is `pw->list != NULL` AFTER the
  dev call — a Dev that registers conditionally is still covered;
  a Dev that registers on a DIFFERENT list than the one it samples
  breaks the atomicity argument.
- The INTR arm must never skip the sweep.

## Seams

[[seam-poll-srv-registry-retain]] · [[seam-poll-heap-waiters]].

## Caveats

- `docs/reference/72-poll.md` (absorbed) still asserted "poll does
  NOT take a reference on the polled Spoors … no such path exists at
  v1.0" — a soundness argument inverted by the multi-thread lift and
  closed by the retain it says doesn't exist — and sized `waiters[]`
  by `PROC_HANDLE_MAX` throughout, the identity whose restoration
  would overflow the kstack. `syscall.h`'s SYS_POLL comment still
  names `PROC_HANDLE_MAX = 64` as the bound; the handler correctly
  uses `POLL_MAX_NFDS`.
- P5-poll F3 ([[fnd-poll-r1-f3]]) is this surface's origin story for
  the batch-8 lesson: a P1 "doc-fixed" by documenting a
  single-thread precondition that a later lift silently voided.

## Provenance

[[chg-2026-05-20-p5-poll]] (mechanism + devpipe + devsrv `.poll` +
the close [[adt-poll-r1]]) → #811 INTR arm →
[[chg-2026-06-10-rw2-poll-retain]] (the retain) → #844 snapshot API →
net-6b-2b `poll_waiter_list_empty` (the dev9p GC's atomic
emptiness probe) → [[chg-2026-06-24-355-poll-decouple]].
