---
id: sub-kernel-torpor
type: sub
parent: moc-kernel-ipc-wake
title: "torpor — the wait-on-address primitive (the futex)"
code: ["kernel/torpor.c", "kernel/include/thylacine/torpor.h"]
audit: hard
guarded-by: [inv-i9, inv-i24]
validated-by: [spec-torpor, spec-tsleep, gate-smp]
locks: [lock-torpor, lock-rendez, lock-wait, lock-timerwait]
created: 2026-08-01
updated: 2026-11-04
---
## Purpose

Block a thread on a user-VA `u32` until the word may have changed;
wake blocked threads when it does. Thylacine's `futex(FUTEX_WAIT)` /
`futex(FUTEX_WAKE)` — the substrate under pouch's pthread
mutex/condvar, musl's `__futexwait`, the Go runtime's `futexsleep`,
and libthyla-rs `torpor::wait`. Two syscalls (`SYS_TORPOR_WAIT` = 39,
`SYS_TORPOR_WAKE` = 40). The death and job-control cascades have no walk here: a
waiter sleeps on a private `Rendez` that `rendez_blocked_on` names, and they reach it
as they reach any sleep.

Named for the marsupial deep-sleep state: metabolism parked on an
external trigger. Distinct from `_torpor()` (the CPU WFI halt loop) —
same metaphor, different scope.

## Contract

- `sys_torpor_wait_for_proc(p, addr_va, expected, timeout_us)` —
  compare `*addr_va` to `expected`; equal ⇒ register on the
  `(AddrSpace, VA)` bucket and `tsleep` until a matching WAKE or the
  deadline; unequal ⇒ return `TORPOR_OK` at once. `timeout_us < 0`
  blocks indefinitely; `== 0` is a probe (registers, then times out
  immediately if still equal); `> TORPOR_MAX_TIMEOUT_US` (1 h) is
  `-EINVAL`. Returns 0 / `-EINVAL` / `-EFAULT` / `-ETIMEDOUT` /
  `-EINTR` (Linux-numeric, so pouch's `syscall_ret.c` decodes them as
  errno). `-EINTR` (`TORPOR_ERR_EINTR`) means a caught note ended the
  wait before any WAKE counted it (ARCH 8.8.3). A Linux `FUTEX_WAIT`, a call on
  signal(7)'s list, can receive it, and since item 11c so can a native `torpor_wait`
  where a table governs. Under `SA_RESTART` the kernel re-issues one with no deadline
  (`regs[2] < 0`) and answers `-EINTR` for one with a deadline.
- **0 is deliberately ambiguous** — it means *woken* OR *value already
  differed* (Linux returns `EAGAIN` for the latter; v1.0 collapses
  both because every futex client re-checks its own predicate on
  return regardless).
- `sys_torpor_wake_for_proc(p, addr_va, count)` — wake up to `count`
  waiters keyed `(p->as, addr_va)`, oldest first; returns the number **actually woken**
  ([[fnd-torpor8-r1-f1]]). Never reads the user word. `count == 0` is
  a literal no-op with **no barrier semantics** (torpor-8 F7).
  `UINT32_MAX` = wake-all (the broadcast shape).
- `p` MUST equal `current_thread()->proc` (torpor-8 F4,
  extinction-asserted): the user-VA load walks the CURRENT thread's
  TTBR0, so a foreign `p` would silently read the wrong word from a
  coincidentally-mapped VA.

## Mechanism

A table of 256 buckets, each a lock and a list on its own cache line, hashed on
`(AddrSpace*, VA)`, of doubly-linked **stack-allocated** waiters, oldest first. The key
is the space so that Procs sharing one wake each other. Each waiter carries its own private
`Rendez`; `tsleep` does the actual parking with `awoken` as the cond.

WAIT, in order (`kernel/torpor.c::sys_torpor_wait_for_proc`):

1. Validate: NULL/unaligned/`>= UACCESS_USER_VA_TOP`/straddles-top ⇒
   `-EINVAL` (the 4-byte alignment check is load-bearing — the uaccess
   fixup table catches translation/permission faults only; an
   unaligned LDR would alignment-fault into the unclassified-extinction
   path; torpor-8 F10).
2. **Pre-fault, no lock** (REVENANT R-5 F1): `uaccess_load_u32` once,
   outside the lock, so the page is resident before step 5's load.
   Pre-REVENANT every user page was eager-anon and an under-lock fault
   could alloc-in-place but never SLEEP; file-backed text pages made
   the under-lock fault a **blocking 9P read under the lock**, which
   `sched()` answers with an extinction. The pre-fault narrows that to a
   window, and step 5 closes the window.
