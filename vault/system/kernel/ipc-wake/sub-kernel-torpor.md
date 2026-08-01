---
id: sub-kernel-torpor
type: sub
parent: moc-kernel-ipc-wake
title: "torpor — the wait-on-address primitive (the futex)"
code: ["kernel/torpor.c", "kernel/include/thylacine/torpor.h"]
audit: hard
guarded-by: [inv-i9, inv-i24]
validated-by: [spec-tsleep, gate-smp]
locks: [lock-torpor, lock-rendez, lock-wait, lock-timerwait]
created: 2026-08-01
updated: 2026-08-01
---
## Purpose

Block a thread on a user-VA `u32` until the word may have changed;
wake blocked threads when it does. Thylacine's `futex(FUTEX_WAIT)` /
`futex(FUTEX_WAKE)` — the substrate under pouch's pthread
mutex/condvar, musl's `__futexwait`, the Go runtime's `futexsleep`,
and libthyla-rs `torpor::wait`. Two syscalls (`SYS_TORPOR_WAIT` = 39,
`SYS_TORPOR_WAKE` = 40) plus two kernel-only cascade walks the death
and job-control paths call.

Named for the marsupial deep-sleep state: metabolism parked on an
external trigger. Distinct from `_torpor()` (the CPU WFI halt loop) —
same metaphor, different scope.

## Contract

- `sys_torpor_wait_for_proc(p, addr_va, expected, timeout_us)` —
  compare `*addr_va` to `expected`; equal ⇒ register on the
  `(Proc, VA)` bucket and `tsleep` until a matching WAKE or the
  deadline; unequal ⇒ return `TORPOR_OK` at once. `timeout_us < 0`
  blocks indefinitely; `== 0` is a probe (registers, then times out
  immediately if still equal); `> TORPOR_MAX_TIMEOUT_US` (1 h) is
  `-EINVAL`. Returns 0 / `-EINVAL` / `-EFAULT` / `-ETIMEDOUT`
  (Linux-numeric, so pouch's `syscall_ret.c` decodes them as errno).
- **0 is deliberately ambiguous** — it means *woken* OR *value already
  differed* (Linux returns `EAGAIN` for the latter; v1.0 collapses
  both because every futex client re-checks its own predicate on
  return regardless).
- `sys_torpor_wake_for_proc(p, addr_va, count)` — wake up to `count`
  waiters keyed `(p, addr_va)`; returns the number **actually woken**
  ([[fnd-torpor8-r1-f1]]). Never reads the user word. `count == 0` is
  a literal no-op with **no barrier semantics** (torpor-8 F7).
  `UINT32_MAX` = wake-all (the broadcast shape).
- `torpor_wake_all_for_proc(p)` — the DEATH cascade's completing walk:
  wake every waiter of `p` on every address, so each returns to its
  EL0-return die-check. Called by `proc_group_terminate` after it
  publishes `group_exit_msg` ([[inv-i24]]).
- `torpor_stop_wake_all_for_proc(p)` — the STOP cascade's
  **non-completing** twin (PTY-4e / #19,
  [[chg-2026-07-18-19-stop-wake]]): wakes without setting `awoken`,
  so the woken thread's cond re-check fails, its `tsleep` re-loop hits
  the debug/job stop detour, and it parks **with the wait preserved**
  — on resume it re-registers with its original deadline. Using the
  completing walk here made every torpor-timed wait spuriously
  COMPLETE on fg-resume (a `/bin/sleep` "finished" instead of
  continuing) — the #19 root cause.
- `p` MUST equal `current_thread()->proc` (torpor-8 F4,
  extinction-asserted): the user-VA load walks the CURRENT thread's
  TTBR0, so a foreign `p` would silently read the wrong word from a
  coincidentally-mapped VA.

## Mechanism

One global `torpor_lock` guards a 64-bucket hash
(`hash(Proc*, VA>>3)`, power-of-two masked) of singly-linked
**stack-allocated** waiters. Each waiter carries its own private
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
   the under-lock fault a **blocking 9P read under the global
   `torpor_lock`** — a system-wide futex stall, permanent under a
   wedged FS. See [[seam-torpor-reclaim-uaccess]] for the obligation a
   future reclaim pass inherits.
3. **Lock-free mismatch return** (#343,
   [[chg-2026-07-04-torpor-lockfree]]): `prefault != expected` ⇒
   `TORPOR_OK` without ever taking `torpor_lock`. Sound because no
   waiter registers on this path — [[inv-i9]]'s window exists only
   between register and sleep, which only the equal path reaches. The
   measured motivation: 36.8 M of a go build's 67.7 M `torpor_wait`
   calls are this mismatch, all on ONE address (`sleepDummy`) — one
   bucket, so per-bucket sharding could never have helped; skipping
   the lock was the only fix. The mismatch return provides only plain
   single-copy-atomic load ordering (weaker than the old incidental
   acquire) — sound because every futex client orders its own data,
   the universal contract.
4. `spin_lock(&torpor_lock)`; authoritative reload; unequal ⇒ unlock,
   return 0 (the near-zero residue).
5. Link the waiter at the bucket head — published under the lock so
   WAKE's walk sees it.
6. **Post-register death re-check under `torpor_lock`**
   ([[inv-i24]]): `thread_die_pending(current)` after registering,
   under the SAME lock `torpor_wake_all_for_proc` walks with. Either
   we registered before the walk (it wakes us) or the walk preceded
   our lock-acquire (we observe the flag here and never sleep). LS-5c
   widened the predicate to death-OR-terminate-`interrupt`; for the
   interrupt leg this check is conservative-prompt only — the
   interrupt waker does not take `torpor_lock`, and its
   register-after-walk race is closed one layer down by `tsleep`'s
   register-then-observe under [[lock-wait]].
7. Unlock; `tsleep(&w.rendez, cond_awoken, &w, deadline)`.
8. Re-lock; unlink; scribble `w.rendez.waiter = NULL`; unlock. The
   unlink-on-every-exit is the stack-waiter lifetime invariant.
9. `TSLEEP_TIMEDOUT` ⇒ `-ETIMEDOUT`. `TSLEEP_INTR` is absorbed via
   fall-through to `TORPOR_OK` — deliberate and documented in-line:
   the return value is immaterial (the EL0-return die-check
   terminates the thread before it resumes userspace) and the waiter
   must NOT re-sleep (its bucket entry is already unlinked). torpor is
   the one blocking site that absorbs `*_INTR` by fall-through rather
   than a dedicated arm.

WAKE: lock; walk the one bucket; for each `(p, VA)` match with
`awoken == 0`: set `awoken = 1`, `wakeup(&w->rendez)`, count it only
if `wakeup` returned true (a waiter whose deadline already fired has
`r->waiter == NULL`; counting it would overstate — torpor-8 F1). The
walk leaves waiters linked; the owner unlinks itself.

## Data structures

`struct torpor_waiter` — 56 B, kernel-stack-only, `_Static_assert`ed
8-byte-aligned (torpor-8 F6): private `Rendez` + `proc` + `user_va` +
`next` + `awoken` + pad. `torpor_buckets[64]` — ~512 B BSS.
Not an ABI type.

## Concurrency

Lock order: `torpor_lock` → `w->rendez.lock` (WAKE side only — the
sleeper releases `torpor_lock` before `tsleep` takes the wait chain).
The under-lock reload can still demand-page in exactly one window — a
concurrent decommit between the pre-fault and the reload re-faults
into the **non-blocking** lazy-anon arm — so the edge
`torpor_lock → vma_lock → buddy` still exists, but only there, and it
never sleeps. A VMA teardown in the same window is a clean `-EFAULT`.

The known hazard, documented not fixed ([[fnd-torpor8-r1-f2]],
[[seam-torpor-lock-wake-spin]]): all three wake walks hold
`torpor_lock` across `wakeup()`, which can spin on the woken thread's
`on_cpu` while a peer CPU switches it out. Under heavy multi-Proc
contention this serializes all torpor traffic.

## Invariants enforced

- [[inv-i9]] specialized to wait-on-address — **prose-validated, not
  modelled**: no `specs/futex.tla` exists (dropped under the
  2026-05-23 spec-to-code suspension). The proof is the lock
  acquire/release pairing argument in `torpor.h`, covering (a)
  WAKE-before-WAIT (the load sees the store, no sleep), (b)
  WAIT-before-WAKE (registered before the walk), (c) timeout-vs-WAKE
  (the count stays honest). The layer below — the tsleep park itself —
  IS modelled ([[spec-tsleep]]).
- [[inv-i24]] — the death cascade's futex leg: step 6's
  register-then-observe under the walk's own lock.

## Error paths

`-EINVAL` before any state; `-EFAULT` from either load (pre-lock:
nothing held; under-lock: unlock first, no waiter registered);
`-ETIMEDOUT` after a clean unlink. Everything else returns 0.

## Performance

The #343 measurement is the load-bearing one: the mismatch fast path
removed the global-lock acquire from 54 % of all torpor calls in a
`go build`. The parking path is unchanged: one lock hold each side of
the sleep plus the tsleep machinery.

## Prosecution

- The lock-free mismatch return must never be extended to the EQUAL
  path — a stale `== expected` read falls through to the
  authoritative under-lock load by design.
- Step 6's die-check must stay AFTER the bucket link and UNDER
  `torpor_lock` — hoisting it above the link reopens the
  register-after-walk lost wake.
- The stop walk must never set `awoken` — a completing stop-wake is
  the #19 bug verbatim.
- Any new exit path from WAIT must unlink before the frame pops.
- The pre-fault must stay OUTSIDE the lock; any future
  page-eviction/reclaim pass must re-establish
  non-blocking-uaccess-under-lock ([[seam-torpor-reclaim-uaccess]]).

## Seams

[[seam-torpor-lock-wake-spin]] · [[seam-torpor-reclaim-uaccess]] ·
[[seam-torpor-cross-proc]] (per-Proc keying — no cross-Proc
shared-anon futex until Tier-2 burrows).

## Caveats

- `docs/reference/80-torpor.md` (absorbed) asserted the retired
  held-across-fault lock chain as a present-tense fact four lines
  above the paragraph recording its retirement, still carried "No
  `-EINTR` at v1.0 — notes/signals don't yet propagate" (three
  generations stale: #811, LS-5c, #19), and never mentioned either
  cascade walk.
- The buddy `struct page.refcount` trap does not apply here, but the
  same shape does: `awoken` is a wake LATCH, not a state machine — a
  reader inferring "sleeping" from `awoken == 0` is wrong for a
  waiter that has not yet slept or has timed out.

## Provenance

[[chg-2026-05-23-torpor]] (born, with its embedded audit
[[adt-torpor8-r1]]) → #809/#811 death integration →
[[chg-2026-06-10-ls5c-widen]] →
[[chg-2026-07-04-torpor-lockfree]] (R-5 pre-fault + #343 mismatch) →
[[chg-2026-07-18-19-stop-wake]] (the stop twin).