3. **Lock-free mismatch return** (#343,
   [[chg-2026-07-04-torpor-lockfree]]): `prefault != expected` ⇒
   `TORPOR_OK` without ever taking a lock. Sound because no
   waiter registers on this path — [[inv-i9]]'s window exists only
   between register and sleep, which only the equal path reaches. The
   measured motivation: 36.8 M of a go build's 67.7 M `torpor_wait`
   calls are this mismatch, all on ONE address (`sleepDummy`) — one
   bucket, so per-bucket sharding could never have helped; skipping
   the lock was the only fix. The mismatch return provides only plain
   single-copy-atomic load ordering (weaker than the old incidental
   acquire) — sound because every futex client orders its own data,
   the universal contract.
4. Take the bucket's lock; authoritative reload; unequal ⇒ unlock,
   return 0 (the near-zero residue).
5. Append the waiter to the bucket — published under the lock so
   WAKE's walk sees it.
6. Unlock; `tsleep_noteintr(&w.rendez, cond_awoken, &w, deadline)`. A death or stop
   published at any point up to here is `tsleep`'s to see, in its
   register-then-observe under [[lock-wait]] ([[inv-i24]]): the bucket's lock has no part
   in it. A stop's wake sets no `awoken`, so the waiter's `tsleep` re-loop takes the stop
   detour and parks **with the wait preserved**, and on resume re-registers with its
   original deadline. A completing wake there made a `/bin/sleep` "finish" on fg-resume
   (PTY-4e / #19, [[chg-2026-07-18-19-stop-wake]]).
7. Re-lock; unlink if still linked; unlock; wait for `wake_held` to clear; scribble
   `w.rendez.waiter = NULL`. The unlink-on-every-exit and the wait are the stack-waiter
   lifetime invariant ([[spec-torpor]] `NoUseAfterFree`).
8. `TSLEEP_TIMEDOUT` ⇒ `-ETIMEDOUT`; `TSLEEP_NOTEINTR` ⇒ `-EINTR`,
   returned without sleeping again. `TSLEEP_INTR` is absorbed via
   fall-through to `TORPOR_OK` — deliberate and documented in-line:
   the return value is immaterial (the EL0-return die-check
   terminates the thread before it resumes userspace) and the waiter
   must NOT re-sleep (its bucket entry is already unlinked). torpor is
   the one blocking site that absorbs `*_INTR` by fall-through rather
   than a dedicated arm.

WAKE: lock; walk the one bucket from the oldest; for each `(as, VA)` match with
`awoken == 0`: under its rendez's lock, set `awoken = 1` and read whether it is asleep;
unlink it. One asleep is counted (torpor-8 F1) and joins a batch, with `wake_held` set. One
not asleep, whose deadline already fired or a stop has parked, is not counted, and the walk
goes on to the next, up to `count`. One pass under one hold, so a thread re-waiting in a loop
cannot keep it going. Unlock, then `wakeup` the batch, clearing each `wake_held` after: the
woken waiter's first act is to take this lock, and `wakeup` can spin on `on_cpu`. The waker is
non-preemptible from the first mark to the last clear, so a waiter's wait for the clear is
bounded by a running thread.

**A counted wake is never lost to EINTR.** tsleep's caught arm runs
after its cond test, both under the Rendez lock, and WAKE sets `awoken`
under that lock, and counts a waiter only when it found it asleep then. So a
waiter WAKE counted resumes, reads `awoken` set, and returns 0, whatever note is
pending or whichever of the deadline and the wake reached it first. A waiter the note
reached first is either off its rendez when WAKE marks it (the count skips it, and the
walk goes on to the next waiter) or returns 0 itself if the mark lands before it re-tests
the cond — a spurious wakeup, which every futex client already tolerates.

## Data structures

`struct torpor_waiter` — kernel-stack-only, `_Static_assert`ed
8-byte-aligned (torpor-8 F6): private `Rendez` + `as` + `user_va` +
`next` + `prev` + `linked` + `awoken` + `wake_held` + pad. `torpor_buckets[256]`,
64 B each — 16 KiB BSS. Not an ABI type.

## Concurrency

Lock order: a bucket's lock → `w->rendez.lock` (WAKE side only — the
sleeper releases the bucket's lock before `tsleep` takes the wait chain). Bucket locks
never nest. A key is never dangling: every thread of a Proc is gone before an exec swaps
its space, so a waiter's space outlives its record.
The under-lock reload can still fault, in the window between the pre-fault and
itself, and EL0 can hold that window open by keeping the bucket busy. A concurrent
decommit re-faults into the **non-blocking** lazy-anon arm, so the edge bucket lock →
`vma_lock` → buddy exists, only there, and never sleeps. A VMA teardown fails the load.
A `MAP_FIXED` that puts an unfaulted file page there reaches the one arm that sleeps,
which refuses a faulter holding a counted lock ([[sub-kernel-fault]]), so that load
fails too. Either failure is a spurious return, not `-EFAULT`: the address was good a
moment ago, the caller re-evaluates, and its next call faults the word in or answers
`-EFAULT` from the pre-fault. Regression: `torpor.word_replaced_under_the_wait`.

WAKE holds no bucket across `wakeup()`, which can spin on the woken thread's `on_cpu`
([[fnd-torpor8-r1-f2]]).

## Invariants enforced

- [[inv-i9]] specialized to wait-on-address — modelled by [[spec-torpor]], the rendez's
  lock from the cond through registering included (`MarkedSleeperIsHeld`), with the lock
  acquire/release pairing argument in `torpor.h`, covering (a)
  WAKE-before-WAIT (the load sees the store, no sleep), (b)
  WAIT-before-WAKE (registered before the walk), (c) timeout-vs-WAKE
  (the count stays honest). The layer below — the tsleep park itself —
  is [[spec-tsleep]].
- [[inv-i24]] — the death cascade's futex leg is `tsleep`'s own: step 6.

## Error paths

`-EINVAL` before any state; `-EFAULT` from either load (pre-lock:
nothing held; under-lock: unlock first, no waiter registered);
`-ETIMEDOUT` or `-EINTR` after a clean unlink. Everything else
returns 0.

## Performance

The #343 measurement is the load-bearing one: the mismatch fast path
removed a lock acquire from 54 % of all torpor calls in a
`go build`. The parking path takes one bucket lock each side of
the sleep plus the tsleep machinery.

## Prosecution

- The lock-free mismatch return must never be extended to the EQUAL
  path — a stale `== expected` read falls through to the
  authoritative under-lock load by design.
- A stop's wake must never set `awoken` — a completing stop-wake is
  the #19 bug verbatim.
- The caught arm must stay after tsleep's cond test, and WAKE must count
  only a waiter its mark found asleep: either change returns
  `-EINTR` for a wake the walk counted, and the futex loses it.
- A mark must take the rendez's lock. The waiter holds it from reading the cond through
  registering, and a mark landing between finds nobody asleep, so the wake counts nothing
  and the waiter sleeps on a mark it has missed. Regression:
  `torpor.mark_takes_the_rendez_lock`, whose stall sits in that window.
- A wake's batch reaches records past the lock. `wake_held` set under the lock, the waiter's
  wait for its clear, and `next` read before the clear are one mechanism; none is optional.
- The mark sets `awoken` under the rendez's lock. A later `wakeup` used to carry it to a waiter
  parked by a stop (PTY-4e R2), and an uncounted mark now has no wakeup, so that lock is the carry.
- Any new exit path from WAIT must unlink before the frame pops.
- The pre-fault must stay OUTSIDE the lock. The load under the lock rests on the fault
  arm's refusal, so a future arm that sleeps (a swap-in) must honour it too.

## Seams

[[seam-torpor-reclaim-uaccess]] ·
[[seam-torpor-cross-proc]] (the key is the space, so Procs sharing a page at their own
VAs have no futex until Tier-2 burrows).

## Caveats

- `docs/reference/80-torpor.md` (absorbed) asserted the retired
  held-across-fault lock chain as a present-tense fact four lines
  above the paragraph recording its retirement, still carried "No
  `-EINTR` at v1.0 — notes/signals don't yet propagate" (stale since
  #811, LS-5c and #19, and simply wrong since
  [[chg-2026-10-05-signal7-list]], which made a Linux futex wait return
  `-EINTR`), and never mentioned the cascade walks it had then.
- The buddy `struct page.refcount` trap does not apply here, but the
  same shape does: `awoken` is a wake LATCH, not a state machine — a
  reader inferring "sleeping" from `awoken == 0` is wrong for a
  waiter that has not yet slept or has timed out.

## Provenance

[[chg-2026-05-23-torpor]] (born, with its embedded audit
[[adt-torpor8-r1]]) → #809/#811 death integration →
[[chg-2026-06-10-ls5c-widen]] →
[[chg-2026-07-04-torpor-lockfree]] (R-5 pre-fault + #343 mismatch) →
[[chg-2026-07-18-19-stop-wake]] (the stop twin) →
[[chg-2026-10-05-signal7-list]] (a caught note ends a Linux futex wait:
`TORPOR_ERR_EINTR`).
